#include "descriptor_bridge.hpp"
#include "tgl_capability_adapter.hpp"
#include "native_return_observer.hpp"
#include "owned_resource.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
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
struct Library { void *handle = nullptr; Create create = nullptr; Destroy destroy = nullptr; };
Library library;
std::once_flag loadOnce;
std::mutex registryLock;
struct Entry {
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
bool executableMemory(uintptr_t address,size_t length) noexcept {
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
        return (info.protection&(VM_PROT_READ|VM_PROT_EXECUTE))==(VM_PROT_READ|VM_PROT_EXECUTE) &&
            !(info.protection&VM_PROT_WRITE);
    }
    return false;
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
    // Keep the native image loaded for all forwarded callback lifetimes.
}
}

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
        std::call_once(loadOnce, load);
        if (!library.create || !library.destroy) return 10;
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
