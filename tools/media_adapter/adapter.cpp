#include "descriptor_bridge.hpp"
#include "tgl_capability_adapter.hpp"
#include "native_return_observer.hpp"
#include "owned_resource.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <array>
#include <cstdlib>

extern "C" __attribute__((visibility("default")))
int NGRN_ObservedCreateContexts(void *context, const void *input);

namespace {
constexpr const char *nativePath = "/Library/Extensions/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsVADriver";
constexpr uint8_t expectedHash[32] = {
    0xd2,0x65,0xf2,0x03,0x81,0x35,0xc2,0xc5,0xc3,0x64,0x9b,0x62,0x1d,0x9a,0xea,0x48,
    0xcf,0x1c,0xb7,0x4a,0x1f,0xb3,0x60,0x69,0xbb,0x1b,0x35,0x62,0xb3,0x75,0x01,0x29};
using Create = int (*)(void **, const void *, AvdMetadata *);
using Destroy = int (*)(void *);
struct Library { void *handle = nullptr; Create create = nullptr; Destroy destroy = nullptr; uintptr_t resourceShared=0; };
Library library;
std::once_flag loadOnce;
std::once_flag ownedImageOnce;
void* ownedImageHandle=nullptr; // Deliberately retained until process exit.
std::mutex registryLock;
// call_once may throw system_error; factory's existing catch must handle it.
bool retainOwnedHookImage() {
    std::call_once(ownedImageOnce,[]() noexcept {
        Dl_info image{};
        const auto hook=reinterpret_cast<void*>(&NGRN_ObservedCreateContexts);
        if(!dladdr(hook,&image) || !image.dli_fbase || !image.dli_fname)return;
        // Pin only the already-loaded image. Do not load an alternate bundle,
        // change signing policy or depend on factory-table lifetime.
        void* handle=dlopen(image.dli_fname,RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
        if(!handle)return;
        if(dlsym(handle,"NGRN_ObservedCreateContexts")!=hook) {dlclose(handle);return;}
        ownedImageHandle=handle;
    });
    return ownedImageHandle!=nullptr;
}
bool readMemory(uintptr_t address, void *output, size_t length) noexcept;
bool executableMemory(uintptr_t address,size_t length) noexcept;
bool writableDataMemory(uintptr_t address,size_t length) noexcept;
struct NativeDataPointerWriter {
    bool operator()(uintptr_t address,uintptr_t value) const noexcept {
        // Caller holds exact owner/physical exclusive admission. A failed write
        // is ambiguous; the owner retains its table and dependency, not rollback.
        if(address%alignof(uintptr_t) || !writableDataMemory(address,sizeof(value)))return false;
        return mach_vm_write(mach_task_self(),address,
            reinterpret_cast<vm_offset_t>(&value),sizeof(value))==KERN_SUCCESS;
    }
};
struct NativeDataAdmission {
    bool operator()(uintptr_t address,size_t length) const noexcept {
        return writableDataMemory(address,length);
    }
};
struct NativeTglImageAdmission {
    uintptr_t image=0;
    bool operator()(uintptr_t candidate) const noexcept {return image && candidate==image;}
};
using NativeCodecOwner=TglOwnedNativeCodecContextOwner<TglOwnedPhysicalDependencyLease,
    decltype(&readMemory),NativeDataPointerWriter,NativeDataAdmission,
    NativeTglImageAdmission,decltype(&executableMemory)>;
struct NativeCodecBackend {
    using Registry=TglOwnedContextOwnerRegistry<NativeCodecOwner,64>;
    using Batch=TglNativeCreatedContextBatch<16>;
    struct Creation {
        uintptr_t image=0,physical=0;uint64_t generation=0;
        Batch batch{};
        std::array<NativeCodecOwner*,16> candidates{};
        bool used=false,adopted=false;
    };
    struct Ledger {
        std::atomic_flag gate=ATOMIC_FLAG_INIT;
        std::array<Creation,64> records{};
    };
    static Ledger& ledger() noexcept {static Ledger value;return value;}
    static Registry& owners() noexcept {
        // Registry destruction does not delete unresolved native owners. Keep
        // its pin counters alive as long as process-owned frames can exist.
        static Registry registry;
        return registry;
    }
    template<class Scope>
    static int adopt(Scope& scope,uintptr_t physical,const Batch& batch) noexcept {
        if(!batch.wellFormed() || !scope.matches(scope.image(),physical,scope.generation()))return 5;
        auto& storage=ledger();
        if(storage.gate.test_and_set(std::memory_order_acquire))return 5;
        struct Unlock {std::atomic_flag& gate;~Unlock(){gate.clear(std::memory_order_release);}} unlock{storage.gate};
        Creation* selected=nullptr;
        for(auto& record:storage.records) {
            // No retry may overwrite partially adopted native state. Native
            // physical mutation failure is quarantined by the caller's gate.
            if(record.used && record.image==scope.image() && record.physical==physical &&
                record.generation==scope.generation()) {
                if(!record.adopted)return 5;
                // Successful prior batches may coexist, but never alias an
                // existing context or erase that batch's deletion obligations.
                if(batch.overlaps(record.batch))return 5;
            }
            if(!record.used && !selected)selected=&record;
        }
        if(!selected)return 5;
        selected->used=true;selected->image=scope.image();selected->physical=physical;
        selected->generation=scope.generation();selected->batch=batch;
        // Store candidates BEFORE leaving this callback, including any partial
        // failure. No Entry/stack ledger lifetime and no synthetic deletion.
        const int result=tglAdoptNativeCodecContextBatch(scope,physical,batch,owners(),selected->candidates,
            &readMemory,NativeDataPointerWriter{},NativeDataAdmission{},
            NativeTglImageAdmission{scope.image()},&executableMemory);
        selected->adopted=result==0;return result;
    }
    // Called ONLY by coordinator after native removal returned success, under
    // the same empty-frame physical mutation gate. Never inspect freed objects.
    template<class SuccessProof>
    static int reconcile(uintptr_t image,uintptr_t physical,uint64_t generation,
        const TglNativeRemovedContextBatch<16>& removed,SuccessProof proof) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<bool,SuccessProof,uintptr_t,uintptr_t,uint64_t,uintptr_t>);
        if constexpr(std::is_pointer_v<SuccessProof>) {if(!proof)return 5;}
        if(!image || !physical || !generation || !removed.valid || removed.count>16)return 5;
        auto& storage=ledger();
        if(storage.gate.test_and_set(std::memory_order_acquire))return 5;
        struct Unlock {std::atomic_flag& gate;~Unlock(){gate.clear(std::memory_order_release);}} unlock{storage.gate};
        return tglReconcileCodecCreationLedger(storage.records,image,physical,generation,removed,[&]() noexcept -> int {
            return owners().reconcileSuccessfulRemoval(image,physical,generation,removed,
                [&](NativeCodecOwner& owner) noexcept -> int {return owner.confirmNativeRemoval(proof)?0:5;});
        });
    }
};
// Compile the real backend body even before the lifecycle callback is enabled;
// this is a build check, not invocation or a synthetic successful-removal proof.
using NativeRemovalProof=bool(*)(uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;
template int NativeCodecBackend::reconcile<NativeRemovalProof>(uintptr_t,uintptr_t,uint64_t,
    const TglNativeRemovedContextBatch<16>&,NativeRemovalProof) noexcept;
struct NativeRendererRetirementAuthority {
    void* scope=nullptr;
    int(*admit)(void*,void*,uint64_t,size_t) noexcept=nullptr;
    int operator()(void* child,uint64_t generation,size_t index) const noexcept {
        // Enclosing creator supplies an authenticated exclusive lifetime scope,
        // including no pending work and restored descriptor borrows. No default
        // success, native-return inference, or Entry lifetime capture.
        return scope && admit?admit(scope,child,generation,index):5;
    }
};
using NativeRendererBacking=TglNativeVeboxVpFrameBacking<TglDarwinStmmSurfaceApi>;
using NativeRendererCleanupContext=TglOwnedVeboxVpCleanupContext<NativeRendererBacking,NativeRendererRetirementAuthority>;
using NativeRendererCleanup=TglOwnedVeboxVpCleanupCallback<NativeRendererCleanupContext>;
using NativeRendererCpu=TglNativeVeboxRendererCpuStorage<TglDarwinStmmSurfaceApi,NativeRendererCleanup>;
static_assert(sizeof(NativeRendererCpu)>0 && !std::is_copy_constructible_v<NativeRendererCpu>);
class NativeRendererCpuAssembly {
    static int scopedRetirement(void* storage,void* child,uint64_t generation,size_t index) noexcept {
        auto* context=TglOwnedScopedContext<TglOwnedVphalRetirementContext,TglOwnedVphalRetirementDomain>::get(storage);
        return context && context->scope && context->admit?
            context->admit(context->scope,child,generation,index):5;
    }
    // Value-owned authority outlives contexts, which outlive CPU/child teardown.
    // The opaque key is THIS assembly, never a borrowed scope pointee. Actual
    // authority is resolved only inside the creator's synchronous TLS scope.
    NativeRendererRetirementAuthority authority_;
    NativeRendererCleanupContext firstCleanup_,secondCleanup_;
    NativeRendererCpu cpu_;
    bool cleanupBound_=false;
public:
    NativeRendererCpuAssembly(uintptr_t image,
        const TglNativeVphalResourceEnvironment& environment)
        :authority_{this,&scopedRetirement},firstCleanup_(authority_),secondCleanup_(authority_),
         cpu_(image,&readMemory,NativeTglImageAdmission{image},&executableMemory,
            environment.resources,environment.pageSize,environment.fill,
            std::array<TglNativeTrackedCpuInvoker,2>{},
            std::array<NativeRendererCleanup,2>{{{&firstCleanup_},{&secondCleanup_}}},
            std::array<TglNativeTrackedCpuInvoker,2>{},std::array<TglNativeSfcLifetimeInvoker,2>{},
            std::array<TglNativeVeboxDestructorInvoker,2>{}) {}
    NativeRendererCpuAssembly(const NativeRendererCpuAssembly&)=delete;
    NativeRendererCpuAssembly& operator=(const NativeRendererCpuAssembly&)=delete;
    auto& cpu() noexcept {return cpu_;}
    auto& backing() noexcept {return cpu_.backing();}
    auto& children() noexcept {return cpu_.children();}
    bool bindCleanupContexts() noexcept {
        if(cleanupBound_)return false;
        auto& first=children().first().owner();auto& second=children().second().owner();
        if(!firstCleanup_.bind(backing(),first,first.constructedBase(),first.allocationGeneration(),0) ||
            !secondCleanup_.bind(backing(),second,second.constructedBase(),second.allocationGeneration(),1))return false;
        cleanupBound_=true;return true;
    }
    // No implicit retirement here. Existing VPHAL owner must retain this WHOLE
    // allocation on any partial construction/cleanup/native teardown refusal.
};
struct NativeRendererChildAdmission {
    void* scope=nullptr;
    bool(*admit)(void*,void*,uint64_t,uintptr_t,uintptr_t) noexcept=nullptr;
    bool operator()(void* renderer,uint64_t generation,uintptr_t first,uintptr_t second) const noexcept {
        return scope && admit && renderer && generation && first && second && first!=second &&
            admit(scope,renderer,generation,first,second);
    }
};
struct NativeVphalDestructorInvoker {
    void operator()(uintptr_t entry,uintptr_t object) const noexcept {
        // Exact saved native destructor, qualified by the lifetime table;
        // there is no status return and no substituted native counted delete.
        reinterpret_cast<void(*)(void*)>(entry)(reinterpret_cast<void*>(object));
    }
};
using NativeVphalOwner=TglOwnedVphalRendererOwner<NativeRendererCpuAssembly,TglOwnedPhysicalDependencyLease,
    decltype(&readMemory),NativeDataPointerWriter,NativeDataAdmission,NativeRendererChildAdmission,
    NativeVphalDestructorInvoker>;
static_assert(!std::is_copy_constructible_v<NativeVphalOwner>);
struct Entry {
    using CodecOwner=NativeCodecOwner;
    // Factory table lifetime only, not the native physical context lifetime.
    // Exact TGL table slots: [0] factory data, [1] 0x135d9 physical init
    // (void **output, const void *input), [2] 0x13c8c physical destroy,
    // [3] 0x13e43 context creation (physical context, input).
    // Init publishes its 0xaac0 allocation only at 0x13ae9 after setup;
    // context creation consumes physical+0x68/+0x78 at 0x13e78/0x13e83.
    // Do not attach frame owners here: destroy can partially tear down the
    // physical context before returning failure (0x13cc3..0x13cef).
    AvdDescriptorBridge bridge;
    TglNativePhysicalCallbackBinding physical;
    TglNativeResourceCfTypeBinding resourceClass;
    // Qualified allocator entries only. A frontend must own its backend beside
    // its DN owner; borrowing a backend from this factory-lifetime Entry would
    // leave a dangling reference when the native factory table is destroyed.
    TglTrackedCpuBinding nativeCpu;
    using SurfaceApi=TglDarwinStmmSurfaceApi;
    template<class ParentLease>
    bool rendererResourceEnvironment(const TglNativeBorrowedVphalMhwLease<ParentLease>& parent,
        TglNativeVphalResourceEnvironment& output) const noexcept {
        output={};
        const auto* view=parent.view();
        if(!nativeCpu.image || !view || view->renderer.image!=nativeCpu.image)return false;
        // Same qualified native image and parent-pinned OS/MHW roots. This
        // returns value bindings only; caller must retain parent through use.
        return output.resolve(parent,&readMemory,NativeTglImageAdmission{nativeCpu.image},&executableMemory);
    }
    auto frontendCpuEnvironment() const noexcept {
        // Entirely value-owned: no Entry pointer/reference escapes into frames.
        return TglNativeFrontendCpuEnvironment<decltype(&readMemory),NativeTglImageAdmission,
            decltype(&executableMemory)>{nativeCpu.image,readMemory,NativeTglImageAdmission{nativeCpu.image},
                executableMemory,TglNativeTrackedCpuInvoker{}};
    }
    bool expectedFrontendTypes(TglNativeFrontendCfTypes& output) const noexcept {
        return tglReadNativeFrontendCfTypes(resourceClass,readMemory,
            []() noexcept -> uintptr_t {return TglDarwinTypedCfReferenceApi{}.surfaceType();},output);
    }
    std::array<uintptr_t, 17> callbacks{};
    void *native = nullptr;
};
std::map<void *, std::unique_ptr<Entry>> registry;
std::atomic<uintptr_t> nativeCreateContexts{0};
bool readMemory(uintptr_t address, void *output, size_t length) noexcept {
    mach_vm_size_t copied = 0;
    return address && mach_vm_read_overwrite(mach_task_self(), address, length,
        reinterpret_cast<mach_vm_address_t>(output), &copied) == KERN_SUCCESS
        && copied == length;
}
// Read native leaf protections, not a shared-region parent's coarse policy.
// This performs no vm_protect, text writes or policy changes.
bool leafMemoryPermissions(uintptr_t address,size_t length,vm_prot_t required,vm_prot_t forbidden) noexcept {
    if(!address || !length || address>UINTPTR_MAX-length)return false;
    natural_t depth=0;
    for(unsigned attempt=0;attempt<16;++attempt) {
        mach_vm_address_t region=address;mach_vm_size_t span=0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
        if(mach_vm_region_recurse(mach_task_self(),&region,&span,&depth,
            reinterpret_cast<vm_region_recurse_info_t>(&info),&count)!=KERN_SUCCESS ||
            count!=VM_REGION_SUBMAP_INFO_COUNT_64 || region>address ||
            address-region>span || length>span-(address-region))return false;
        if(info.is_submap){if(depth>=16)return false;++depth;continue;}
        return (info.protection&required)==required && !(info.protection&forbidden);
    }
    return false;
}
bool executableMemory(uintptr_t address,size_t length) noexcept {
    return leafMemoryPermissions(address,length,VM_PROT_READ|VM_PROT_EXECUTE,VM_PROT_WRITE);
}
bool writableDataMemory(uintptr_t address,size_t length) noexcept {
    return leafMemoryPermissions(address,length,VM_PROT_READ|VM_PROT_WRITE,VM_PROT_EXECUTE);
}
uintptr_t qualifiedIoAcceleratorFramework(uintptr_t address) noexcept {
    Dl_info info{};
    constexpr uint8_t uuid[16]={0x19,0x8a,0x77,0x6a,0xfe,0x03,0x35,0xee,0x8e,0x53,0x7b,0x9f,0x14,0xca,0xab,0xaa};
    if(!address || !dladdr(reinterpret_cast<void*>(address),&info) || !info.dli_fbase || !info.dli_fname ||
       std::strcmp(info.dli_fname,"/System/Library/PrivateFrameworks/IOAccelerator.framework/Versions/A/IOAccelerator"))return 0;
    const uintptr_t base=reinterpret_cast<uintptr_t>(info.dli_fbase);
    mach_header_64 header{};
    if(!readMemory(base,&header,sizeof(header)) || header.magic!=MH_MAGIC_64 || header.filetype!=MH_DYLIB ||
       header.cputype!=CPU_TYPE_X86_64 || !header.ncmds || header.ncmds>256 ||
       header.sizeofcmds>65536 || header.ncmds>header.sizeofcmds/8 ||
       base>UINTPTR_MAX-sizeof(header)-header.sizeofcmds)return 0;
    size_t offset=0;bool found=false;
    for(uint32_t i=0;i<header.ncmds;++i) {
        load_command command{};
        if(header.sizeofcmds-offset<sizeof(command) ||
           !readMemory(base+sizeof(header)+offset,&command,sizeof(command)) ||
           command.cmdsize<sizeof(command) || (command.cmdsize&7) || command.cmdsize>header.sizeofcmds-offset)return 0;
        if(command.cmd==LC_UUID) {
            uuid_command identity{};
            if(found || command.cmdsize!=sizeof(identity) ||
               !readMemory(base+sizeof(header)+offset,&identity,sizeof(identity)) ||
               std::memcmp(identity.uuid,uuid,sizeof(uuid)))return 0;
            found=true;
        }
        offset+=command.cmdsize;
    }
    return found && offset==header.sizeofcmds?base:0;
}
bool pinnedFile() {
    FILE *file = std::fopen(nativePath, "rb");
    if (!file) return false;
    CC_SHA256_CTX context;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA256_Init(&context);
    uint8_t buffer[8192], hash[32];
    size_t total = 0, count;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file))) {
        total += count;
        if (total > 8450544) { std::fclose(file); return false; }
        CC_SHA256_Update(&context, buffer, static_cast<CC_LONG>(count));
    }
    bool valid = !std::ferror(file) && total == 8450544;
    CC_SHA256_Final(hash, &context);
#pragma clang diagnostic pop
    std::fclose(file);
    return valid && std::memcmp(hash, expectedHash, sizeof(hash)) == 0;
}
void load() {
    if (!pinnedFile()) return;
    library.handle = dlopen(nativePath, RTLD_NOW | RTLD_LOCAL);
    if (!library.handle) return;
    library.create = reinterpret_cast<Create>(dlsym(library.handle, "AVD_CreateAVDAccelerator"));
    library.destroy = reinterpret_cast<Destroy>(dlsym(library.handle, "AVD_DestroyAVDAccelerator"));
    library.resourceShared=reinterpret_cast<uintptr_t>(dlsym(library.handle,"IOAccelResourceGetClientShared"));
    // Keep the native image loaded for all forwarded callback lifetimes.
}
}

// Expand the real Darwin/native backend composition, not the fixture Api and
// tracked allocator types. This compiles cleanup, but enables no callbacks.
template class TglOwnedVeboxVpCleanupContext<NativeRendererBacking,NativeRendererRetirementAuthority>;
template class TglNativeVeboxRendererCpuStorage<TglDarwinStmmSurfaceApi,NativeRendererCleanup>;
template class TglOwnedVphalRendererOwner<NativeRendererCpuAssembly,TglOwnedPhysicalDependencyLease,
    decltype(&readMemory),NativeDataPointerWriter,NativeDataAdmission,NativeRendererChildAdmission,
    NativeVphalDestructorInvoker>;

extern "C" __attribute__((visibility("default")))
int NGRN_ObservedCreateContexts(void *context, const void *input) {
    using Callback = int (*)(void *, const void *);
    const auto address = nativeCreateContexts.load(std::memory_order_acquire);
    const auto native = reinterpret_cast<Callback>(address);
    if (!native) return 10;
    // Only this owned table slot changes. The native callback receives the
    // original context/input exactly once, and its result passes through.
    // Pinned 13e43 -> 14342 constructor -> vtable 75d018+10 = 2338e0;
    // 14e1f invokes it synchronously. Prior observation at 2340c5 captured raw=31.
    if (std::strcmp(getprogname(), "VTEncoderXPCService")) return native(context, input);
    os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_CONTEXT_CALLBACK_ENTER scoped-observation-only");
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(address), &info) || !info.dli_fbase ||
        address != reinterpret_cast<uintptr_t>(info.dli_fbase) + 0x13e43) {
        os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_SKIP callback-identity");
        return native(context, input);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    const uintptr_t site = base + 0x13260f, source = base + 0x531550;
    constexpr uint8_t anchor[] = {0xe8,0x9c,0x42,0xf3,0xff};
    // Crash 8734F7ED: native passed the metadata factory, then dereferenced
    // null renderer+0x910 at 13282a. Capture the two real vtables, not guessed
    // subclasses. These are registers the following native call dereferences;
    // no object memory, Apple text or native result is changed.
    constexpr uint8_t creatorAnchor[] = {0xff,0x51,0x18};
    constexpr uint8_t rendererAnchor[] = {0xff,0x52,0x20};
    uint8_t creatorActual[3]{}, rendererActual[3]{};
    uint8_t actual[sizeof(anchor)]{};
    std::vector<uint8_t> owned;
    try { owned.resize(TglKernelMetadataAdapter::blobBytes); }
    catch (...) { return native(context, input); }
    if (!readMemory(base + 0x133c0f, creatorActual, sizeof(creatorActual)) ||
        std::memcmp(creatorActual, creatorAnchor, sizeof(creatorAnchor)) ||
        !readMemory(base + 0x133c36, rendererActual, sizeof(rendererActual)) ||
        std::memcmp(rendererActual, rendererAnchor, sizeof(rendererAnchor)) ||
        !readMemory(site, actual, sizeof(actual)) || std::memcmp(actual, anchor, sizeof(anchor)) ||
        !readMemory(source, owned.data(), owned.size()) || !TglKernelMetadataAdapter::translate(owned)) {
        os_log_error(OS_LOG_DEFAULT, "NGRN_OWNED_KERNEL_METADATA_SKIP exact-data-or-call-anchor");
        return native(context, input);
    }
    // Native thread/debug policy remains authoritative: refusal leaves native untouched.
    // The single pinned copy consumes our owned CPU metadata; GPU kernel bytes are identical.
    // The XPC sandbox rejects global /private/tmp. Ask Darwin for this
    // service identity's native temporary directory; never expand policy.
    char temporary[1024]{};
    char journalPath[1100]{};
    const size_t temporaryBytes = confstr(_CS_DARWIN_USER_TEMP_DIR, temporary, sizeof(temporary));
    if (!temporaryBytes || temporaryBytes > sizeof(temporary) || temporary[0] != '/') {
        os_log_error(OS_LOG_DEFAULT, "NGRN_OWNED_COPY_JOURNAL_DENIED native-temp-directory");
        return native(context, input);
    }
    const int pathBytes = std::snprintf(journalPath, sizeof(journalPath), "%s%sngreen-owned-copy-XXXXXX",
        temporary, temporary[temporaryBytes - 2] == '/' ? "" : "/");
    if (pathBytes < 0 || size_t(pathBytes) >= sizeof(journalPath)) return native(context, input);
    struct Journal {
        int fd;
        ~Journal() { if (fd >= 0) close(fd); }
    } journal{mkstemp(journalPath)};
    if (journal.fd < 0) {
        os_log_error(OS_LOG_DEFAULT, "NGRN_OWNED_COPY_JOURNAL_DENIED native-unchanged");
        return native(context, input);
    }
    os_log_error(OS_LOG_DEFAULT, "NGRN_OWNED_COPY_JOURNAL path=%{public}s", journalPath);
    NativeReturnObserver observer({site, base + 0x133c0f, base + 0x133c36, 0},
        0, 0, 0, 4, base, 8450544,
        {source, reinterpret_cast<uintptr_t>(owned.data()), owned.size(), journal.fd}, 2);
    const int result = native(context, input);
    os_log_error(OS_LOG_DEFAULT, "NGRN_OWNED_KERNEL_METADATA_COPY applied=%{public}d result=%{public}d",
        int(observer.observed(0, 1)), result);
    std::fprintf(stderr, "NGRN_NATIVE_CONTEXT_CALLBACK_RETURN result=%d\n", result);
    os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_CONTEXT_CALLBACK_RETURN result=%{public}d", result);
    return result;
}

extern "C" __attribute__((visibility("default")))
int AVD_CreateAVDAccelerator(void **output, const void *input, AvdMetadata *metadata) {
    if (!output || !input || !metadata) return 5;
    *output = nullptr;
    *metadata = {};
    void *table = nullptr;
    try {
        if(!retainOwnedHookImage())return 10; // Before native factory side effects.
        std::call_once(loadOnce, load);
        if (!library.create || !library.destroy || !library.resourceShared) return 10;
        AvdMetadata native{};
        int result = library.create(&table, input, &native);
        if (result) { if (table) library.destroy(table); return result; }
        auto entry = std::make_unique<Entry>();
        if (!table || native.renderer != 0x1080080 || !entry->bridge.build(native.descriptor, readMemory) ||
            !readMemory(reinterpret_cast<uintptr_t>(table), entry->callbacks.data(), sizeof(entry->callbacks))) {
            if (table) library.destroy(table);
            return 10;
        }
        Dl_info callbackInfo{};
        const auto callback = entry->callbacks[3];
        if (!dladdr(reinterpret_cast<void *>(callback), &callbackInfo) || !callbackInfo.dli_fbase ||
            !callbackInfo.dli_fname || std::strcmp(callbackInfo.dli_fname, nativePath) ||
            callback != reinterpret_cast<uintptr_t>(callbackInfo.dli_fbase) + 0x13e43) {
            library.destroy(table);
            return 10;
        }
        const uintptr_t image=reinterpret_cast<uintptr_t>(callbackInfo.dli_fbase);
        if(!entry->physical.resolve(image,readMemory,
            [image](uintptr_t candidate) noexcept {return candidate==image;},executableMemory) ||
            !entry->physical.matchesFactoryTable(entry->callbacks)) {
            library.destroy(table);
            return 10;
        }
        if(!entry->nativeCpu.resolve(image,readMemory,
            [image](uintptr_t candidate) noexcept {return candidate==image;},executableMemory)) {
            library.destroy(table);
            return 10;
        }
        const uintptr_t framework=qualifiedIoAcceleratorFramework(library.resourceShared);
        const auto frameworkImage=[framework](uintptr_t address) noexcept {
            Dl_info info{};
            return framework && dladdr(reinterpret_cast<void*>(address),&info) &&
                reinterpret_cast<uintptr_t>(info.dli_fbase)==framework;
        };
        if(!framework || !entry->resourceClass.resolve(library.resourceShared,readMemory,frameworkImage,executableMemory)) {
            library.destroy(table);
            return 10;
        }
        nativeCreateContexts.store(callback, std::memory_order_release);
        entry->native = table;
        entry->callbacks[3] = reinterpret_cast<uintptr_t>(&NGRN_ObservedCreateContexts);
        native.descriptor = reinterpret_cast<uintptr_t>(entry->bridge.descriptor());
        void *ownedTable = entry->callbacks.data();
        {
            std::lock_guard<std::mutex> lock(registryLock);
            if (registry.count(ownedTable)) { library.destroy(table); return 10; }
            registry.emplace(ownedTable, std::move(entry));
        }
        // Owned table keeps all 15 other callbacks and all native contexts intact.
        *metadata = native;
        *output = ownedTable;
        std::fprintf(stderr, "NGRN_ADAPTER_FACTORY_TRANSLATED native=430 consumer=438\n");
        return 0;
    } catch (...) {
        if (table && library.destroy) library.destroy(table);
        return 7;
    }
}

extern "C" __attribute__((visibility("default")))
int AVD_DestroyAVDAccelerator(void *table) {
    if (!table) return 5;
    try {
    std::unique_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(registryLock);
        auto found = registry.find(table);
        if (found == registry.end()) return 5;
        entry = std::move(found->second);
        registry.erase(found);
    }
    void *native = entry->native;
    entry.reset();
    return library.destroy(native);
    } catch (...) { return 7; }
}
