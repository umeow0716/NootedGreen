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
bool leafMemoryPermissions(uintptr_t address,size_t length,vm_prot_t required,vm_prot_t forbidden) noexcept;
bool ownedExecutableMemory(uintptr_t address,size_t length) noexcept;
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
struct NativeStackPacketAdmission {
    bool operator()(uintptr_t address,size_t size) const noexcept {
        const auto thread=pthread_self();
        const auto top=reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
        const size_t extent=pthread_get_stacksize_np(thread);
        return address && size && top && extent && extent<=top && address>=top-extent &&
            address<=UINTPTR_MAX-size && address+size<=top && writableDataMemory(address,size);
    }
};
// Frame data capability, distinct from the qualified native image reader.
// The retained frame supplies its closed region resolver; there is no raw
// readMemory fallback and this object does not extend the resolver lifetime.
struct NativeFrameDataRead {
    void* scope=nullptr;
    bool(*resolve)(void*,uintptr_t,void*,size_t) noexcept=nullptr;
    bool operator()(uintptr_t address,void* output,size_t bytes) const noexcept {
        return scope && resolve && address && output && bytes && address<=UINTPTR_MAX-bytes &&
            resolve(scope,address,output,bytes);
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
using NativeRendererChildOwner=std::remove_reference_t<decltype(std::declval<NativeRendererCpu&>().children().first().owner())>;
using NativePreparedVpReset=TglOwnedVeboxPreparedVpReset<NativeRendererChildOwner>;
struct NativeRendererChildRetirement {
    NativeRendererRetirementAuthority* authority;
    void* child;uint64_t generation;size_t index;
    int operator()() const noexcept {return (*authority)(child,generation,index);}
};
using NativeRendererChildLifetime=TglOwnedVeboxLifetimeContext<NativeRendererChildOwner,NativeRendererChildRetirement>;
using NativeVpModeResolve=TglOwnedVpSfcResolve<TglOwnedVpModeContext<decltype(&readMemory)>>;
using NativeAllocationSfcFormat=TglOwnedVeboxSfcModeQuery<true,NativeRendererChildOwner,NativeVpModeResolve>;
using NativeAllocationSfcColor=TglOwnedVeboxSfcModeQuery<false,NativeRendererChildOwner,NativeVpModeResolve>;
using NativeExecutionBackend=TglExecutionBackend<decltype(&readMemory),NativeTglImageAdmission,TglNativeExecutionInvoker>;
using NativeExecutionOwner=TglExecutionOwner<NativeExecutionBackend>;
using NativeVpGraph=std::remove_reference_t<decltype(std::declval<TglNativeVpCpuStorage<>&>().graph())>;
struct NativeSfcReleaseSelection {
    NativeRendererChildOwner& owner;void* child;uintptr_t renderHal,sku;NativeVpGraph& graph;
    bool operator()(bool& output) const noexcept {
        return tglSelectLeasedVeboxSfcRelease(output,owner,child,renderHal,sku,graph,&readMemory);
    }
};
static_assert(sizeof(NativeRendererCpu)>0 && !std::is_copy_constructible_v<NativeRendererCpu>);
class NativeRendererCpuAssembly {
    static TglExecutionBinding executionBinding(uintptr_t image) noexcept {
        TglExecutionBinding binding;
        if(!binding.resolve(image,&readMemory,NativeTglImageAdmission{image},&executableMemory))return {};
        return binding;
    }
    static int scopedRetirement(void* storage,void* child,uint64_t generation,size_t index) noexcept {
        auto* context=TglOwnedScopedContext<TglOwnedVphalRetirementContext,TglOwnedVphalRetirementDomain>::get(storage);
        return context && context->scope && context->admit?
            context->admit(context->scope,child,generation,index):5;
    }
    // Value-owned authority outlives contexts, which outlive CPU/child teardown.
    // The opaque key is THIS assembly, never a borrowed scope pointee. Actual
    // authority is resolved only inside the creator's synchronous TLS scope.
    const uintptr_t image_;
    NativeRendererRetirementAuthority authority_;
    NativeRendererCleanupContext firstCleanup_,secondCleanup_;
    NativeExecutionBackend executionBackend_;
    NativeRendererCpu cpu_;
    std::array<NativeExecutionOwner,2> executions_;
    bool cleanupBound_=false,basesAttempted_=false,basesReady_=false,vpAttempted_=false;
public:
    NativeRendererCpuAssembly(uintptr_t image,
        const TglNativeVphalResourceEnvironment& environment)
        :image_(image),authority_{this,&scopedRetirement},firstCleanup_(authority_),secondCleanup_(authority_),
         executionBackend_(executionBinding(image),&readMemory,NativeTglImageAdmission{image},
            TglNativeExecutionInvoker{},&executableMemory),
         cpu_(image,&readMemory,NativeTglImageAdmission{image},&executableMemory,
            environment.resources,environment.pageSize,environment.fill,
            std::array<TglNativeTrackedCpuInvoker,2>{},
            std::array<NativeRendererCleanup,2>{{{&firstCleanup_},{&secondCleanup_}}},
            std::array<TglNativeTrackedCpuInvoker,2>{},std::array<TglNativeSfcLifetimeInvoker,2>{},
            std::array<TglNativeVeboxDestructorInvoker,2>{}),
         executions_{{NativeExecutionOwner(executionBackend_),NativeExecutionOwner(executionBackend_)}} {}
    NativeRendererCpuAssembly(const NativeRendererCpuAssembly&)=delete;
    NativeRendererCpuAssembly& operator=(const NativeRendererCpuAssembly&)=delete;
    auto& cpu() noexcept {return cpu_;}
    auto& backing() noexcept {return cpu_.backing();}
    auto& children() noexcept {return cpu_.children();}
    bool readyForFrontend() noexcept {
        if(!executionBackend_.bound() || !basesReady_ || !cleanupBound_ || backing().retention()() ||
            !backing().vpBound(0) || !backing().vpBound(1))return false;
        // Do not retain a frame pin before both private tables are admitted:
        // that pin would prevent the exclusive creator installation stage.
        return children().first().owner().withPrivateChild([&](void*) noexcept -> int {
            return children().second().owner().withPrivateChild([](void*) noexcept -> int {return 0;});
        })==0;
    }
    template<class Use> int withChildExecution(size_t index,Use use) noexcept {
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(index>1 || !readyForFrontend())return 5;
        auto& owner=index?children().second().owner():children().first().owner();
        auto& execution=executions_[index];
        return owner.withNativeOwnedExecution(execution,&readMemory,[&](void* child,uintptr_t report) noexcept -> int {
            static_assert(noexcept(use(owner,execution,child,report)) &&
                std::is_same_v<decltype(use(owner,execution,child,report)),int>);
            return use(owner,execution,child,report);
        });
    }
    template<class Prepare,class Use> TglOwnedVeboxCommandTransactionResult withChildVpResourcesPrepared(
        size_t index,Prepare prepare,Use use) noexcept {
        TglOwnedVeboxCommandTransactionResult result;
        if constexpr(std::is_pointer_v<Use>)if(!use)return result;
        const int status=withChildExecution(index,[&](auto& owner,auto& execution,void* child,
            uintptr_t report) noexcept -> int {
            auto enter=[&](NativePreparedVpReset* reset) noexcept -> int {
              return backing().withVpResources(index,[&](auto& graph,auto& storage,auto& lines) noexcept -> int {
                return owner.withLeasedBorrowedVpCpuView(child,storage.bytes.data(),
                    [&](void* parent,void* vp) noexcept -> int {
                        if(parent!=child || vp!=storage.bytes.data() || !execution.hasExecutionLease(report))return 5;
                        static_assert(noexcept(use(owner,execution,parent,report,graph,storage,lines,reset)) &&
                            std::is_same_v<decltype(use(owner,execution,parent,report,graph,storage,lines,reset)),
                                TglOwnedVeboxCommandTransactionResult>);
                        result=use(owner,execution,parent,report,graph,storage,lines,reset);
                        backing().retention().observe(result); // before child80/report/VP lease unwind
                        return result.status;
                    });
              });
            };
            return prepare(owner,execution,child,report,enter);
        });
        if(status)result.status=status;
        return result;
    }
    template<class Use> TglOwnedVeboxCommandTransactionResult withChildVpResources(size_t index,Use use) noexcept {
        if constexpr(std::is_pointer_v<Use>)if(!use)return {};
        return withChildVpResourcesPrepared(index,
            [](auto&,auto&,void*,uintptr_t,auto enter) noexcept -> int {return enter(nullptr);},
            [&](auto& owner,auto& execution,void* child,uintptr_t report,auto& graph,auto& storage,
                auto& lines,NativePreparedVpReset*) noexcept {return use(owner,execution,child,report,graph,storage,lines);});
    }
    int initializeNativeBases(const TglNativeVphalMhwView& view,const TglOwnedVphalLifetimeTable& lifetime) noexcept {
        if(basesAttempted_ || view.renderer.image!=image_ || lifetime.object()!=view.parent ||
            lifetime.publication()!=TglOwnedVphalLifetimeTable::Publication::Published)return 5;
        basesAttempted_=true;
        try {
            TglVeboxLifetimeBinding base;TglSfcLifetimeBinding sfc;
            const NativeTglImageAdmission qualify{image_};
            if(!base.resolve(image_,&readMemory,qualify,&executableMemory) ||
                !sfc.resolve(image_,&readMemory,qualify,&executableMemory))return 5;
            const int initialized=tglInitializeRendererChildrenFromCreator(cpu_,view,base,sfc,
                &readMemory,qualify,&executableMemory);
            if(initialized)return initialized;
            const int shells=cpu_.allocateCpuShellsFromCreator(view,&readMemory,qualify,&executableMemory);
            if(shells)return shells;
            const int helpers=cpu_.allocateHelpersFromCreator(view,&readMemory,qualify,&executableMemory);
            basesReady_=helpers==0;return helpers;
        } catch(...) {return 7;}
    }
    int initializeNativeVpPair(const TglNativeVphalMhwView& view,
        const TglOwnedVphalLifetimeTable& lifetime) noexcept {
        if(!basesReady_ || vpAttempted_ || view.renderer.image!=image_ ||
            lifetime.object()!=view.parent ||
            lifetime.publication()!=TglOwnedVphalLifetimeTable::Publication::Published)return 5;
        vpAttempted_=true;
        try {
            // Graph.initialize executes and checks the scoped VP10 entry itself.
            // No second pass initialization before the resource owner pins it.
            const int status=cpu_.initializeVpPairFromCreator(view,&readMemory,
                NativeTglImageAdmission{image_},&executableMemory,
                [](size_t,auto&) noexcept -> int {return 0;});
            if(status)return status; // retain successful first VP on partial failure
            return bindCleanupContexts()?0:5;
        } catch(...) {return 7;}
    }
    bool bindCleanupContexts() noexcept {
        if(cleanupBound_)return false;
        auto& first=children().first().owner();auto& second=children().second().owner();
        if(!firstCleanup_.bind(backing(),first,first.constructedBase(),first.allocationGeneration(),0) ||
            !secondCleanup_.bind(backing(),second,second.constructedBase(),second.allocationGeneration(),1))return false;
        cleanupBound_=true;return true;
    }
    template<class Use> int withChildLifetimeScopes(Use use) noexcept {
        static_assert(noexcept(use(nullptr,nullptr,std::declval<NativeRendererChildLifetime&>(),
            std::declval<NativeRendererChildLifetime&>())) &&
            std::is_same_v<decltype(use(nullptr,nullptr,std::declval<NativeRendererChildLifetime&>(),
                std::declval<NativeRendererChildLifetime&>())),int>);
        if(!cleanupBound_ || backing().retention()())return 5;
        auto& first=children().first().owner();auto& second=children().second().owner();
        void* a=first.constructedBase();void* b=second.constructedBase();
        if(!a || !b || a==b)return 5;
        using Life=NativeRendererChildLifetime;
        using Retirement=NativeRendererChildRetirement;
        using Scope=TglOwnedVeboxScopedContext<Life>;
        Life firstLife(first,Retirement{&authority_,a,first.allocationGeneration(),0});
        Life secondLife(second,Retirement{&authority_,b,second.allocationGeneration(),1});
        typename Scope::Pair scope(a,firstLife,b,secondLife);
        if(!scope.bound())return 5;
        // The consumer may build/use the typed hooks only within this scope.
        // A missing retirement domain rejects teardown; no synchronous default.
        return use(a,b,firstLife,secondLife);
    }
    // No implicit retirement here. Existing VPHAL owner must retain this WHOLE
    // allocation on any partial construction/cleanup/native teardown refusal.
};
struct NativeRendererChildAdmission {
    bool operator()(void* renderer,uint64_t generation,uintptr_t first,uintptr_t second) const noexcept {
        auto* context=TglOwnedScopedContext<TglOwnedVphalChildAdmissionContext,TglOwnedVphalChildAdmissionDomain>::get(renderer);
        return context && context->scope && context->admit &&
            context->admit(context->scope,renderer,generation,first,second);
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
struct NativeVphalRoutingPolicy {
    static bool admit(uintptr_t image,uintptr_t physical,uint64_t generation,uintptr_t object) noexcept {
        auto* context=TglOwnedScopedContext<TglOwnedVphalRoutingContext,TglOwnedVphalRoutingDomain>::get(reinterpret_cast<void*>(object));
        return context && context->matches(image,physical,generation,object);
    }
    static void unresolved(uintptr_t,TglOwnedVphalDestructionState::Kind kind,bool faulted) noexcept {
        os_log_error(OS_LOG_DEFAULT,"NGRN_VPHAL_ROUTER_UNRESOLVED kind=%{public}u faulted=%{public}d",unsigned(kind),int(faulted));
    }
    static void retained(NativeVphalOwner&,TglOwnedVphalDestructionState::Kind kind,bool quarantined) noexcept {
        os_log_error(OS_LOG_DEFAULT,"NGRN_VPHAL_ROUTER_RETAINED kind=%{public}u quarantined=%{public}d",unsigned(kind),int(quarantined));
    }
};
using NativeVphalRouter=TglOwnedVphalProcessRouter<NativeVphalOwner,NativeVphalRoutingPolicy>;
using NativeVphalFramePin=decltype(NativeVphalRouter::pinFrame(uintptr_t{},uintptr_t{},uint64_t{},uintptr_t{}));
using NativeFrontendAccess=TglPinnedVphalStorageAccess<NativeDataAdmission,NativeTglImageAdmission,
    decltype(&executableMemory)>;
using NativeFrontendEnvironment=TglNativeFrontendCpuEnvironment<decltype(&readMemory),NativeTglImageAdmission,
    decltype(&executableMemory)>;
using NativeContextSurfaceOwner=typename TglNativeContextSurfaceOwners<NativeCodecBackend::Registry>::Owner;
template<class SurfaceOwner> using NativeVpStateHealthFor=
    TglLeasedVpStateFrameHealth<NativeRendererChildOwner,NativeExecutionOwner,SurfaceOwner,SurfaceOwner>;
using NativeVpStateHealth=NativeVpStateHealthFor<NativeContextSurfaceOwner>;
// Core ranges derive from actual typed owners, never a VM-readable-address
// grant. Other metadata/interfaces need their own explicit retained grants.
template<class SurfaceOwner> struct NativeCoreFrameDataScopeFor {
    NativeRendererChildOwner& childOwner;NativeExecutionOwner& executionOwner;
    SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
    const TglOwnedSurfaceStorage& source;const TglOwnedSurfaceStorage& target;
    TglOwnedVpSfcCallerStorage& vp;
    uintptr_t image,child,execution;uint64_t generation;
    NativePreparedVpReset* preparedReset=nullptr;
    NativeFrameDataRead capability() noexcept {return {this,&resolve};}
    NativeVpStateHealthFor<SurfaceOwner> vpHealth(uintptr_t os) noexcept {
        return {childOwner,executionOwner,sourceOwner,targetOwner,reinterpret_cast<void*>(child),
            execution,image,os,source,target};
    }
    bool live() const noexcept {
        return generation && childOwner.allocationGeneration()==generation &&
            childOwner.hasPrivateChildImageLease(child,image) &&
            childOwner.hasPrivateSfcRenderDataLease(child,execution) && executionOwner.hasExecutionLease(execution) &&
            sourceOwner.hasSurfaceLease(source) && targetOwner.hasSurfaceLease(target);
    }
    static bool contains(uintptr_t start,size_t extent,uintptr_t address,size_t bytes) noexcept {
        return start && bytes && address>=start && address-start<=extent && bytes<=extent-(address-start);
    }
    static bool resolve(void* context,uintptr_t address,void* output,size_t bytes) noexcept {
        auto* self=static_cast<NativeCoreFrameDataScopeFor*>(context);
        if(!self || !output || !bytes || !self->live())return false;
        if(self->childOwner.readLeasedStorage(self->child,self->generation,address,output,bytes))return self->live();
        if(contains(reinterpret_cast<uintptr_t>(self->vp.bytes.data()),self->vp.bytes.size(),address,bytes)) {
            uintptr_t linked=0;
            if(!self->childOwner.readLeasedStorage(self->child,self->generation,self->child+0x80,&linked,8) ||
                linked!=reinterpret_cast<uintptr_t>(self->vp.bytes.data()))return false;
            return readMemory(address,output,bytes) && self->live();
        }
        // Exact native report constructor allocates d48 bytes; its linked
        // report identity and NativeExecutionOwner lease are checked above.
        const bool admitted=contains(self->execution,0xd48,address,bytes) ||
            contains(reinterpret_cast<uintptr_t>(&self->source),sizeof(self->source),address,bytes) ||
            contains(reinterpret_cast<uintptr_t>(&self->target),sizeof(self->target),address,bytes);
        return admitted && readMemory(address,output,bytes) && self->live();
    }
};
using NativeCoreFrameDataScope=NativeCoreFrameDataScopeFor<NativeContextSurfaceOwner>;
using NativeTemporarySurfaceOwner=typename TglNativePerPassCpuShells<
    TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>>::template DerivedSurfaceLease<NativeContextSurfaceOwner>;
// Compile the SAME bounded resolver/health against concrete derived owners.
// Only synchronous producer views use these types; retained frontend owners
// remain the original registry/context-pinned owners.
template struct NativeCoreFrameDataScopeFor<NativeTemporarySurfaceOwner>;
template<class SurfaceOwner> struct NativeVpScalarStateProducerFor {
    NativeVpStateHealthFor<SurfaceOwner>& health;NativeFrameDataRead& dataRead;
    const TglOwnedSurfaceStorage& source;const TglOwnedSurfaceStorage& target;
    const TglFillCscBinding& fill;const TglNativeVpCompressionBinding& compression;
    TglOwnedSfcFillCache& cache;
    uintptr_t image,settings,conditionalSource,alpha;void* vp;
    TglOwnedVpChromaStatePrefix produced{};
    bool attempted=false,ready=false,busy=false;
    // The initialized graph supplies frame/geometry to its scoped State
    // callback. This is ONLY scalar/prefix production, not a complete Gen12
    // packet: resource allocation/binding and command emission must follow.
    int prepare(void* actual,const TglOwnedVpFramePreparation& frame,
        const TglOwnedVpRegionInput& geometry) noexcept {
        if(attempted || busy || !vp || actual!=vp || !settings || !dataRead.scope || !dataRead.resolve ||
            !health.qualifiesImage(image) || !health.qualifiesVp(reinterpret_cast<uintptr_t>(vp)))return 5;
        attempted=busy=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy};
        const auto sourceAddress=reinterpret_cast<uintptr_t>(&source);
        const auto targetAddress=reinterpret_cast<uintptr_t>(&target);
        if(!health(sourceAddress,targetAddress))return 5;
        auto candidateCache=cache;TglOwnedVpChromaStatePrefix candidate;
        const int status=tglPrepareNativeLeasedOwnedVpCpuState(frame,geometry,sourceAddress,target,
            settings,conditionalSource,alpha,fill,compression,dataRead,&readMemory,
            NativeTglImageAdmission{image},&executableMemory,
            [&](uintptr_t s,uintptr_t t) noexcept {return health(s,t) && health.qualifiesImage(image) &&
                health.qualifiesVp(reinterpret_cast<uintptr_t>(vp));},
            candidateCache,candidate);
        if(status)return status;
        if(!health(sourceAddress,targetAddress) || !health.qualifiesImage(image) ||
            !health.qualifiesVp(reinterpret_cast<uintptr_t>(vp)))return 5;
        cache=candidateCache;produced=candidate;ready=true;return 0;
    }
    const TglOwnedVpChromaStatePrefix* state() const noexcept {
        return ready && !busy && health.qualifiesImage(image) &&
            health.qualifiesVp(reinterpret_cast<uintptr_t>(vp))?&produced:nullptr;
    }
};
using NativeVpScalarStateProducer=NativeVpScalarStateProducerFor<NativeContextSurfaceOwner>;
template struct NativeVpScalarStateProducerFor<NativeTemporarySurfaceOwner>;
// Scope domains distinguish original and derived scalar owners; a producer
// cannot retrieve another owner's publication merely by sharing the VP key.
template<class SurfaceOwner> struct NativeVpPacketDomainFor {};
template<class SurfaceOwner> struct NativeVpPacketContextFor {
    NativeVpScalarStateProducerFor<SurfaceOwner>* scalar;TglOwnedSfcCommandParameters* parameters;
};
template<class SurfaceOwner> using NativeVpPacketPublicationFor=
    TglOwnedScopedContext<NativeVpPacketContextFor<SurfaceOwner>,NativeVpPacketDomainFor<SurfaceOwner>>;
using NativeVpPacketDomain=NativeVpPacketDomainFor<NativeContextSurfaceOwner>;
using NativeVpPacketContext=NativeVpPacketContextFor<NativeContextSurfaceOwner>;
using NativeVpPacketPublication=NativeVpPacketPublicationFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeVpNv12PacketScopeFor;
using NativeVpNv12PacketScope=NativeVpNv12PacketScopeFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeVpAvsProducerFor {
    using Owner=TglOwnedSfcAvsParameters<TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>>;
    NativeVpScalarStateProducerFor<SurfaceOwner>& scalar;NativeFrameDataRead& dataRead;
    TglOwnedSfcAvsTables& cache;TglOwnedSfcCommandParameters& parameters;
    uintptr_t performance;
    uint32_t phaseSourceSiting=0;
    bool attempted=false,ready=false;
    Owner* preparedOwner=nullptr;
    std::array<uint8_t,0x30> preparedHeader{};
    bool readyForSend() const noexcept {
        if(!ready || !preparedOwner || !preparedOwner->hasParametersLease() || !scalar.state())return false;
        const auto* header=preparedOwner->parameters();
        return header && *header==preparedHeader;
    }
    int prepare(void* actual,const TglOwnedVpFramePreparation& frame,Owner& owner,
        NativeVpNv12PacketScopeFor<SurfaceOwner>& packet) noexcept;
    template<class Use> int run(void* actual,const TglOwnedVpFramePreparation& frame,
        Owner& owner,Use use) noexcept {
        static_assert(noexcept(use(parameters)) && std::is_same_v<decltype(use(parameters)),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        const auto* prefix=scalar.state();
        const auto* packet=NativeVpPacketPublicationFor<SurfaceOwner>::get(actual);
        if(attempted || !prefix || actual!=scalar.vp || &dataRead!=&scalar.dataRead ||
            !packet || packet->scalar!=&scalar || packet->parameters!=&parameters || parameters.filtersInUse)return 5;
        TglOwnedSfcAvsConfiguration config;
        if(!tglReadOwnedVpAvsConfiguration(frame,*prefix,performance,dataRead,config))return 5;
        TglNativeSineBinding sine;
        // Native math admission precedes even a cache hit. No host-libm
        // substitution and no widening of the closed frame-data reader.
        if(!sine.resolve(scalar.image,&readMemory,
            [&](uintptr_t image) noexcept {return scalar.health.qualifiesImage(image) &&
                scalar.health.qualifiesVp(reinterpret_cast<uintptr_t>(actual));},&executableMemory))return 5;
        attempted=true;phaseSourceSiting=config.phaseSourceSiting;
        const int status=tglWithOwnedSfcAvsCommands(parameters,owner,cache,config.format,
            config.scaleX,config.scaleY,config.siting,config.forceFilter,config.symmetric,
            config.sharpening,config.window,
            [&](float value) noexcept {return sine.evaluate(value);},
            [&](TglOwnedSfcCommandParameters& commands) noexcept -> int {
                if(scalar.state()!=prefix)return 5;
                const auto* header=owner.parameters();
                if(!owner.hasParametersLease() || !header)return 5;
                preparedOwner=&owner;preparedHeader=*header;
                ready=true;
                const int result=use(commands); // coefficient/AVS lease still active
                if(scalar.state()!=prefix) {ready=false;return result?result:5;}
                return result;
            },&phaseSourceSiting);
        if(status)ready=false;
        return status;
    }
};
using NativeVpAvsProducer=NativeVpAvsProducerFor<NativeContextSurfaceOwner>;
template int NativeVpAvsProducer::run<int(*)(TglOwnedSfcCommandParameters&) noexcept>(
    void*,const TglOwnedVpFramePreparation&,NativeVpAvsProducer::Owner&,
    int(*)(TglOwnedSfcCommandParameters&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeVpIefCscProducerFor {
    NativeVpScalarStateProducerFor<SurfaceOwner>& scalar;NativeFrameDataRead& dataRead;
    SurfaceOwner& sourceOwner;
    TglOwnedSfcCommandParameters& parameters;TglOwnedVpCscCache& cache;
    bool attempted=false;
    bool prepared=false,cscPrepared=false,sendAttempted=false;
    std::array<uint8_t,64> preparedIef{};
    int prepare(void* actual,const TglOwnedVpFramePreparation& frame,
        NativeVpNv12PacketScopeFor<SurfaceOwner>& packet,uint8_t* liveState) noexcept;
    template<class Use> int withPreparedInputs(void* actual,Use use) noexcept {
        static_assert(noexcept(use(static_cast<const TglOwnedSfcCommandParameters&>(parameters))) &&
            std::is_same_v<decltype(use(static_cast<const TglOwnedSfcCommandParameters&>(parameters))),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        const auto* packet=NativeVpPacketPublicationFor<SurfaceOwner>::get(actual);
        if(!prepared || sendAttempted || !scalar.state() || actual!=scalar.vp ||
            !packet || packet->scalar!=&scalar || packet->parameters!=&parameters ||
            parameters.filtersInUse || parameters.ief!=preparedIef ||
            !sourceOwner.hasSurfaceLease(scalar.source))return 5;
        sendAttempted=true;parameters.filtersInUse=true;
        auto consume=[&]() noexcept -> int {
            if(!scalar.state())return 5;
            const int result=use(static_cast<const TglOwnedSfcCommandParameters&>(parameters));
            return result?result:(scalar.state()?0:5);
        };
        const int result=cscPrepared?cache.withParameters(parameters.ief,parameters.state,consume):consume();
        parameters.filtersInUse=false;return result;
    }
    template<class Use> int run(void* actual,const TglOwnedVpFramePreparation& frame,Use use) noexcept {
        static_assert(noexcept(use(static_cast<const TglOwnedSfcCommandParameters&>(parameters))) &&
            std::is_same_v<decltype(use(static_cast<const TglOwnedSfcCommandParameters&>(parameters))),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        const auto* prefix=scalar.state();const auto* packet=NativeVpPacketPublicationFor<SurfaceOwner>::get(actual);
        if(attempted || !prefix || actual!=scalar.vp || &dataRead!=&scalar.dataRead || !packet ||
            packet->scalar!=&scalar || packet->parameters!=&parameters || parameters.filtersInUse ||
            !sourceOwner.hasSurfaceLease(scalar.source))return 5;
        TglOwnedSfcIefInput input;
        input.enabled=frame.iefPresent && (frame.iefFlags&1) && frame.iefStrength>0.f;
        if(input.enabled!=bool(frame.at922))return 5;
        input.strength=frame.iefStrength;input.skinEnable=frame.iefSkin;
        input.detailEnable=frame.iefDetail;input.thresholds=frame.iefThresholds;
        auto qualify=[&](uintptr_t image) noexcept {return scalar.state()==prefix &&
            scalar.health.qualifiesImage(image);};
        TglOwnedIefTables tables{};
        if(input.enabled && !tglReadQualifiedOwnedIefTables(tables,scalar.image,&readMemory,qualify))return 5;
        const bool csc=frame.at920;uint32_t from=0,to=0;
        TglCscCoefficientsBinding binding;
        if(csc && (!dataRead(reinterpret_cast<uintptr_t>(&scalar.source),&from,4) ||
            !dataRead(reinterpret_cast<uintptr_t>(&scalar.target),&to,4) ||
            !binding.resolve(scalar.image,&readMemory,qualify,&executableMemory)))return 5;
        attempted=true;
        auto produce=[&](TglCscCoefficients& output,uint32_t a,uint32_t b) noexcept {
            TglNativeCscInvoker invoke;
            // VP+a0 uses native coefficients, NOT VEBOX+b8's extra R/B swap.
            return tglProduceNativeCscCoefficients(binding,output,a,b,&readMemory,qualify,&executableMemory,
                [&](uintptr_t entry,uint32_t x,uint32_t y,float* m,float* i,float* o) noexcept {
                    invoke.coefficients(entry,x,y,m,i,o);
                },[&](uintptr_t entry,uint32_t x,uint32_t y,float* m) noexcept {return invoke.matrix(entry,x,y,m);});
        };
        auto filters=[&](uintptr_t metadata,auto read) noexcept -> int {
            return tglWithOwnedSfcFilterCommands(parameters,input,tables,cache,csc,from,to,metadata,read,produce,
                [&](TglOwnedSfcCommandParameters& commands) noexcept -> int {
                    if(scalar.state()!=prefix)return 5;
                    const int status=use(static_cast<const TglOwnedSfcCommandParameters&>(commands));
                    return status?status:(scalar.state()==prefix?0:5);
                });
        };
        uintptr_t metadata=0;
        if(csc)std::memcpy(&metadata,scalar.source.prefix.data()+0xa8,8);
        if(metadata)return sourceOwner.withLeasedConversionMetadata(scalar.source,&readMemory,filters);
        return filters(0,[](uintptr_t,void*,size_t) noexcept {return false;});
    }
};
using NativeVpIefCscProducer=NativeVpIefCscProducerFor<NativeContextSurfaceOwner>;
template int NativeVpIefCscProducer::run<int(*)(const TglOwnedSfcCommandParameters&) noexcept>(
    void*,const TglOwnedVpFramePreparation&,int(*)(const TglOwnedSfcCommandParameters&) noexcept) noexcept;
template int NativeVpIefCscProducer::withPreparedInputs<int(*)(const TglOwnedSfcCommandParameters&) noexcept>(
    void*,int(*)(const TglOwnedSfcCommandParameters&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeNv12OutputQueryFor {
    struct State {bool attempted=false;TglOwnedNv12LayoutRead result{};};
    SurfaceOwner& owner;const TglOwnedSurfaceStorage& expected;State& state;
    int operator()(TglOwnedSurfaceStorage& original) const noexcept {
        if(state.attempted || &original!=&expected || owner.surfaceBinding()!=&expected ||
            !owner.hasSurfaceLease(original))return 5;
        state.attempted=true;
        TglDarwinNv12LayoutApi api;
        using Quarantine=TglOwnedStmmProcessQuarantine<IOSurfaceRef>;
        typename Quarantine::Reservation reservation(Quarantine::processLifetime());
        state.result=tglPrepareLeasedOwnedNv12Metadata(owner,original,api,reservation);
        if(state.result.ready())return 0;
        return state.result.lockStatus?state.result.lockStatus:
            state.result.unlockStatus?state.result.unlockStatus:5;
    }
};
using NativeNv12OutputQuery=NativeNv12OutputQueryFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeVpNv12PacketScopeFor {
    using CpuBackend=TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>;
    using StateOwner=TglOwnedSfcStateParameters<CpuBackend>;
    NativeVpScalarStateProducerFor<SurfaceOwner>& scalar;SurfaceOwner& targetOwner;
    NativeNv12OutputQueryFor<SurfaceOwner> query;
    TglOwnedGen12TargetCompression preparedCompression_{};
    bool attempted=false;
    NativeVpNv12PacketScopeFor(NativeVpScalarStateProducerFor<SurfaceOwner>& producer,SurfaceOwner& target,
        NativeNv12OutputQueryFor<SurfaceOwner> outputQuery) noexcept:scalar(producer),targetOwner(target),query(outputQuery) {}
    NativeVpNv12PacketScopeFor(const NativeVpNv12PacketScopeFor&)=delete;
    NativeVpNv12PacketScopeFor& operator=(const NativeVpNv12PacketScopeFor&)=delete;
    template<class Use> int run(TglOwnedVpSfcCallerStorage& vp,TglOwnedSfcStateParameters<CpuBackend>& state,
        TglNativeVpLineResourceStorage& lines,const TglOwnedVpEntrySet& catalogue,
        TglOwnedSfcCommandParameters& parameters,
        Use use) noexcept {
        static_assert(noexcept(use(parameters)) && std::is_same_v<decltype(use(parameters)),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        const auto* prefix=scalar.state();
        auto* packet=state.data();
        if(attempted || query.state.attempted || !prefix || !packet || scalar.vp!=vp.bytes.data() || !catalogue.allocate ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(catalogue.allocate),1) ||
            !lines.lines().boundToVp(vp) || &query.owner!=&targetOwner || &query.expected!=&scalar.target ||
            !targetOwner.hasSurfaceLease(scalar.target) || parameters.filtersInUse)return 5;
        TglOwnedSfcGen12Tail tail;
        TglOwnedSfcGen12FilterFlags filters;
        TglOwnedGen12TargetCompression compression;
        if(!tglReadOwnedSfcGen12FilterFlags(prefix->packet,filters) ||
            !tglReadLeasedGen12TargetCompression(targetOwner,scalar.target,scalar.dataRead,compression) ||
            !tglPrepareOwnedVeboxSfcGen12Tail(prefix->packet,tail))return 5;
        attempted=true;
        uint32_t input=0,scaled=0;
        std::memcpy(&input,prefix->packet.data()+0x30,4);std::memcpy(&scaled,prefix->packet.data()+0x4c,4);
        const int allocation=tglAllocateOwnedVpPacketLines(vp,state,prefix->packet,[&](void* actual) noexcept -> int {
            return catalogue.allocate(actual); // actual scoped owned VP58, once
        });
        if(allocation)return allocation;
        if(scalar.state()!=prefix)return 5;
        return lines.resources().withCommandResources(input,scaled,
            [&](const auto& avs,const auto& ief,const auto* sfd) noexcept -> int {
                return targetOwner.withMutableSurface([&](TglOwnedSurfaceStorage& output) noexcept -> int {
                    if(&output!=&scalar.target || scalar.state()!=prefix)return 5;
                    const int prepared=tglPrepareOwnedVpLeasedSurfaceState(parameters,state,output,
                        avs,ief,sfd,prefix->packet,tail,filters.rgbAdaptive,filters.eightTapChroma,
                        compression.enabled,compression.mode,query);
                    if(prepared)return prepared;
                    TglOwnedGen12TargetCompression after;
                    if(!tglReadLeasedGen12TargetCompression(targetOwner,output,scalar.dataRead,after) ||
                        after.enabled!=compression.enabled || after.mode!=compression.mode)return 5;
                    preparedCompression_=compression;
                    NativeVpPacketContextFor<SurfaceOwner> context{&scalar,&parameters};
                    NativeVpPacketPublicationFor<SurfaceOwner> scope(scalar.vp,context);
                    if(!scope.bound())return 5;
                    // Keep resource/target scopes through AVS/IEF and send.
                    // State parameter lease is NOT held during their updates.
                    const int status=use(parameters);
                    return status?status:(scalar.state()==prefix && targetOwner.hasSurfaceLease(output)?0:5);
                });
            });
    }
private:
    StateOwner* preparedState_=nullptr;
    TglOwnedVpSfcCallerStorage* preparedVp_=nullptr;
    TglNativeVpLineResourceStorage* preparedLines_=nullptr;
    TglOwnedSfcCommandParameters* preparedCommands_=nullptr;
    const uint8_t* preparedData_=nullptr;
    uint32_t input_=0,scaled_=0;
    bool preparedForSend_=false,sendAttempted_=false;
    std::array<uint32_t,13> layout_{};
    static std::array<uint32_t,13> layout(const TglOwnedSurfaceStorage& surface) noexcept {
        constexpr std::array<size_t,13> offsets{0xe0,0xf0,0xf4,0xf8,0x100,0x104,0x108,0x110,0x114,0x118,0x130,0xd8,0xdc};
        std::array<uint32_t,13> out{};
        for(size_t i=0;i<offsets.size();++i)std::memcpy(&out[i],surface.prefix.data()+offsets[i],4);
        return out;
    }
public:
    bool readyForSetup(const NativeVpScalarStateProducerFor<SurfaceOwner>& producer,
        const TglOwnedSfcCommandParameters& commands) const noexcept {
        return &producer==&scalar && preparedForSend_ && !sendAttempted_ &&
            preparedState_ && preparedVp_ && preparedLines_ && preparedCommands_==&commands &&
            scalar.state() && query.state.result.ready() &&
            targetOwner.hasSurfaceLease(scalar.target) && layout(scalar.target)==layout_ &&
            static_cast<const StateOwner&>(*preparedState_).data()==preparedData_ &&
            preparedLines_->lines().boundToVp(*preparedVp_) && !commands.filtersInUse;
    }
    bool matchesPreparedState(const uint8_t* state) const noexcept {
        return state && preparedCommands_ && state==preparedData_ && readyForSetup(scalar,*preparedCommands_);
    }
    // Setup materializes owned packet/resources only. No borrowed coefficient
    // pointers or scope addresses survive this call; graph/frame ownership
    // retains the allocations until send or qualified retirement.
    int prepare(TglOwnedVpSfcCallerStorage& vp,StateOwner& state,TglNativeVpLineResourceStorage& lines,
        const TglOwnedVpEntrySet& catalogue,TglOwnedSfcCommandParameters& parameters) noexcept {
        const int status=run(vp,state,lines,catalogue,parameters,
            [&](TglOwnedSfcCommandParameters& commands) noexcept -> int {
                preparedState_=&state;preparedVp_=&vp;preparedLines_=&lines;preparedCommands_=&commands;
                preparedData_=state.data();layout_=layout(scalar.target);
                std::memcpy(&input_,commands.state.data()+0x30,4);std::memcpy(&scaled_,commands.state.data()+0x4c,4);
                return preparedData_?0:5;
            });
        preparedForSend_=status==0;return status;
    }
    template<class Use> int withPreparedInputs(Use use) noexcept {
        static_assert(noexcept(use(std::declval<TglOwnedSfcCommandParameters&>())) &&
            std::is_same_v<decltype(use(std::declval<TglOwnedSfcCommandParameters&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(!preparedForSend_ || sendAttempted_ || !preparedState_ || !preparedVp_ || !preparedLines_ ||
            !preparedCommands_ || !scalar.state() || !query.state.result.ready() ||
            !targetOwner.hasSurfaceLease(scalar.target) || layout(scalar.target)!=layout_ ||
            !preparedState_->hasParametersLease() ||
            static_cast<const StateOwner&>(*preparedState_).data()!=preparedData_ ||
            !preparedLines_->lines().boundToVp(*preparedVp_) || preparedCommands_->filtersInUse)return 5;
        TglOwnedGen12TargetCompression currentCompression;
        if(!tglReadLeasedGen12TargetCompression(targetOwner,scalar.target,scalar.dataRead,currentCompression) ||
            currentCompression.enabled!=preparedCompression_.enabled || currentCompression.mode!=preparedCompression_.mode)return 5;
        sendAttempted_=true;
        return preparedState_->withPublishedVpParameters(*preparedVp_,[&](const uint8_t* data) noexcept -> int {
            if(data!=preparedData_)return 5;
            return preparedLines_->resources().withCommandResources(input_,scaled_,
                [&](const auto& avs,const auto& ief,const auto* sfd) noexcept -> int {
                    return targetOwner.withMutableSurface([&](TglOwnedSurfaceStorage& output) noexcept -> int {
                        if(&output!=&scalar.target || !scalar.state() || layout(output)!=layout_)return 5;
                        const std::array<uintptr_t,3> expected{reinterpret_cast<uintptr_t>(output.resource.data()),
                            reinterpret_cast<uintptr_t>(avs.resource.data()),reinterpret_cast<uintptr_t>(ief.resource.data())};
                        for(size_t i=0;i<expected.size();++i) {
                            uintptr_t actual=0;std::memcpy(&actual,data+0x90+i*8,8);if(actual!=expected[i])return 5;
                        }
                        uintptr_t row=0;std::memcpy(&row,data+0xe0,8);
                        if(row!=(sfd?reinterpret_cast<uintptr_t>(sfd->data()):0))return 5;
                        const auto saved=preparedCommands_->state;
                        std::memcpy(preparedCommands_->state.data(),data,saved.size());
                        NativeVpPacketContextFor<SurfaceOwner> context{&scalar,preparedCommands_};
                        NativeVpPacketPublicationFor<SurfaceOwner> scope(scalar.vp,context);
                        const int status=scope.bound()?use(*preparedCommands_):5;
                        preparedCommands_->state=saved;
                        return status?status:(scalar.state() && targetOwner.hasSurfaceLease(output) && layout(output)==layout_?0:5);
                    });
                });
        });
    }
};
template<class SurfaceOwner> int NativeVpAvsProducerFor<SurfaceOwner>::prepare(void* actual,const TglOwnedVpFramePreparation& frame,
    Owner& owner,NativeVpNv12PacketScopeFor<SurfaceOwner>& packet) noexcept {
    if(!packet.readyForSetup(scalar,parameters) || actual!=scalar.vp)return 5;
    NativeVpPacketContextFor<SurfaceOwner> context{&scalar,&parameters};
    NativeVpPacketPublicationFor<SurfaceOwner> publication(actual,context);
    if(!publication.bound())return 5;
    const int result=run(actual,frame,owner,[&](TglOwnedSfcCommandParameters&) noexcept -> int {
        return packet.readyForSetup(scalar,parameters)?0:5;
    });
    if(!packet.readyForSetup(scalar,parameters)) {ready=false;return result?result:5;}
    return result;
}
template<class SurfaceOwner> int NativeVpIefCscProducerFor<SurfaceOwner>::prepare(void* actual,const TglOwnedVpFramePreparation& frame,
    NativeVpNv12PacketScopeFor<SurfaceOwner>& packet,uint8_t* liveState) noexcept {
    if(!packet.readyForSetup(scalar,parameters) || !packet.matchesPreparedState(liveState) ||
        actual!=scalar.vp)return 5;
    NativeVpPacketContextFor<SurfaceOwner> context{&scalar,&parameters};
    NativeVpPacketPublicationFor<SurfaceOwner> publication(actual,context);
    if(!publication.bound())return 5;
    const int result=run(actual,frame,[&](const TglOwnedSfcCommandParameters&) noexcept -> int {
        return scalar.state()?0:5;
    });
    // run has closed the temporary CSC borrow: only owned IEF bytes and the
    // persistent coefficient cache may survive setup. Never copy its pointers.
    if(result || !packet.matchesPreparedState(liveState))return result?result:5;
    for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
        uintptr_t pointer=0;std::memcpy(&pointer,parameters.ief.data()+offset,8);
        if(pointer)return 5;
    }
    liveState[0x67]=parameters.state[0x67];liveState[0x68]=parameters.state[0x68];
    preparedIef=parameters.ief;cscPrepared=bool(frame.at920);prepared=true;
    return 0;
}
template struct NativeNv12OutputQueryFor<NativeTemporarySurfaceOwner>;
template struct NativeVpNv12PacketScopeFor<NativeTemporarySurfaceOwner>;
template struct NativeVpAvsProducerFor<NativeTemporarySurfaceOwner>;
template struct NativeVpIefCscProducerFor<NativeTemporarySurfaceOwner>;
template int NativeVpNv12PacketScopeFor<NativeTemporarySurfaceOwner>::run<int(*)(TglOwnedSfcCommandParameters&) noexcept>(
    TglOwnedVpSfcCallerStorage&,TglOwnedSfcStateParameters<NativeVpNv12PacketScope::CpuBackend>&,
    TglNativeVpLineResourceStorage&,const TglOwnedVpEntrySet&,TglOwnedSfcCommandParameters&,
    int(*)(TglOwnedSfcCommandParameters&) noexcept) noexcept;
template int NativeVpNv12PacketScope::run<int(*)(TglOwnedSfcCommandParameters&) noexcept>(
    TglOwnedVpSfcCallerStorage&,TglOwnedSfcStateParameters<NativeVpNv12PacketScope::CpuBackend>&,
    TglNativeVpLineResourceStorage&,const TglOwnedVpEntrySet&,TglOwnedSfcCommandParameters&,
    int(*)(TglOwnedSfcCommandParameters&) noexcept) noexcept;
template int NativeVpNv12PacketScope::withPreparedInputs<int(*)(TglOwnedSfcCommandParameters&) noexcept>(
    int(*)(TglOwnedSfcCommandParameters&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeVpCommandEmissionFor {
    NativeVpScalarStateProducerFor<SurfaceOwner>& scalar;NativeRendererChildOwner& childOwner;
    NativeFrameDataRead& dataRead;SurfaceOwner& targetOwner;
    const TglSfcMhwBinding& binding;const TglNativeSfcOutputUsageBinding& usage;
    const TglOwnedVpEntrySet& catalogue;
    void* parent;void* command;NativeVpAvsProducerFor<SurfaceOwner>* avs;
    bool attempted=false;
    int run(void* actual,const TglOwnedVpFramePreparation& frame,
        const TglOwnedSfcCommandParameters& parameters) noexcept {
        const auto* packet=NativeVpPacketPublicationFor<SurfaceOwner>::get(actual);
        const auto* prefix=scalar.state();
        if(attempted || !prefix || actual!=scalar.vp || !packet || packet->scalar!=&scalar ||
            packet->parameters!=&parameters || &dataRead!=&scalar.dataRead || !command ||
            !scalar.health.matchesChild(childOwner,reinterpret_cast<uintptr_t>(parent)) ||
            !targetOwner.hasSurfaceLease(scalar.target) || binding.image!=scalar.image || !catalogue.platformSend ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(catalogue.platformSend),1))return 5;
        const uint8_t scaling=frame.regions.scale.scaling,forceAvs=frame.color.at9b8;
        const uint8_t ief=frame.at922,csc=frame.at920;
        if(((scaling|forceAvs)&1) && (!avs || !avs->readyForSend() || &avs->scalar!=&scalar ||
            &avs->parameters!=&parameters))return 5;
        if(((ief|csc)&1) && !parameters.filtersInUse)return 5;
        uintptr_t os=0,renderHal=0,mhw=0,renderArgument=0;
        const auto vp=reinterpret_cast<uintptr_t>(actual);
        if(vp>UINTPTR_MAX-0x28 || !dataRead(vp+0x10,&os,8) || !dataRead(vp+0x18,&renderHal,8) ||
            !dataRead(vp+0x20,&mhw,8) || !os || !mhw || !renderHal || renderHal>UINTPTR_MAX-0x20 ||
            !dataRead(renderHal+0x18,&renderArgument,8) || !usage.matches(scalar.image,os))return 5;
        auto platform=[&](std::array<uint8_t,0x38>& descriptor) noexcept -> int {
            if(scalar.state()!=prefix || !targetOwner.hasSurfaceLease(scalar.target))return 5;
            // Qualified owned callback reproduces the proven six-argument
            // no-op ABI; not a fabricated generic platform-success callback.
            const int status=catalogue.platformSend(actual,reinterpret_cast<void*>(mhw),
                reinterpret_cast<void*>(renderArgument),reinterpret_cast<void*>(os),descriptor.data(),command);
            return status?status:(scalar.state()==prefix?0:5);
        };
        attempted=true;TglNativeSfcCommandInvoker invoke;
        const int status=tglSendOwnedVeboxSfcNativeOutputCommandsWithReaders(childOwner,parent,binding,
            command,invoke,usage,parameters,scalar.target,scalar.settings,scaling,forceAvs,ief,csc,
            dataRead,&readMemory,platform);
        // Any failure after command entry requires the enclosing frame's
        // qualified discard/retention. Never submit/discard/retire here.
        return status?status:(scalar.state()==prefix && targetOwner.hasSurfaceLease(scalar.target)?0:5);
    }
};
using NativeVpCommandEmission=NativeVpCommandEmissionFor<NativeContextSurfaceOwner>;
// Per-frame callbacks for the graph's existing two-phase producer context.
// Own no allocation; the enclosing graph/frame must retain every referenced
// owner until both setup and send (or qualified failure retirement) finish.
template<class SurfaceOwner> struct NativeVpPreparedCallbacksFor {
    NativeVpScalarStateProducerFor<SurfaceOwner>& scalar;NativeVpNv12PacketScopeFor<SurfaceOwner>& packet;
    NativeVpAvsProducerFor<SurfaceOwner>& avs;NativeVpIefCscProducerFor<SurfaceOwner>& filters;
    NativeVpCommandEmissionFor<SurfaceOwner>& emission;
    TglOwnedVpSfcCallerStorage& vp;TglNativeVpLineResourceStorage& lines;const TglOwnedVpEntrySet& catalogue;
    TglOwnedSfcCommandParameters& commands;
    void* renderData;
    int error=0;
    TglOwnedVpFramePreparation capturedFrame{};
    bool frameCaptured=false;
    int fail(int result) noexcept {if(result && !error)error=result;return result;}
    bool sameOwners() const noexcept {
        return &packet.scalar==&scalar && &avs.scalar==&scalar && &filters.scalar==&scalar &&
            &emission.scalar==&scalar && &avs.parameters==&commands &&
            &filters.parameters==&commands && emission.avs==&avs && scalar.vp==vp.bytes.data() &&
            &emission.catalogue==&catalogue && &packet.targetOwner==&emission.targetOwner;
    }
    int state(void* actual,const TglOwnedVpFramePreparation& frame,const TglOwnedVpRegionInput& capturedGeometry,
        typename NativeVpNv12PacketScopeFor<SurfaceOwner>::StateOwner& owner,void* data,void* source,void* target) noexcept {
        if(error)return error;
        if(!sameOwners() || data!=renderData || !data || source!=&scalar.source || target!=&scalar.target ||
            actual!=scalar.vp)return fail(5);
        int result=scalar.prepare(actual,frame,capturedGeometry);
        if(result)return fail(result);
        result=packet.prepare(vp,owner,lines,
            catalogue,commands);
        if(!result) {capturedFrame=frame;frameCaptured=true;}
        return fail(result);
    }
    int prepareAvs(void* actual,const TglOwnedVpFramePreparation& frame,
        typename NativeVpAvsProducerFor<SurfaceOwner>::Owner& owner) noexcept {
        if(error)return error;
        return fail(sameOwners()?avs.prepare(actual,frame,owner,packet):5);
    }
    void prepareFilters(void* actual,const TglOwnedVpFramePreparation& frame,uint8_t* liveState) noexcept {
        if(error)return;
        fail(sameOwners()?filters.prepare(actual,frame,packet,liveState):5);
    }
    int emit(void* actual,const TglOwnedVpFramePreparation& frame,void* data,void* command) noexcept {
        if(error)return error;
        if(!sameOwners() || actual!=scalar.vp || data!=renderData || !command ||
            (emission.command && emission.command!=command))return fail(5);
        void* previous=emission.command;emission.command=command;
        const int result=packet.withPreparedInputs([&](TglOwnedSfcCommandParameters&) noexcept -> int {
            return filters.withPreparedInputs(actual,[&](const TglOwnedSfcCommandParameters& inputs) noexcept {
                return emission.run(actual,frame,inputs);
            });
        });
        emission.command=previous;return fail(result);
    }
    using AssemblyUse=int(*)(void*,void*,const TglOwnedVpEntrySet&) noexcept;
    bool assemblyAttempted=false;
    int withAssembly(NativeVpGraph& graph,const TglOwnedVpFramePreparation& frame,
        const TglOwnedVpRegionInput& capturedGeometry,uintptr_t sourceA8,void* scope,AssemblyUse use) noexcept {
        if(error)return error;
        if(assemblyAttempted || !sameOwners() || !scope || !use || !renderData ||
            scalar.attempted || packet.attempted || avs.attempted || filters.attempted || emission.attempted)
            return fail(5);
        uintptr_t originalSourceA8=0;
        std::memcpy(&originalSourceA8,scalar.source.prefix.data()+0xa8,8);
        if(sourceA8!=originalSourceA8)return fail(5);
        assemblyAttempted=true;
        const int result=graph.withPreparedCallbackAssemblyPass(catalogue,frame,scalar.conditionalSource,
            scalar.settings,reinterpret_cast<uintptr_t>(&scalar.target),sourceA8,
            const_cast<TglOwnedSurfaceStorage*>(&scalar.source),renderData,
            [&](void* child,const TglOwnedVpFramePreparation& captured,auto& owner,
                void* data,void* source,void* target) noexcept -> int {
                return state(child,captured,capturedGeometry,owner,data,source,target);
            },
            [&](void* child,const TglOwnedVpFramePreparation& captured,auto& owner) noexcept -> int {
                return prepareAvs(child,captured,owner);
            },
            [&](void* child,const TglOwnedVpFramePreparation& captured,uint8_t* liveState) noexcept {
                prepareFilters(child,captured,liveState);
            },
            [&](void* child,void* data,void* command) noexcept -> int {
                return emit(child,frame,data,command);
            },
            [&](void* child,const TglOwnedVpEntrySet& entries) noexcept -> int {
                if(error || child!=vp.bytes.data())return error?error:5;
                const int status=use(scope,child,entries);
                return status?status:error;
            });
        return fail(result?result:error);
    }
};
using NativeVpPreparedCallbacks=NativeVpPreparedCallbacksFor<NativeContextSurfaceOwner>;
// Concrete per-frame VP assembly. CPU caches belong to the enclosing frame
// scope; GPU resources remain the original graph's borrowed owners.
template<class SurfaceOwner> class NativeVpProducerFrameFor {
public:
    struct Inputs {
        const NativeVpScalarStateProducerFor<SurfaceOwner>& seed;
        SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
        NativeRendererChildOwner& childOwner;
        TglOwnedSfcAvsTables& avsCache;TglOwnedVpCscCache& cscCache;
        TglOwnedSfcCommandParameters& commands;
        const TglSfcMhwBinding& binding;const TglNativeSfcOutputUsageBinding& usage;
        const TglOwnedVpEntrySet& catalogue;
        TglOwnedVpSfcCallerStorage& vp;TglNativeVpLineResourceStorage& lines;
        void* parent;void* renderData;uintptr_t performance;
    };
private:
    NativeVpScalarStateProducerFor<SurfaceOwner> scalar_;
    typename NativeNv12OutputQueryFor<SurfaceOwner>::State output_{};
    NativeVpNv12PacketScopeFor<SurfaceOwner> packet_;
    NativeVpAvsProducerFor<SurfaceOwner> avs_;
    NativeVpIefCscProducerFor<SurfaceOwner> filters_;
    NativeVpCommandEmissionFor<SurfaceOwner> emission_;
    NativeVpPreparedCallbacksFor<SurfaceOwner> callbacks_;
    bool attempted_=false;
public:
    explicit NativeVpProducerFrameFor(const Inputs& in) noexcept
        :scalar_(in.seed),packet_(scalar_,in.targetOwner,{in.targetOwner,scalar_.target,output_}),
         avs_{scalar_,scalar_.dataRead,in.avsCache,in.commands,in.performance},
         filters_{scalar_,scalar_.dataRead,in.sourceOwner,in.commands,in.cscCache},
         emission_{scalar_,in.childOwner,scalar_.dataRead,in.targetOwner,in.binding,in.usage,
             in.catalogue,in.parent,nullptr,&avs_},
         callbacks_{scalar_,packet_,avs_,filters_,emission_,in.vp,in.lines,in.catalogue,
             in.commands,in.renderData} {}
    NativeVpProducerFrameFor(const NativeVpProducerFrameFor&)=delete;
    NativeVpProducerFrameFor& operator=(const NativeVpProducerFrameFor&)=delete;
    template<class Use> int run(Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeVpPreparedCallbacksFor<SurfaceOwner>&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeVpPreparedCallbacksFor<SurfaceOwner>&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(attempted_ || scalar_.attempted || scalar_.ready || scalar_.busy ||
            !callbacks_.sameOwners() ||
            !filters_.sourceOwner.hasSurfaceLease(scalar_.source) ||
            !packet_.targetOwner.hasSurfaceLease(scalar_.target))return 5;
        attempted_=true;
        const int status=use(callbacks_);
        return status?status:callbacks_.error;
    }
};
using NativeVpProducerFrame=NativeVpProducerFrameFor<NativeContextSurfaceOwner>;
template struct NativeVpCommandEmissionFor<NativeTemporarySurfaceOwner>;
template struct NativeVpPreparedCallbacksFor<NativeTemporarySurfaceOwner>;
template class NativeVpProducerFrameFor<NativeTemporarySurfaceOwner>;
template int NativeVpProducerFrameFor<NativeTemporarySurfaceOwner>::run<
    int(*)(NativeVpPreparedCallbacksFor<NativeTemporarySurfaceOwner>&) noexcept>(
    int(*)(NativeVpPreparedCallbacksFor<NativeTemporarySurfaceOwner>&) noexcept) noexcept;
template int NativeVpProducerFrame::run<int(*)(NativeVpPreparedCallbacks&) noexcept>(
    int(*)(NativeVpPreparedCallbacks&) noexcept) noexcept;
template<class SurfaceOwner> class NativeVpProducerInputScopeFor {
public:
    struct Inputs {
        NativeCoreFrameDataScopeFor<SurfaceOwner>& core;NativeFrameDataRead& read;
        const TglNativeCreatedVeboxRendererView& pinned;
        const TglVeboxAdmissionBorrowedInterfaces& interfaces;
        TglNativeVpLineResourceStorage& lines;const TglOwnedVpEntrySet& catalogue;
    };
private:
    Inputs& in_;bool attempted_=false;
    TglOwnedSfcFillCache fillCache_;
    TglOwnedSfcAvsTables avsCache_;
    TglOwnedVpCscCache cscCache_;
    TglOwnedSfcCommandParameters commands_;
public:
    explicit NativeVpProducerInputScopeFor(Inputs& in) noexcept:in_(in) {}
    NativeVpProducerInputScopeFor(const NativeVpProducerInputScopeFor&)=delete;
    NativeVpProducerInputScopeFor& operator=(const NativeVpProducerInputScopeFor&)=delete;
    template<class Use> TglOwnedVeboxCommandTransactionResult run(Use use) noexcept {
        TglOwnedVeboxCommandTransactionResult result;auto& in=in_;auto& core=in.core;
        if constexpr(std::is_pointer_v<Use>)if(!use)return result;
        if(attempted_ || !core.live() || in.pinned.image!=core.image ||
            (in.pinned.originalChildren[0]!=core.child && in.pinned.originalChildren[1]!=core.child))return result;
        attempted_=true;
        auto health=core.vpHealth(in.interfaces.os);
        if(!health.qualifiesImage(core.image) || !health.qualifiesVp(reinterpret_cast<uintptr_t>(core.vp.bytes.data())))return result;
        TglFillCscBinding fill;TglNativeVpCompressionBinding compression;
        TglSfcMhwBinding commands;TglNativeSfcOutputUsageBinding usage;
        const NativeTglImageAdmission qualify{core.image};
        if(!fill.resolve(core.image,&readMemory,qualify,&executableMemory) ||
            !compression.resolve(in.interfaces.os,core.image,&readMemory,qualify,&executableMemory) ||
            !commands.resolve(core.image,&readMemory,qualify,&executableMemory) ||
            !usage.resolve(in.interfaces.os,core.image,&readMemory,qualify,&executableMemory))return result;
        TglOwnedVpModeState original;uint8_t flag=0;uintptr_t performance=0;
        const auto vp=reinterpret_cast<uintptr_t>(core.vp.bytes.data());
        if(!in.read(vp+0x998,&original.at998,4) || !in.read(vp+0x99c,&original.at99c,4) ||
            !in.read(vp+0x923,&flag,1) || !in.read(in.pinned.configuration,&performance,8) || !core.live())return result;
        original.at923=bool(flag&1);
        NativeVpScalarStateProducerFor<SurfaceOwner> seed{health,in.read,core.source,core.target,fill,compression,fillCache_,
            core.image,core.execution,in.interfaces.paramsE0,in.interfaces.paramsF0,core.vp.bytes.data()};
        typename NativeVpProducerFrameFor<SurfaceOwner>::Inputs inputs{seed,core.sourceOwner,core.targetOwner,core.childOwner,
            avsCache_,cscCache_,commands_,commands,usage,in.catalogue,core.vp,in.lines,
            reinterpret_cast<void*>(core.child),reinterpret_cast<void*>(core.execution),performance};
        NativeVpProducerFrameFor<SurfaceOwner> producers(inputs);
        static_assert(noexcept(use(producers,original)) &&
            std::is_same_v<decltype(use(producers,original)),TglOwnedVeboxCommandTransactionResult>);
        result=use(producers,original);
        if(!result.status && !core.live())result.status=5;
        return result;
    }
};
using NativeVpProducerInputScope=NativeVpProducerInputScopeFor<NativeContextSurfaceOwner>;
using NativeTemporaryVpProducerInputUse=TglOwnedVeboxCommandTransactionResult(*)(
    NativeVpProducerFrameFor<NativeTemporarySurfaceOwner>&,const TglOwnedVpModeState&) noexcept;
template TglOwnedVeboxCommandTransactionResult NativeVpProducerInputScopeFor<NativeTemporarySurfaceOwner>::run<
    NativeTemporaryVpProducerInputUse>(NativeTemporaryVpProducerInputUse) noexcept;
using NativeVpProducerInputUse=TglOwnedVeboxCommandTransactionResult(*)(NativeVpProducerFrame&,
    const TglOwnedVpModeState&) noexcept;
template TglOwnedVeboxCommandTransactionResult NativeVpProducerInputScope::run<NativeVpProducerInputUse>(
    NativeVpProducerInputUse) noexcept;
template<class SurfaceOwner> struct NativeTwoPassCscHealthFor {
    NativeRendererChildOwner& childOwner;NativeExecutionOwner& executionOwner;
    SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
    const TglOwnedSurfaceStorage& source;const TglOwnedSurfaceStorage& target;
    uintptr_t image,child,execution;uint64_t generation;
    bool operator()(uintptr_t actualSource,uintptr_t actualTarget) const noexcept {
        if(!generation || actualSource!=reinterpret_cast<uintptr_t>(&source) ||
            actualTarget!=reinterpret_cast<uintptr_t>(&target) ||
            childOwner.allocationGeneration()!=generation ||
            !childOwner.hasPrivateChildImageLease(child,image) ||
            !executionOwner.hasExecutionLease(execution) ||
            !sourceOwner.hasSurfaceLease(source) || !targetOwner.hasSurfaceLease(target))return false;
        uintptr_t linked=0;uint32_t format=0;
        return child<=UINTPTR_MAX-0x90 && readMemory(child+0x88,&linked,8) && linked==execution &&
            readMemory(reinterpret_cast<uintptr_t>(source.prefix.data()+0x130),&format,4);
    }
};
using NativeTwoPassCscHealth=NativeTwoPassCscHealthFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeTwoPassCscQueryFor=TglLeasedNativeTwoPassCscQuery<NativeRendererChildOwner,
    decltype(&readMemory),NativeTglImageAdmission,decltype(&executableMemory),
    TglNativeTwoPassCscInvoke,NativeTwoPassCscHealthFor<SurfaceOwner>>;
using NativeTwoPassCscQuery=NativeTwoPassCscQueryFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeVeboxCscProducerFor {
    NativeTwoPassCscHealthFor<SurfaceOwner>& health;const TglCscCoefficientsBinding& binding;
    bool busy=false;
    bool operator()(TglCscCoefficients& output,uint32_t from,uint32_t to,uint32_t format) noexcept {
        const auto source=reinterpret_cast<uintptr_t>(&health.source);
        const auto target=reinterpret_cast<uintptr_t>(&health.target);
        if(busy || binding.image!=health.image || !health(source,target))return false;
        uint32_t expectedFrom=0,expectedTo=0,expectedFormat=0;
        std::memcpy(&expectedFrom,health.source.prefix.data(),4);
        std::memcpy(&expectedTo,health.target.prefix.data(),4);
        std::memcpy(&expectedFormat,health.source.prefix.data()+0x130,4);
        if(from!=expectedFrom || to!=expectedTo || format!=expectedFormat)return false;
        busy=true;struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy};
        auto qualify=[&](uintptr_t image) noexcept {return image==health.image && health(source,target);};
        TglNativeCscInvoker invoke;TglCscCoefficients candidate;
        const bool produced=tglPrepareOwnedVeboxCsc(binding,candidate,from,to,format,&readMemory,qualify,
            &executableMemory,[&](uintptr_t entry,uint32_t a,uint32_t b,float* m,float* i,float* o) noexcept {
                invoke.coefficients(entry,a,b,m,i,o);
            },[&](uintptr_t entry,uint32_t a,uint32_t b,float* m) noexcept {return invoke.matrix(entry,a,b,m);});
        if(!produced || !health(source,target))return false;
        output=candidate;return true;
    }
};
using NativeVeboxCscProducer=NativeVeboxCscProducerFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeTargetDecompressionFor {
    void* scope=nullptr;
    int(*invoke)(void*,SurfaceOwner&,const TglOwnedSurfaceStorage&) noexcept=nullptr;
};
using NativeTargetDecompression=NativeTargetDecompressionFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeFastFillTransactionFor {
    SurfaceOwner& targetOwner;
    TglVeboxPipeAcquisition& acquisition;
    // This Darwin policy consumes the proven compressed/mode fields; it has
    // no compression-format side-channel consumer. Decompression remains real.
    int& decompressionStatus;
    NativeTargetDecompressionFor<SurfaceOwner>& decompression;
    template<class Policy> bool operator()(Policy& policy) noexcept {
        bool entered=false,selected=false;
        const int status=targetOwner.withMutableSurface([&](TglOwnedSurfaceStorage& target) noexcept -> int {
            entered=true;
            bool compressed=false;auto mode=TglOwnedSfcCompressionMode::Disabled;
            if(!policy.readTargetCompressionState(compressed,mode))return 5;
            auto decompress=[&]() noexcept -> int {
                // Admission belongs at the actual conditional call: Media,
                // non-8-aligned height, AFTER fast-fill/direct recheck. Render
                // compression and aligned Media do not invoke this producer.
                if(!decompression.scope || !decompression.invoke ||
                    !ownedExecutableMemory(reinterpret_cast<uintptr_t>(decompression.invoke),1) ||
                    !targetOwner.hasSurfaceLease(target)) {
                    acquisition.fail();return 5;
                }
                const int native=decompression.invoke(decompression.scope,targetOwner,target);
                if(!targetOwner.hasSurfaceLease(target))acquisition.fail();
                return native;
            };
            selected=policy.runFastFillTransactionWithClear(target,compressed,mode,
                decompressionStatus,decompress,[]() noexcept {}); // no format side channel in this path
            return selected?0:5;
        });
        if(!entered)acquisition.fail();
        return !status && selected && acquisition.healthy();
    }
};
using NativeFastFillTransaction=NativeFastFillTransactionFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeOutputPipePolicyFor=TglLeasedGen12VeboxOutputPolicy<NativeFrameDataRead,
    NativeRendererChildOwner,SurfaceOwner,SurfaceOwner,
    NativeTwoPassCscQueryFor<SurfaceOwner>,NativeFastFillTransactionFor<SurfaceOwner>>;
using NativeOutputPipePolicy=NativeOutputPipePolicyFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeOutputPipeSelectorFor=TglOwnedVpPipeSelector<NativeFrameDataRead,NativeOutputPipePolicyFor<SurfaceOwner>>;
using NativeOutputPipeSelector=NativeOutputPipeSelectorFor<NativeContextSurfaceOwner>;
// Compile the concrete selector/transaction bodies; no policy instance is
// published until the compressed-resource producer has a proven native route.
template<class SurfaceOwner> struct NativeOutputPipeSelectionFor {
    static uint32_t select(NativeOutputPipeSelectorFor<SurfaceOwner>& selector,uint8_t& compositionNeeded) noexcept {
        return selector(compositionNeeded);
    }
};
using NativeOutputPipeSelection=NativeOutputPipeSelectionFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeOutputCscQueryScopeFor {
    static bool query(NativeTwoPassCscQueryFor<SurfaceOwner>& scoped,uintptr_t source,uintptr_t target,bool& needed) noexcept {
        return scoped(source,target,needed);
    }
    template<class Use> static int run(NativeTwoPassCscHealthFor<SurfaceOwner>& health,Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeTwoPassCscQueryFor<SurfaceOwner>&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeTwoPassCscQueryFor<SurfaceOwner>&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        const auto source=reinterpret_cast<uintptr_t>(&health.source);
        const auto target=reinterpret_cast<uintptr_t>(&health.target);
        if(!health(source,target))return 5;
        auto read=&readMemory;auto executable=&executableMemory;
        NativeTglImageAdmission qualify{health.image};TglNativeTwoPassCscInvoke invoke;
        TglTwoPassCscBinding binding;
        if(!binding.resolve(health.image,read,qualify,executable))return 5;
        NativeTwoPassCscQueryFor<SurfaceOwner> scoped(health.childOwner,binding,health.child,read,qualify,executable,invoke,health);
        const int status=use(scoped);
        return status?status:(health(source,target)?0:5);
    }
};
using NativeOutputCscQueryScope=NativeOutputCscQueryScopeFor<NativeContextSurfaceOwner>;
template int NativeOutputCscQueryScope::run<int(*)(NativeTwoPassCscQuery&) noexcept>(
    NativeTwoPassCscHealth&,int(*)(NativeTwoPassCscQuery&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeOutputPipeFrameScopeFor {
    NativeTwoPassCscHealthFor<SurfaceOwner>& health;NativeFrameDataRead& read;
    TglOwnedVpSfcCallerStorage& vp;uintptr_t parameters,di,colorFill;
    const TglOwnedGen12SurfaceFeatures& sourceFeatures;
    const TglOwnedGen12SurfaceFeatures& targetFeatures;const uint8_t& compositionNeeded;
    TglVeboxPipeAcquisition& acquisition;
    int& decompressionStatus;NativeTargetDecompressionFor<SurfaceOwner>& decompression;
    bool attempted=false;
    template<class Use> int run(Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeOutputPipeSelectorFor<SurfaceOwner>&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeOutputPipeSelectorFor<SurfaceOwner>&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(attempted || !read.scope || !read.resolve || !acquisition.healthy())return 5;
        attempted=true;
        NativeFastFillTransactionFor<SurfaceOwner> fastFill{health.targetOwner,acquisition,decompressionStatus,decompression};
        return NativeOutputCscQueryScopeFor<SurfaceOwner>::run(health,[&](NativeTwoPassCscQueryFor<SurfaceOwner>& query) noexcept -> int {
            // Not an inference from the CSC prefix: exact2343d0 uploads
            // the cube for mode2;237730 publishes that mode at+a8/+64.
            // The checked original owner supplies this conversion lane only.
            struct LutSource {
                NativeTwoPassCscHealthFor<SurfaceOwner>& health;TglVeboxPipeAcquisition& acquisition;
                static bool produce(void* context,TglOwnedGen12SurfaceFeatures& output) noexcept {
                    auto& self=*static_cast<LutSource*>(context);
                    return tglAcquireLeasedConversionLutFeatures(self.health.sourceOwner,self.health.source,
                        self.acquisition,&readMemory,output);
                }
            } lut{health,acquisition};
            NativeOutputPipePolicyFor<SurfaceOwner> policy(read,health.childOwner,health.image,health.child,parameters,
                reinterpret_cast<uintptr_t>(vp.bytes.data()),di,colorFill,
                health.sourceOwner,health.source,sourceFeatures,health.targetOwner,health.target,targetFeatures,
                compositionNeeded,acquisition,query,fastFill);
            if(sourceFeatures.lut==TglOwnedFeaturePresence::Unknown &&
                (!ownedExecutableMemory(reinterpret_cast<uintptr_t>(&LutSource::produce),1) ||
                    !policy.bindLutProducer(&lut,&LutSource::produce)))return 5;
            NativeOutputPipeSelectorFor<SurfaceOwner> selector(vp,read,policy,const_cast<TglOwnedSurfaceStorage*>(&health.source),
                const_cast<TglOwnedSurfaceStorage*>(&health.target),reinterpret_cast<void*>(parameters));
            if(!acquisition.healthy())return 5;
            const int status=use(selector);
            return status?status:(acquisition.healthy()?0:5);
        });
    }
};
using NativeOutputPipeFrameScope=NativeOutputPipeFrameScopeFor<NativeContextSurfaceOwner>;
template struct NativeTwoPassCscHealthFor<NativeTemporarySurfaceOwner>;
template struct NativeVeboxCscProducerFor<NativeTemporarySurfaceOwner>;
template struct NativeOutputPipeSelectionFor<NativeTemporarySurfaceOwner>;
template int NativeOutputPipeFrameScopeFor<NativeTemporarySurfaceOwner>::run<
    int(*)(NativeOutputPipeSelectorFor<NativeTemporarySurfaceOwner>&) noexcept>(
    int(*)(NativeOutputPipeSelectorFor<NativeTemporarySurfaceOwner>&) noexcept) noexcept;
template int NativeOutputPipeFrameScope::run<int(*)(NativeOutputPipeSelector&) noexcept>(
    int(*)(NativeOutputPipeSelector&) noexcept) noexcept;
using NativeSourceGraph=TglOwnedFrontendSourceGraph<NativeContextSurfaceOwner,TglDarwinTypedCfReferenceApi>;
template<class SourceGraph> using NativeAdmissionHistoryStageFor=TglOwnedVeboxHistoryAdmissionStage<NativeRendererChildOwner,
    NativeExecutionOwner,SourceGraph,NativeDataAdmission,NativeDataAdmission>;
using NativeAdmissionHistoryStage=NativeAdmissionHistoryStageFor<NativeSourceGraph>;
template<class SourceGraph> struct NativeAdmissionHistorySetupFor {
    NativeAdmissionHistoryStageFor<SourceGraph> stage;
    bool operator()(TglLeasedAdmissionExecution& report) noexcept {
        const auto address=reinterpret_cast<uintptr_t>(stage.child);
        const uint64_t generation=stage.childOwner.allocationGeneration();
        if(!generation || !stage.sourceGraph.hasSurfaceLease(stage.source) ||
            !stage.childOwner.hasPrivateChildLease(address) ||
            !stage.executionOwner.hasExecutionLease(stage.execution))return false;
        // Exact source c0/c8 come from the retained original-context graph.
        // History uses the freshly published report pipe, not a pre-reset copy
        // and not an ABI-incompatible direct call to native child+180.
        const bool accepted=stage(report);
        return accepted && stage.childOwner.allocationGeneration()==generation &&
            stage.childOwner.hasPrivateChildLease(address) &&
            stage.executionOwner.hasExecutionLease(stage.execution) &&
            stage.sourceGraph.hasSurfaceLease(stage.source);
    }
};
using NativeAdmissionHistorySetup=NativeAdmissionHistorySetupFor<NativeSourceGraph>;
using NativeTemporaryHistoryGraph=TglNativePerPassHistoryOverlay<NativeSourceGraph,NativeTemporarySurfaceOwner>;
template struct NativeAdmissionHistorySetupFor<NativeTemporaryHistoryGraph>;
using NativeDenoiseParameters=TglTrackedCpuOwner<TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>>;
template<class SurfaceOwner> struct NativeSfcDenoiseRequestFor {
    SurfaceOwner& owner;const NativeDenoiseParameters& parameters;
    bool operator()(const TglOwnedSurfaceStorage& source,uint8_t& requested) const noexcept {
        return owner.readLeasedDenoiseRequest(source,parameters,requested,&readMemory);
    }
};
using NativeSfcDenoiseRequest=NativeSfcDenoiseRequestFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeSfcAdmissionFlagsFor {
    static bool publish(TglLeasedAdmissionExecution& report,const TglOwnedSurfaceStorage& source,
        const NativeSfcDenoiseRequestFor<SurfaceOwner>& request) noexcept {
        return tglPublishOwnedVeboxSfcAdmissionFlags(report,source,request);
    }
};
using NativeSfcAdmissionFlags=NativeSfcAdmissionFlagsFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeAdmissionFeatureStageFor=TglOwnedVeboxFeatureAdmissionStage<SurfaceOwner,
    SurfaceOwner,NativeRendererChildOwner,NativeExecutionOwner,NativeDenoiseParameters,
    decltype(&readMemory),NativeTglImageAdmission,decltype(&executableMemory)>;
using NativeAdmissionFeatureStage=NativeAdmissionFeatureStageFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeAdmissionFeaturePreparationFor {
    template<class Use> static int run(NativeAdmissionFeatureStageFor<SurfaceOwner>& stage,Use use) noexcept {
        static_assert(noexcept(use(stage)) && std::is_same_v<decltype(use(stage)),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(!stage.child || !stage.sourceOwner.hasSurfaceLease(stage.source) ||
            !stage.targetOwner.hasSurfaceLease(stage.target) ||
            !stage.childOwner.hasPrivateFeaturePreparationLease(
                reinterpret_cast<uintptr_t>(stage.child),stage.binding.image) ||
            !stage.executionOwner.hasExecutionLease(stage.execution))return 5;
        int status=5;
        const bool reconciled=stage.sourceOwner.withNativeDenoiseParameters(stage.source,stage.parameters,
            [&]() noexcept {
                status=stage.executionOwner.withLeasedFeatureReport(stage.execution,&stage.target,
                    [&]() noexcept -> int {return use(stage);});
            });
        return status?status:(reconciled?0:5);
    }
    // Use the report already borrowed by admission, not another execution
    // lease. Native170 consumes source90 and may retain target in report60;
    // caller encloses ALL synchronous consumers in the existing DN/feature
    // scopes and reconciles/detaches before surface owners unlock.
    static bool apply(NativeAdmissionFeatureStageFor<SurfaceOwner>& stage,TglLeasedAdmissionExecution& report) noexcept {
        if(stage.read!=&readMemory || stage.executable!=&executableMemory ||
            !stage.binding.image || stage.qualify.image!=stage.binding.image ||
            !tglHasNativeDenoiseAdmissionGrant(stage.sourceOwner,stage.source,stage.parameters))return false;
        return stage(report);
    }
};
using NativeAdmissionFeaturePreparation=NativeAdmissionFeaturePreparationFor<NativeContextSurfaceOwner>;
template struct NativeSfcDenoiseRequestFor<NativeTemporarySurfaceOwner>;
template struct NativeSfcAdmissionFlagsFor<NativeTemporarySurfaceOwner>;
template struct NativeAdmissionFeaturePreparationFor<NativeTemporarySurfaceOwner>;
template int NativeAdmissionFeaturePreparationFor<NativeTemporarySurfaceOwner>::run<
    int(*)(NativeAdmissionFeatureStageFor<NativeTemporarySurfaceOwner>&) noexcept>(
    NativeAdmissionFeatureStageFor<NativeTemporarySurfaceOwner>&,
    int(*)(NativeAdmissionFeatureStageFor<NativeTemporarySurfaceOwner>&) noexcept) noexcept;
template int NativeAdmissionFeaturePreparation::run<int(*)(NativeAdmissionFeatureStage&) noexcept>(
    NativeAdmissionFeatureStage&,int(*)(NativeAdmissionFeatureStage&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeAllocationParameterKindFor {
    SurfaceOwner& owner;const TglOwnedSurfaceStorage& source;
    bool operator()(uintptr_t parameter,uint32_t& output) const noexcept {
        return owner.readLeasedDiParameterKind(source,parameter,output,&readMemory);
    }
};
using NativeAllocationParameterKind=NativeAllocationParameterKindFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeAllocationRectangleFor {
    SurfaceOwner& input;SurfaceOwner& output;
    NativeRendererChildOwner& child;NativeExecutionOwner& execution;
    int operator()(void* actual,uintptr_t report,const TglOwnedSurfaceStorage& source,
        const TglOwnedFfdiTailViews&,const TglOwnedFfdnTailViews&) const noexcept {
        return output.withMutableSurface([&](auto& target) noexcept -> int {
            return tglCopyCapturedVeboxDirectRectangle(input,source,output,target,child,
                execution,actual,report,&readMemory);
        });
    }
};
using NativeAllocationRectangle=NativeAllocationRectangleFor<NativeContextSurfaceOwner>;
template struct NativeAllocationParameterKindFor<NativeTemporarySurfaceOwner>;
template struct NativeAllocationRectangleFor<NativeTemporarySurfaceOwner>;
struct NativeModeRetirementAuthority {
    void* scope=nullptr;int(*admit)(void*) noexcept=nullptr;
    int operator()() const noexcept {return scope && admit?admit(scope):5;}
};
using NativeRendererBacking=std::remove_reference_t<decltype(std::declval<NativeRendererCpu&>().backing())>;
template<class SurfaceOwner> struct NativeSfcPreparationFor {
    using CpuBackend=TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>;
    using State=int(*)(void*,const TglOwnedVpFramePreparation&,const TglOwnedVpRegionInput&,
        TglOwnedSfcStateParameters<CpuBackend>&,void*,void*,void*) noexcept;
    using Avs=int(*)(void*,const TglOwnedVpFramePreparation&,TglOwnedSfcAvsParameters<CpuBackend>&) noexcept;
    using Ief=void(*)(void*,const TglOwnedVpFramePreparation&,uint8_t*) noexcept;
    using Emit=int(*)(void*,void*,void*,const TglOwnedVpEntrySet&) noexcept;
    NativeRendererBacking& backing;NativeRendererChildOwner& childOwner;NativeExecutionOwner& executionOwner;
    SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
    const TglOwnedVpEntrySet& base;const TglOwnedVpModeState& original;
    size_t index;void* child;uintptr_t mhw;void* renderData;
    State state;Avs avs;Ief ief;Emit emit;
    NativeVpGraph* borrowedGraph=nullptr;TglOwnedVpSfcCallerStorage* borrowedStorage=nullptr;
    template<class Read,class Complete>
    int run(void* expectedVp,uintptr_t e0,uintptr_t f0,const TglOwnedSurfaceStorage& source,
        const TglOwnedSurfaceStorage& target,uintptr_t execution,Read read,Complete complete) noexcept {
        static_assert(noexcept(complete(expectedVp)) && std::is_same_v<decltype(complete(expectedVp)),int>);
        if constexpr(std::is_pointer_v<Read>)if(!read)return 5;
        if constexpr(std::is_pointer_v<Complete>)if(!complete)return 5;
        const auto address=reinterpret_cast<uintptr_t>(child);
        const uint64_t generation=childOwner.allocationGeneration();
        auto live=[&]() noexcept {
            uintptr_t linkedVp=0;
            return index<2 && expectedVp && child && address<=UINTPTR_MAX-0x90 && mhw && renderData && generation &&
                state && avs && ief && emit &&
                childOwner.allocationGeneration()==generation && childOwner.hasPrivateChildLease(address) &&
                childOwner.hasPrivateSfcRenderDataLease(address,execution) &&
                read(address+0x80,&linkedVp,8) && linkedVp==reinterpret_cast<uintptr_t>(expectedVp) &&
                executionOwner.hasExecutionLease(execution) && sourceOwner.hasSurfaceLease(source) &&
                targetOwner.hasSurfaceLease(target);
        };
        if(backing.retention()() || !live() ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(state),1) ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(avs),1) ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(ief),1) ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(emit),1))return 5;
        TglVeboxAdmissionStagesInput input;input.leasedParamsE0=e0;input.leasedParamsF0=f0;
        auto prepare=[&](auto& graph,auto& storage) noexcept -> int {
            if(!live() || storage.bytes.data()!=expectedVp || !graph.ownsStorage(expectedVp))return 5;
            // This acquires/prepares the frame exactly once. Do not invoke
            // entries.prepare again, or send before admission continuation.
            return graph.withSfcAdmissionCallbackAssembly(expectedVp,input,
                reinterpret_cast<uintptr_t>(&source),reinterpret_cast<uintptr_t>(&target),execution,mhw,
                base,original,renderData,read,state,avs,ief,emit,
                [&](void* actual,const TglOwnedVpEntrySet& entries) noexcept -> int {
                    if(actual!=expectedVp || !entries.initialize || !entries.prepare || backing.retention()() || !live())return 5;
                    const int status=complete(actual);
                    // The continuation may acquire/submit work. Its retention
                    // is expected and must survive scope unwind, not be called
                    // a completion certificate or converted to a false error.
                    return status?status:(live() && graph.ownsStorage(actual)?0:5);
                });
        };
        if(borrowedGraph || borrowedStorage) {
            if(!borrowedGraph || !borrowedStorage)return 5;
            return prepare(*borrowedGraph,*borrowedStorage);
        }
        return backing.withVpResources(index,[&](auto& graph,auto& storage,auto&) noexcept -> int {
            return prepare(graph,storage);
        });
    }
};
using NativeSfcPreparation=NativeSfcPreparationFor<NativeContextSurfaceOwner>;
template int NativeSfcPreparationFor<NativeTemporarySurfaceOwner>::run<NativeFrameDataRead,int(*)(void*) noexcept>(
    void*,uintptr_t,uintptr_t,const TglOwnedSurfaceStorage&,const TglOwnedSurfaceStorage&,
    uintptr_t,NativeFrameDataRead,int(*)(void*) noexcept) noexcept;
template int NativeSfcPreparation::run<NativeFrameDataRead,int(*)(void*) noexcept>(
    void*,uintptr_t,uintptr_t,const TglOwnedSurfaceStorage&,const TglOwnedSurfaceStorage&,
    uintptr_t,NativeFrameDataRead,int(*)(void*) noexcept) noexcept;
template<class SurfaceOwner> struct NativeVpPreparationDomainFor {};
template<class SurfaceOwner> struct NativeVpPreparationScopeFor {
    using Publication=TglOwnedScopedContext<NativeVpPreparedCallbacksFor<SurfaceOwner>,NativeVpPreparationDomainFor<SurfaceOwner>>;
    static int state(void* vp,const TglOwnedVpFramePreparation& frame,const TglOwnedVpRegionInput& geometry,
        NativeVpNv12PacketScope::StateOwner& owner,void* data,void* source,void* target) noexcept {
        auto* callbacks=Publication::get(vp);
        return callbacks?callbacks->state(vp,frame,geometry,owner,data,source,target):5;
    }
    static int avs(void* vp,const TglOwnedVpFramePreparation& frame,NativeVpAvsProducer::Owner& owner) noexcept {
        auto* callbacks=Publication::get(vp);
        return callbacks?callbacks->prepareAvs(vp,frame,owner):5;
    }
    static void ief(void* vp,const TglOwnedVpFramePreparation& frame,uint8_t* liveState) noexcept {
        if(auto* callbacks=Publication::get(vp))callbacks->prepareFilters(vp,frame,liveState);
    }
    static int emit(void* vp,void* data,void* command,const TglOwnedVpEntrySet& entries) noexcept {
        auto* callbacks=Publication::get(vp);
        if(!callbacks || !callbacks->frameCaptured || entries.allocate!=callbacks->catalogue.allocate ||
            entries.platformSend!=callbacks->catalogue.platformSend)return 5;
        return callbacks->emit(vp,callbacks->capturedFrame,data,command);
    }
    using Use=int(*)(void*,NativeSfcPreparationFor<SurfaceOwner>&) noexcept;
    static int run(NativeVpPreparedCallbacksFor<SurfaceOwner>& callbacks,NativeSfcPreparationFor<SurfaceOwner>& preparation,
        void* context,Use use) noexcept {
        if(!context || !use || !callbacks.sameOwners() || callbacks.error || callbacks.scalar.attempted ||
            preparation.renderData!=callbacks.renderData || &preparation.base!=&callbacks.catalogue ||
            &preparation.childOwner!=&callbacks.emission.childOwner || preparation.child!=callbacks.emission.parent ||
            &preparation.targetOwner!=&callbacks.packet.targetOwner ||
            &preparation.sourceOwner!=&callbacks.filters.sourceOwner)return 5;
        Publication scope(callbacks.vp.bytes.data(),callbacks);
        if(!scope.bound())return 5;
        const auto oldState=preparation.state;const auto oldAvs=preparation.avs;
        const auto oldIef=preparation.ief;const auto oldEmit=preparation.emit;
        preparation.state=&state;preparation.avs=&avs;preparation.ief=&ief;preparation.emit=&emit;
        const int result=use(context,preparation);
        preparation.state=oldState;preparation.avs=oldAvs;preparation.ief=oldIef;preparation.emit=oldEmit;
        return result?result:callbacks.error;
    }
};
using NativeVpPreparationDomain=NativeVpPreparationDomainFor<NativeContextSurfaceOwner>;
using NativeVpPreparationScope=NativeVpPreparationScopeFor<NativeContextSurfaceOwner>;
template struct NativeVpPreparationScopeFor<NativeTemporarySurfaceOwner>;
template<class SurfaceOwner,class Read> struct NativeSfcPrepareCallFor {
    NativeSfcPreparationFor<SurfaceOwner>& preparation;Read& read;
    template<class Complete> int operator()(void* vp,uintptr_t e0,uintptr_t f0,
        const TglOwnedSurfaceStorage& source,const TglOwnedSurfaceStorage& target,
        uintptr_t execution,Complete complete) noexcept {
        return preparation.run(vp,e0,f0,source,target,execution,read,complete);
    }
};
template<class Read> using NativeSfcPrepareCall=NativeSfcPrepareCallFor<NativeContextSurfaceOwner,Read>;
template<class SurfaceOwner> struct NativeSfcAdmissionFor {
    template<class Read,class Finish>
    static bool run(NativeSfcPreparationFor<SurfaceOwner>& preparation,Read& read,const NativeSfcDenoiseRequestFor<SurfaceOwner>& dn,
        void* vp,uintptr_t execution,const TglOwnedSurfaceStorage& source,const TglOwnedSurfaceStorage& target,
        TglLeasedAdmissionExecution& report,const TglVeboxAdmissionStagesInput& input,Finish finish) noexcept {
        if constexpr(std::is_pointer_v<Finish>)if(!finish)return false;
        // DN authority must name this exact original source owner; it must
        // remain inside the enclosing tracked native-release-slot borrow.
        if(&dn.owner!=&preparation.sourceOwner)return false;
        using Prepare=NativeSfcPrepareCallFor<SurfaceOwner,Read>;
        TglOwnedVeboxScopedSfcAdmissionStage<NativeRendererChildOwner,NativeExecutionOwner,
            SurfaceOwner,SurfaceOwner,NativeSfcDenoiseRequestFor<SurfaceOwner>,Prepare>
            stage(preparation.childOwner,preparation.executionOwner,preparation.sourceOwner,
                preparation.targetOwner,preparation.child,vp,execution,source,target,dn,{preparation,read});
        return stage(report,input,finish);
    }
};
using NativeSfcAdmission=NativeSfcAdmissionFor<NativeContextSurfaceOwner>;
template bool NativeSfcAdmissionFor<NativeTemporarySurfaceOwner>::run<NativeFrameDataRead,bool(*)() noexcept>(
    NativeSfcPreparationFor<NativeTemporarySurfaceOwner>&,NativeFrameDataRead&,
    const NativeSfcDenoiseRequestFor<NativeTemporarySurfaceOwner>&,void*,uintptr_t,
    const TglOwnedSurfaceStorage&,const TglOwnedSurfaceStorage&,TglLeasedAdmissionExecution&,
    const TglVeboxAdmissionStagesInput&,bool(*)() noexcept) noexcept;
template bool NativeSfcAdmission::run<NativeFrameDataRead,bool(*)() noexcept>(
    NativeSfcPreparation&,NativeFrameDataRead&,const NativeSfcDenoiseRequest&,void*,uintptr_t,
    const TglOwnedSurfaceStorage&,const TglOwnedSurfaceStorage&,TglLeasedAdmissionExecution&,
    const TglVeboxAdmissionStagesInput&,bool(*)() noexcept) noexcept;
struct NativeAdmissionReset {
    NativeRendererChildOwner& childOwner;
    NativeExecutionOwner& executionOwner;
    NativeRendererBacking& backing;
    NativeModeRetirementAuthority& retirement;
    size_t index;void* child;uintptr_t execution;void* vp;
    NativePreparedVpReset* preparedReset=nullptr;NativeVpGraph* borrowedGraph=nullptr;
    bool operator()() noexcept {
        const auto address=reinterpret_cast<uintptr_t>(child);
        const uint64_t generation=childOwner.allocationGeneration();
        if(index>1 || !child || !vp || !generation || !retirement.scope || !retirement.admit ||
            backing.retention()() || !childOwner.hasPrivateChildLease(address) ||
            !executionOwner.hasExecutionLease(execution))return false;
        auto admitRetirement=[&]() noexcept -> int {
            if(backing.retention()() || childOwner.allocationGeneration()!=generation ||
                !childOwner.hasPrivateChildLease(address) || !executionOwner.hasExecutionLease(execution))return 5;
            const int status=retirement();
            return status?status:(!backing.retention()() && childOwner.allocationGeneration()==generation &&
                childOwner.hasPrivateChildLease(address) && executionOwner.hasExecutionLease(execution)?0:5);
        };
        using VpReset=TglOwnedVeboxPersistentVpAdmissionReset<NativeRendererChildOwner,
            NativeRendererBacking,decltype(admitRetirement)>;
        VpReset legacy{childOwner,backing,index,child,vp,admitRetirement};
        auto reset=[&]() noexcept -> bool {
            if(preparedReset)return borrowedGraph && preparedReset->consume(childOwner,index,child,vp,*borrowedGraph);
            return !borrowedGraph && legacy();
        };
        TglOwnedVeboxAdmissionResetStage<NativeRendererChildOwner,NativeExecutionOwner,decltype(reset)>
            stage{childOwner,executionOwner,child,execution,reset};
        const bool accepted=stage();
        return accepted && childOwner.allocationGeneration()==generation &&
            childOwner.hasPrivateChildLease(address) && executionOwner.hasExecutionLease(execution) &&
            !backing.retention()();
    }
};
using NativeAdmissionParams=TglBorrowedVeboxAdmissionPacketView<0x139>;
using NativeAdmissionPass=TglBorrowedVeboxAdmissionPacketView<0x18,8>;
template<class SurfaceOwner> class NativeAdmissionPacketScopeFor {
    static_assert(std::is_same_v<SurfaceOwner,NativeContextSurfaceOwner> ||
        std::is_same_v<SurfaceOwner,NativeTemporarySurfaceOwner>);
    static constexpr bool temporary=std::is_same_v<SurfaceOwner,NativeTemporarySurfaceOwner>;
    NativeCoreFrameDataScopeFor<SurfaceOwner>& core_;
    const TglVeboxAdmissionBorrowedInterfaces interfaces_;
    uintptr_t parameters_,passAddress_;
    NativeAdmissionParams params_;
    NativeAdmissionPass pass_;
    TglBorrowedVeboxByteView<12> fill_;
    TglBorrowedVeboxByteView<8> alpha_;
    bool attempted_=false;
    bool same() const noexcept {
        if(!core_.live() || !params_.data() || !pass_.data())return false;
        uintptr_t source=0,target=0,e0=0,f0=0;
        bool roots=false;
        if constexpr(temporary)roots=tglMatchNativePerPassSelectedRoots(parameters_,params_.size(),
            passAddress_,0x40,reinterpret_cast<uintptr_t>(&core_.source),
            reinterpret_cast<uintptr_t>(&core_.target),&readMemory);
        else roots=readMemory(parameters_+8,&source,8) && source==reinterpret_cast<uintptr_t>(&core_.source) &&
            readMemory(parameters_+0x98,&target,8) && target==reinterpret_cast<uintptr_t>(&core_.target);
        return roots &&
            readMemory(parameters_+0xe0,&e0,8) && e0==interfaces_.paramsE0 &&
            readMemory(parameters_+0xf0,&f0,8) && f0==interfaces_.paramsF0 && core_.live();
    }
public:
    NativeAdmissionPacketScopeFor(NativeCoreFrameDataScopeFor<SurfaceOwner>& core,
        const TglVeboxAdmissionBorrowedInterfaces& interfaces,uintptr_t parameters,uintptr_t pass) noexcept
        :core_(core),interfaces_(interfaces),parameters_(parameters),passAddress_(pass) {}
    NativeAdmissionPacketScopeFor(const NativeAdmissionPacketScopeFor&)=delete;
    NativeAdmissionPacketScopeFor& operator=(const NativeAdmissionPacketScopeFor&)=delete;
    template<class Use> int run(Use use) noexcept {
        static_assert(noexcept(use(params_,pass_,fill_,alpha_)) &&
            std::is_same_v<decltype(use(params_,pass_,fill_,alpha_)),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(attempted_ || !core_.live())return 5;
        attempted_=true;
        auto readable=[](uintptr_t p,size_t n) noexcept {
            return leafMemoryPermissions(p,n,VM_PROT_READ,VM_PROT_EXECUTE);
        };
        if constexpr(temporary)if(passAddress_>UINTPTR_MAX-0x40 || !readable(passAddress_,0x40))return 5;
        if(!params_.bind(parameters_,0x139,readable,&writableDataMemory) ||
            !pass_.bind(passAddress_,0x18,readable,&writableDataMemory) ||
            (passAddress_<parameters_+params_.size() && parameters_<passAddress_+(temporary?0x40:pass_.size())) || !same() ||
            (interfaces_.paramsE0 && !fill_.bind(interfaces_.paramsE0,12,readable,&writableDataMemory)) ||
            (interfaces_.paramsF0 && !alpha_.bind(interfaces_.paramsF0,8,readable,&writableDataMemory)))return 5;
        // Original synchronous packet lifetime belongs to the enclosing native
        // call; these views neither retain pointers beyond it nor own copies.
        const int status=use(params_,pass_,fill_,alpha_);
        if(status)return status;
        return same()?0:5;
    }
};
using NativeAdmissionPacketScope=NativeAdmissionPacketScopeFor<NativeContextSurfaceOwner>;
template class NativeAdmissionPacketScopeFor<NativeTemporarySurfaceOwner>;
template int NativeAdmissionPacketScopeFor<NativeTemporarySurfaceOwner>::run<
    int(*)(NativeAdmissionParams&,NativeAdmissionPass&,TglBorrowedVeboxByteView<12>&,
        TglBorrowedVeboxByteView<8>&) noexcept>(int(*)(NativeAdmissionParams&,NativeAdmissionPass&,
        TglBorrowedVeboxByteView<12>&,TglBorrowedVeboxByteView<8>&) noexcept) noexcept;
template int NativeAdmissionPacketScope::run<int(*)(NativeAdmissionParams&,NativeAdmissionPass&,
    TglBorrowedVeboxByteView<12>&,TglBorrowedVeboxByteView<8>&) noexcept>(
    int(*)(NativeAdmissionParams&,NativeAdmissionPass&,TglBorrowedVeboxByteView<12>&,
        TglBorrowedVeboxByteView<8>&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeAdmissionFrameDataScopeFor {
    NativeCoreFrameDataScopeFor<SurfaceOwner>& core;NativeAdmissionParams& params;NativeAdmissionPass& pass;
    const TglVeboxAdmissionBorrowedInterfaces& interfaces;
    NativeFrameDataRead capability() noexcept {return {this,&resolve};}
    static bool resolve(void* context,uintptr_t address,void* output,size_t bytes) noexcept {
        auto* self=static_cast<NativeAdmissionFrameDataScopeFor*>(context);
        if(!self || !output || !bytes || !self->core.live() || !self->params.data() || !self->pass.data())return false;
        if(NativeCoreFrameDataScopeFor<SurfaceOwner>::resolve(&self->core,address,output,bytes))return true;
        const auto p=reinterpret_cast<uintptr_t>(self->params.data());
        const auto q=reinterpret_cast<uintptr_t>(self->pass.data());
        bool admitted=NativeCoreFrameDataScope::contains(p,self->params.size(),address,bytes) ||
            NativeCoreFrameDataScope::contains(q,self->pass.size(),address,bytes);
        if(!admitted) {
            uintptr_t os=0,hal=0,sku=0;
            auto& core=self->core;
            if(!core.childOwner.readLeasedStorage(core.child,core.generation,core.child+0x10,&os,8) ||
                !core.childOwner.readLeasedStorage(core.child,core.generation,core.child+0x18,&hal,8) ||
                !core.childOwner.readLeasedStorage(core.child,core.generation,core.child+0x20,&sku,8) ||
                !os || !hal || !sku || os!=self->interfaces.os || hal!=self->interfaces.renderHal ||
                sku!=self->interfaces.sku)return false;
            // Exact fields consumed by admission SKU policy and the qualified
            // six-argument platform send. Do not grant entire native objects.
            admitted=NativeCoreFrameDataScope::contains(sku,8,address,bytes) ||
                (sku<=UINTPTR_MAX-0x6c && NativeCoreFrameDataScope::contains(sku+0x64,8,address,bytes)) ||
                (hal<=UINTPTR_MAX-0x20 && NativeCoreFrameDataScope::contains(hal+0x18,8,address,bytes));
        }
        return admitted && readMemory(address,output,bytes) && self->core.live();
    }
};
using NativeAdmissionFrameDataScope=NativeAdmissionFrameDataScopeFor<NativeContextSurfaceOwner>;
// Created view is borrowed from the same pinned VPHAL access callback.
// Child8 is the proven renderer+2cb0 configuration container; only its first
// pointer and AVS's two performance DWORDs are exposed, never a whole object.
template<class SurfaceOwner> struct NativeRendererPerformanceReadFor {
    NativeAdmissionFrameDataScopeFor<SurfaceOwner>& base;const TglNativeCreatedVeboxRendererView& view;
    NativeFrameDataRead capability() noexcept {return {this,&resolve};}
    static bool resolve(void* context,uintptr_t address,void* output,size_t bytes) noexcept {
        auto* self=static_cast<NativeRendererPerformanceReadFor*>(context);
        if(!self || !output || !bytes || !self->base.core.live())return false;
        if(NativeAdmissionFrameDataScopeFor<SurfaceOwner>::resolve(&self->base,address,output,bytes))return true;
        auto& core=self->base.core;
        if(self->view.image!=core.image || !self->view.renderer || self->view.renderer>UINTPTR_MAX-0x2cb8)return false;
        const uintptr_t slot=self->view.renderer+0x2cb0;
        uintptr_t configured=0;
        if(!core.childOwner.readLeasedStorage(core.child,core.generation,core.child+8,&configured,8) || configured!=slot)return false;
        if(NativeCoreFrameDataScope::contains(slot,8,address,bytes))
            return readMemory(address,output,bytes) && core.live();
        uintptr_t performance=0,published=0;
        if(!readMemory(slot,&performance,8) || !performance || performance>UINTPTR_MAX-0x10)return false;
        std::memcpy(&published,core.vp.bytes.data()+0x9c8,8);
        if(published!=performance || !NativeCoreFrameDataScope::contains(performance+8,8,address,bytes))return false;
        if(!readMemory(address,output,bytes) || !core.live())return false;
        uintptr_t after=0;std::memcpy(&published,core.vp.bytes.data()+0x9c8,8);
        return readMemory(slot,&after,8) && after==performance && published==performance;
    }
};
using NativeRendererPerformanceRead=NativeRendererPerformanceReadFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeOptionalVpMetadataReadFor {
    NativeRendererPerformanceReadFor<SurfaceOwner>& base;
    const TglBorrowedVeboxByteView<12>& fill;
    const TglBorrowedVeboxByteView<8>& alpha;
    NativeFrameDataRead capability() noexcept {return {this,&resolve};}
    static bool resolve(void* context,uintptr_t address,void* output,size_t bytes) noexcept {
        auto* self=static_cast<NativeOptionalVpMetadataReadFor*>(context);
        if(!self || !output || !bytes || !self->base.base.core.live())return false;
        if(NativeRendererPerformanceReadFor<SurfaceOwner>::resolve(&self->base,address,output,bytes))return true;
        auto& admission=self->base.base;
        uintptr_t e0=0,f0=0;
        if(!admission.params.data() || !readMemory(reinterpret_cast<uintptr_t>(admission.params.data())+0xe0,&e0,8) ||
            !readMemory(reinterpret_cast<uintptr_t>(admission.params.data())+0xf0,&f0,8) ||
            e0!=admission.interfaces.paramsE0 || f0!=admission.interfaces.paramsF0)return false;
        const auto fill=reinterpret_cast<uintptr_t>(self->fill.data());
        const auto alpha=reinterpret_cast<uintptr_t>(self->alpha.data());
        const bool admitted=(e0 && e0==fill && NativeCoreFrameDataScope::contains(fill,self->fill.size(),address,bytes)) ||
            (f0 && f0==alpha && NativeCoreFrameDataScope::contains(alpha,self->alpha.size(),address,bytes));
        if(!admitted || !readMemory(address,output,bytes) || !admission.core.live())return false;
        uintptr_t afterE0=0,afterF0=0;
        return readMemory(reinterpret_cast<uintptr_t>(admission.params.data())+0xe0,&afterE0,8) &&
            readMemory(reinterpret_cast<uintptr_t>(admission.params.data())+0xf0,&afterF0,8) && afterE0==e0 && afterF0==f0;
    }
};
using NativeOptionalVpMetadataRead=NativeOptionalVpMetadataReadFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeSfcFrameDataReadFor {
    NativeOptionalVpMetadataReadFor<SurfaceOwner>& base;const TglSfcMhwBinding& binding;
    NativeFrameDataRead capability() noexcept {return {this,&resolve};}
    static bool resolve(void* context,uintptr_t address,void* output,size_t bytes) noexcept {
        auto* self=static_cast<NativeSfcFrameDataReadFor*>(context);
        if(!self || !output || !bytes)return false;
        auto& core=self->base.base.base.core;
        if(!core.live())return false;
        if(NativeOptionalVpMetadataReadFor<SurfaceOwner>::resolve(&self->base,address,output,bytes))return true;
        uintptr_t linked=0,mhw=0;
        if(!core.childOwner.readLeasedStorage(core.child,core.generation,core.child+0x80,&linked,8) ||
            linked!=reinterpret_cast<uintptr_t>(core.vp.bytes.data()))return false;
        std::memcpy(&mhw,core.vp.bytes.data()+0x20,8);
        if(!mhw || mhw>UINTPTR_MAX-0x2c)return false;
        bool read=false;
        if(NativeCoreFrameDataScope::contains(mhw+0x18,4,address,bytes))
            read=core.childOwner.readLeasedSfcAlignment(reinterpret_cast<void*>(core.child),self->binding,
                address,output,bytes,&readMemory);
        else if(NativeCoreFrameDataScope::contains(mhw+0x1c,16,address,bytes))
            read=core.childOwner.readLeasedSfcLimits(reinterpret_cast<void*>(core.child),self->binding,
                address,output,bytes,&readMemory);
        if(!read || !core.live())return false;
        uintptr_t after=0;std::memcpy(&after,core.vp.bytes.data()+0x20,8);
        return after==mhw && core.childOwner.readLeasedStorage(core.child,core.generation,core.child+0x80,&linked,8) &&
            linked==reinterpret_cast<uintptr_t>(core.vp.bytes.data());
    }
};
using NativeSfcFrameDataRead=NativeSfcFrameDataReadFor<NativeContextSurfaceOwner>;
// One synchronous owner for the closed reader chain. Producers receive the
// SAME capability object; its backing scopes cannot move while they use it.
template<class SurfaceOwner> class NativeCompleteFrameDataScopeFor {
    NativeCoreFrameDataScopeFor<SurfaceOwner> core_;
    NativeAdmissionFrameDataScopeFor<SurfaceOwner> admission_;
    NativeRendererPerformanceReadFor<SurfaceOwner> performance_;
    NativeOptionalVpMetadataReadFor<SurfaceOwner> optional_;
    TglSfcMhwBinding sfcBinding_;
    NativeSfcFrameDataReadFor<SurfaceOwner> sfc_;
    NativeFrameDataRead read_;
    bool attempted_=false;
public:
    NativeCompleteFrameDataScopeFor(const NativeCoreFrameDataScopeFor<SurfaceOwner>& core,NativeAdmissionParams& params,
        NativeAdmissionPass& pass,const TglVeboxAdmissionBorrowedInterfaces& interfaces,
        const TglNativeCreatedVeboxRendererView& pinned,const TglBorrowedVeboxByteView<12>& fill,
        const TglBorrowedVeboxByteView<8>& alpha) noexcept
        :core_(core),admission_{core_,params,pass,interfaces},performance_{admission_,pinned},
         optional_{performance_,fill,alpha},sfc_{optional_,sfcBinding_},read_(sfc_.capability()) {}
    NativeCompleteFrameDataScopeFor(const NativeCompleteFrameDataScopeFor&)=delete;
    NativeCompleteFrameDataScopeFor& operator=(const NativeCompleteFrameDataScopeFor&)=delete;
    template<class Use> int run(Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeFrameDataRead&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeFrameDataRead&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(attempted_ || !core_.live() || performance_.view.image!=core_.image ||
            !admission_.params.data() || !admission_.pass.data())return 5;
        attempted_=true;
        if(!sfcBinding_.resolve(core_.image,&readMemory,NativeTglImageAdmission{core_.image},&executableMemory))return 5;
        const int status=use(read_);
        if(status)return status; // retain native error, never manufacture success
        return core_.live()?0:5;
    }
};
using NativeCompleteFrameDataScope=NativeCompleteFrameDataScopeFor<NativeContextSurfaceOwner>;
template class NativeCompleteFrameDataScopeFor<NativeTemporarySurfaceOwner>;
template int NativeCompleteFrameDataScopeFor<NativeTemporarySurfaceOwner>::run<int(*)(NativeFrameDataRead&) noexcept>(
    int(*)(NativeFrameDataRead&) noexcept) noexcept;
template int NativeCompleteFrameDataScope::run<int(*)(NativeFrameDataRead&) noexcept>(
    int(*)(NativeFrameDataRead&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeLeasedFrameInputScopeFor {
    struct Inputs {
        NativeRendererCpuAssembly& assembly;size_t index;
        SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
        const TglNativeCreatedVeboxRendererView& pinned;
        const TglVeboxAdmissionBorrowedInterfaces& interfaces;
        uintptr_t parameters,pass;
        NativeModeRetirementAuthority* resetAuthority=nullptr;
    };
    template<class Use> static TglOwnedVeboxCommandTransactionResult run(Inputs& in,Use use) noexcept {
        if constexpr(std::is_pointer_v<Use>)if(!use)return {};
        if(in.index>1 || !in.pinned.creator || !in.pinned.renderer)return {};
        auto prepare=[&](auto& owner,auto& execution,void* child,uintptr_t report,auto enter) noexcept -> int {
            if(!in.resetAuthority)return enter(nullptr);
            const auto generation=owner.allocationGeneration();uintptr_t vp=0;
            auto live=[&]() noexcept {
                return in.index<2 && in.pinned.originalChildren[in.index]==reinterpret_cast<uintptr_t>(child) &&
                    generation && owner.allocationGeneration()==generation && execution.hasExecutionLease(report) &&
                    owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)) && !in.assembly.backing().retention()();
            };
            if(!live() || !owner.readLeasedStorage(reinterpret_cast<uintptr_t>(child),generation,
                reinterpret_cast<uintptr_t>(child)+0x80,&vp,8) || !vp)return 5;
            auto retire=[&]() noexcept -> int {
                if(!live())return 5;
                const int status=(*in.resetAuthority)();return status?status:live()?0:5;
            };
            return NativePreparedVpReset::run(owner,in.assembly.backing(),in.index,child,reinterpret_cast<void*>(vp),
                retire,[&](NativePreparedVpReset& reset) noexcept -> int {return live()?enter(&reset):5;});
        };
        return in.assembly.withChildVpResourcesPrepared(in.index,prepare,[&](auto& owner,auto& execution,void* child,
            uintptr_t report,auto& graph,auto& vp,auto& lines,NativePreparedVpReset* reset) noexcept -> TglOwnedVeboxCommandTransactionResult {
            TglOwnedVeboxCommandTransactionResult result;
            const auto* source=in.sourceOwner.surfaceBinding();
            const auto* target=in.targetOwner.surfaceBinding();
            if(in.pinned.originalChildren[in.index]!=reinterpret_cast<uintptr_t>(child) ||
                !source || !target || !in.sourceOwner.hasSurfaceLease(*source) ||
                !in.targetOwner.hasSurfaceLease(*target))return result;
            NativeCoreFrameDataScopeFor<SurfaceOwner> core{owner,execution,in.sourceOwner,in.targetOwner,*source,*target,vp,
                in.pinned.image,reinterpret_cast<uintptr_t>(child),report,owner.allocationGeneration(),reset};
            NativeAdmissionPacketScopeFor<SurfaceOwner> packets(core,in.interfaces,in.parameters,in.pass);
            const int status=packets.run([&](NativeAdmissionParams& params,NativeAdmissionPass& pass,
                TglBorrowedVeboxByteView<12>& fill,TglBorrowedVeboxByteView<8>& alpha) noexcept -> int {
                NativeCompleteFrameDataScopeFor<SurfaceOwner> data(core,params,pass,in.interfaces,in.pinned,fill,alpha);
                return data.run([&](NativeFrameDataRead& read) noexcept -> int {
                    static_assert(noexcept(use(core,graph,lines,params,pass,read)) &&
                        std::is_same_v<decltype(use(core,graph,lines,params,pass,read)),
                            TglOwnedVeboxCommandTransactionResult>);
                    result=use(core,graph,lines,params,pass,read);
                    return result.status;
                });
            });
            if(status)result.status=status;
            return result;
        });
    }
};
using NativeLeasedFrameInputScope=NativeLeasedFrameInputScopeFor<NativeContextSurfaceOwner>;
using NativeTemporaryFrameInputScope=NativeLeasedFrameInputScopeFor<NativeTemporarySurfaceOwner>;
using NativeTemporaryFrameInputUse=TglOwnedVeboxCommandTransactionResult(*)(
    NativeCoreFrameDataScopeFor<NativeTemporarySurfaceOwner>&,NativeVpGraph&,TglNativeVpLineResourceStorage&,
    NativeAdmissionParams&,NativeAdmissionPass&,NativeFrameDataRead&) noexcept;
template TglOwnedVeboxCommandTransactionResult NativeTemporaryFrameInputScope::run<NativeTemporaryFrameInputUse>(
    NativeTemporaryFrameInputScope::Inputs&,NativeTemporaryFrameInputUse) noexcept;
using NativeLeasedFrameInputUse=TglOwnedVeboxCommandTransactionResult(*)(NativeCoreFrameDataScope&,
    NativeVpGraph&,TglNativeVpLineResourceStorage&,NativeAdmissionParams&,NativeAdmissionPass&,NativeFrameDataRead&) noexcept;
template TglOwnedVeboxCommandTransactionResult NativeLeasedFrameInputScope::run<NativeLeasedFrameInputUse>(
    NativeLeasedFrameInputScope::Inputs&,NativeLeasedFrameInputUse) noexcept;
// One synchronous join: original packet/surface leases and the closed reader
// enclose the actual VP producers. No caller-supplied replacement geometry,
// compression flags or detached producer scope crosses this boundary.
struct NativeLeasedVpFrameInputScope {
    using Use=TglOwnedVeboxCommandTransactionResult(*)(void*,NativeCoreFrameDataScope&,
        NativeVpGraph&,TglNativeVpLineResourceStorage&,NativeAdmissionParams&,NativeAdmissionPass&,NativeFrameDataRead&,
        NativeVpProducerFrame&,const TglOwnedVpModeState&) noexcept;
    static TglOwnedVeboxCommandTransactionResult run(NativeLeasedFrameInputScope::Inputs& in,
        const TglOwnedVpEntrySet& catalogue,void* context,Use use) noexcept {
        if(!use)return {};
        return NativeLeasedFrameInputScope::run(in,[&](NativeCoreFrameDataScope& core,
            NativeVpGraph& graph,TglNativeVpLineResourceStorage& lines,NativeAdmissionParams& params,
            NativeAdmissionPass& pass,NativeFrameDataRead& read) noexcept -> TglOwnedVeboxCommandTransactionResult {
            NativeVpProducerInputScope::Inputs inputs{core,read,in.pinned,in.interfaces,lines,catalogue};
            NativeVpProducerInputScope producers(inputs);
            return producers.run([&](NativeVpProducerFrame& vp,const TglOwnedVpModeState& original) noexcept {
                return use(context,core,graph,lines,params,pass,read,vp,original);
            });
        });
    }
};
template<class SurfaceOwner> using NativeAdmissionSourceGraphFor=std::conditional_t<
    std::is_same_v<SurfaceOwner,NativeContextSurfaceOwner>,NativeSourceGraph,NativeTemporaryHistoryGraph>;
template<class SurfaceOwner> struct NativeAdmissionStagesFor {
    NativeAdmissionReset& reset;NativeOutputPipeSelectorFor<SurfaceOwner>& select;
    NativeAdmissionHistorySetupFor<NativeAdmissionSourceGraphFor<SurfaceOwner>>& history;
    NativeAdmissionFeatureStageFor<SurfaceOwner>& feature;
    NativeSfcPreparationFor<SurfaceOwner>& sfc;const NativeSfcDenoiseRequestFor<SurfaceOwner>& dn;
    NativeAdmissionParams& params;NativeAdmissionPass& pass;
    const TglVeboxAdmissionBorrowedInterfaces& interfaces;NativeFrameDataRead& read;
    bool compatible() const noexcept {
        return reset.child && reset.child==feature.child && reset.child==history.stage.child &&
            reset.child==sfc.child && reset.execution==feature.execution &&
            reset.execution==history.stage.execution && reset.index==sfc.index &&
            &reset.backing==&sfc.backing && &reset.childOwner==&feature.childOwner &&
            &reset.childOwner==&history.stage.childOwner && &reset.childOwner==&sfc.childOwner &&
            &reset.executionOwner==&feature.executionOwner &&
            &reset.executionOwner==&history.stage.executionOwner && &reset.executionOwner==&sfc.executionOwner &&
            &feature.sourceOwner==&sfc.sourceOwner && &feature.targetOwner==&sfc.targetOwner &&
            &feature.source==&history.stage.source && &dn.owner==&feature.sourceOwner &&
            &dn.parameters==&feature.parameters && feature.sourceOwner.hasSurfaceLease(feature.source) &&
            feature.targetOwner.hasSurfaceLease(feature.target) &&
            select.matchesFrame(read,feature.binding.image,reinterpret_cast<uintptr_t>(reset.child),
                reset.vp,feature.source,feature.target,params.data()) &&
            tglHasNativeDenoiseAdmissionGrant(feature.sourceOwner,feature.source,feature.parameters) &&
            reset.executionOwner.hasLeasedFeatureReport(reset.execution,&feature.target);
    }
    struct Result {TglOwnedVeboxCommandTransactionResult native{};int status=5;bool admitted=false;};
    template<class Render>
    Result run(Render& render) noexcept {
        static_assert(noexcept(render()) &&
            std::is_same_v<decltype(render()),TglOwnedVeboxCommandTransactionResult>);
        Result result;
        if constexpr(std::is_pointer_v<Render>)if(!render)return result;
        // Called by the owned outer frame, INSIDE its original surface/DN/
        // feature-report scopes. Do not use this as a bool-only callback that
        // returns then calls native Render a second time outside VP scopes.
        if(!read.scope || !read.resolve || !compatible() ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(read.resolve),1))return result;
        const uint64_t generation=reset.childOwner.allocationGeneration();
        if(!generation)return result;
        int loanStatus=5;
        auto pausedRender=[&]() noexcept {
            TglOwnedVeboxCommandTransactionResult native;
            loanStatus=reset.executionOwner.withAdmissionContinuation(reset.execution,[&]() noexcept -> int {
                native=render();return native.status;
            });
            return native;
        };
        TglOwnedVeboxAdmissionRenderContinuation continuation(pausedRender,reset.backing.retention());
        bool admitted=false;
        const int status=reset.executionOwner.withLeasedAdmissionBytes(reset.execution,
            [&](TglLeasedAdmissionExecution& report) noexcept -> int {
                auto resetStage=[&]() noexcept {return reset();};
                auto selectStage=[&](uint8_t& flag) noexcept {return select(flag);};
                auto setupStage=[&](TglLeasedAdmissionExecution& active) noexcept {return history(active);};
                auto featureStage=[&](TglLeasedAdmissionExecution& active) noexcept {
                    return NativeAdmissionFeaturePreparationFor<SurfaceOwner>::apply(feature,active);
                };
                auto sfcStage=[&](TglLeasedAdmissionExecution& active,const TglVeboxAdmissionStagesInput& input,
                    auto finish) noexcept -> bool {
                    return NativeSfcAdmissionFor<SurfaceOwner>::run(sfc,read,dn,reset.vp,reset.execution,
                        feature.source,feature.target,active,input,finish);
                };
                auto retained=[&](auto next) noexcept -> bool {return continuation(next);};
                admitted=tglRunCapturedSelectedVeboxAdmissionStages(reset.childOwner,reset.executionOwner,
                    reset.child,params,pass,feature.source,feature.target,interfaces,report,read,
                    resetStage,selectStage,setupStage,featureStage,sfcStage,retained);
                return admitted?0:5;
            });
        result.native=continuation.nativeResult();
        result.admitted=!status && admitted && !loanStatus && !continuation.status() &&
            reset.childOwner.allocationGeneration()==generation && compatible();
        result.status=result.native.nativeEntered && result.native.status?result.native.status:
            result.admitted?0:5;
        return result;
    }
};
using NativeAdmissionStages=NativeAdmissionStagesFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> class NativeAdmissionFrameAssemblyFor {
public:
    struct Inputs {
        NativeRendererChildOwner& childOwner;NativeExecutionOwner& executionOwner;
        NativeRendererBacking& backing;NativeModeRetirementAuthority& retirement;
        SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
        NativeAdmissionSourceGraphFor<SurfaceOwner>& sourceGraph;NativeDenoiseParameters& parameters;
        const TglOwnedSurfaceStorage& source;const TglOwnedSurfaceStorage& target;
        const TglVeboxFeatureBinding& featureBinding;
        NativeAdmissionParams& params;NativeAdmissionPass& pass;
        const TglVeboxAdmissionBorrowedInterfaces& interfaces;
        const TglOwnedVpEntrySet& catalogue;const TglOwnedVpModeState& mode;
        size_t index;void* child;uintptr_t execution;void* vp;uintptr_t mhw;void* renderData;
        NativeVpGraph* borrowedGraph=nullptr;TglOwnedVpSfcCallerStorage* borrowedStorage=nullptr;
        NativePreparedVpReset* preparedReset=nullptr;
    };
private:
    NativeAdmissionReset reset_;
    NativeAdmissionHistorySetupFor<NativeAdmissionSourceGraphFor<SurfaceOwner>> history_;
    NativeAdmissionFeatureStageFor<SurfaceOwner> feature_;
    NativeSfcPreparationFor<SurfaceOwner> sfc_;
    NativeSfcDenoiseRequestFor<SurfaceOwner> dn_;
    NativeAdmissionStagesFor<SurfaceOwner> stages_;
    bool attempted_=false;
public:
    NativeAdmissionFrameAssemblyFor(const Inputs& in,NativeOutputPipeSelectorFor<SurfaceOwner>& select,NativeFrameDataRead& read) noexcept
        :reset_{in.childOwner,in.executionOwner,in.backing,in.retirement,in.index,in.child,in.execution,in.vp,
             in.preparedReset,in.borrowedGraph},
         history_{{in.childOwner,in.executionOwner,in.sourceGraph,in.child,in.execution,in.source,
             NativeDataAdmission{},NativeDataAdmission{}}},
         feature_{in.featureBinding,in.sourceOwner,in.targetOwner,in.childOwner,in.executionOwner,
             in.parameters,in.child,in.execution,in.source,in.target,&readMemory,
             NativeTglImageAdmission{in.featureBinding.image},&executableMemory},
         sfc_{in.backing,in.childOwner,in.executionOwner,in.sourceOwner,in.targetOwner,
             in.catalogue,in.mode,in.index,in.child,in.mhw,in.renderData,nullptr,nullptr,nullptr,nullptr,
             in.borrowedGraph,in.borrowedStorage},
         dn_{in.sourceOwner,in.parameters},
         stages_{reset_,select,history_,feature_,sfc_,dn_,in.params,in.pass,in.interfaces,read} {}
    NativeAdmissionFrameAssemblyFor(const NativeAdmissionFrameAssemblyFor&)=delete;
    NativeAdmissionFrameAssemblyFor& operator=(const NativeAdmissionFrameAssemblyFor&)=delete;
    template<class Use> int run(Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeAdmissionStagesFor<SurfaceOwner>&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeAdmissionStagesFor<SurfaceOwner>&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        if(attempted_ || !stages_.read.scope || !stages_.read.resolve || reset_.index>1)return 5;
        attempted_=true;
        // PreparedAdmissionRender installs real VP producer callbacks before
        // stages.run; null fields here are unbound, never success stubs.
        return use(stages_);
    }
};
using NativeAdmissionFrameAssembly=NativeAdmissionFrameAssemblyFor<NativeContextSurfaceOwner>;
template class NativeAdmissionFrameAssemblyFor<NativeTemporarySurfaceOwner>;
template int NativeAdmissionFrameAssemblyFor<NativeTemporarySurfaceOwner>::run<
    int(*)(NativeAdmissionStagesFor<NativeTemporarySurfaceOwner>&) noexcept>(
    int(*)(NativeAdmissionStagesFor<NativeTemporarySurfaceOwner>&) noexcept) noexcept;
template int NativeAdmissionFrameAssembly::run<int(*)(NativeAdmissionStages&) noexcept>(
    int(*)(NativeAdmissionStages&) noexcept) noexcept;
using NativeAdmissionRenderFn=TglOwnedVeboxCommandTransactionResult(*)() noexcept;
template NativeAdmissionStagesFor<NativeTemporarySurfaceOwner>::Result
    NativeAdmissionStagesFor<NativeTemporarySurfaceOwner>::run<NativeAdmissionRenderFn>(NativeAdmissionRenderFn&) noexcept;
template NativeAdmissionStages::Result NativeAdmissionStages::run<NativeAdmissionRenderFn>(
    NativeAdmissionRenderFn&) noexcept;
// One scoped path from admission through actual VP producers to native Render.
// Native Render's result is retained by admission before any callback/owner
// scope unwinds. This entry does NOT certify completion or release the frame.
template<class SurfaceOwner> struct NativeAdmissionHookContextFor {
    NativeAdmissionStagesFor<SurfaceOwner>& stages;bool active=false,consumed=false;
    bool owns(void* child) const noexcept {return child && child==stages.reset.child;}
    bool admit(void* params,void* pass) noexcept {
        if(!active || consumed || params!=stages.params.data() || pass!=stages.pass.data() ||
            !stages.compatible())return false;
        consumed=true;return true;
    }
};
using NativeAdmissionHookContext=NativeAdmissionHookContextFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeAdmissionResolveFor=
    TglOwnedVeboxScopedContext<NativeAdmissionHookContextFor<SurfaceOwner>>;
using NativeAdmissionResolve=NativeAdmissionResolveFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeDirectOutputCachePublicationFor {
    static bool apply(NativeAdmissionStagesFor<SurfaceOwner>& stages,uintptr_t image) noexcept {
        auto query=[&](uintptr_t os,uint32_t policy,uint32_t& output) noexcept -> bool {
            TglNativeRendererCacheBinding binding;
            return binding.resolve(image,os,&readMemory,NativeTglImageAdmission{image},&executableMemory) &&
                binding.query(policy,output,&readMemory,TglNativeRendererCacheInvoker{});
        };
        return tglApplyLeasedVeboxDirectOutputCachePolicy(stages.reset.childOwner,stages.reset.executionOwner,
            reinterpret_cast<uintptr_t>(stages.reset.child),stages.reset.execution,image,stages.read,query);
    }
};
using NativeDirectOutputCachePublication=NativeDirectOutputCachePublicationFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativePreparedAdmissionRenderFor {
    NativeVpPreparedCallbacksFor<SurfaceOwner>& callbacks;NativeAdmissionStagesFor<SurfaceOwner>& admission;
    const TglVeboxRenderBinding& binding;void* argument;
    bool attempted=false;
    typename NativeAdmissionStagesFor<SurfaceOwner>::Result run() noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        if(attempted || !callbacks.sameOwners() || !admission.compatible() ||
            binding.image!=callbacks.scalar.image || !binding.entry ||
            admission.reset.child!=callbacks.emission.parent || admission.reset.vp!=callbacks.scalar.vp ||
            &admission.reset.childOwner!=&callbacks.emission.childOwner ||
            &admission.feature.sourceOwner!=&callbacks.filters.sourceOwner ||
            &admission.feature.targetOwner!=&callbacks.packet.targetOwner ||
            &admission.feature.source!=&callbacks.scalar.source ||
            &admission.feature.target!=&callbacks.scalar.target ||
            &admission.read!=&callbacks.scalar.dataRead)return result;
        attempted=true;
        struct Call {NativePreparedAdmissionRenderFor& self;
            typename NativeAdmissionStagesFor<SurfaceOwner>::Result& result;};
        Call call{*this,result};
        const int status=NativeVpPreparationScopeFor<SurfaceOwner>::run(callbacks,admission.sfc,&call,
            [](void* context,NativeSfcPreparationFor<SurfaceOwner>& preparation) noexcept -> int {
                auto& call=*static_cast<Call*>(context);auto& self=call.self;
                if(&preparation!=&self.admission.sfc)return 5;
                NativeAdmissionHookContextFor<SurfaceOwner> hook{self.admission};
                NativeAdmissionResolveFor<SurfaceOwner> admissionScope(self.admission.reset.child,hook);
                if(!admissionScope.bound())return 5;
                auto render=[&]() noexcept -> TglOwnedVeboxCommandTransactionResult {
                    // Native admission already finished the selected stages.
                    // Any native hook query observes this exact continuation;
                    // it cannot recursively execute admission or another Render.
                    if(!NativeDirectOutputCachePublicationFor<SurfaceOwner>::apply(self.admission,self.binding.image))
                        return TglOwnedVeboxCommandTransactionResult{};
                    hook.active=true;
                    struct Restore {bool& active;~Restore(){active=false;}} restore{hook.active};
                    return tglRunLeasedVeboxSurfaceRender(self.binding,self.admission.reset.childOwner,
                        self.admission.reset.child,self.argument,self.admission.feature.sourceOwner,
                        self.admission.feature.source,self.admission.feature.targetOwner,self.admission.feature.target);
                };
                call.result=self.admission.run(render);
                return call.result.status;
            });
        if(status) {result.status=status;result.admitted=false;}
        return result;
    }
};
using NativePreparedAdmissionRender=NativePreparedAdmissionRenderFor<NativeContextSurfaceOwner>;
template struct NativePreparedAdmissionRenderFor<NativeTemporarySurfaceOwner>;
using NativeSurfaceFrameBacking=TglNativeVeboxSurfaceFrameStorage<TglDarwinStmmSurfaceApi>;
using NativeVpLines=std::remove_reference_t<decltype(std::declval<TglNativeVpLineResourceStorage&>().resources())>;
using NativeModeReleaseContext=TglOwnedVeboxModeReleaseContext<NativeSurfaceFrameBacking,
    NativeVpLines,NativeModeRetirementAuthority>;
using NativeModeReleaseScope=TglOwnedVeboxScopedContext<NativeModeReleaseContext>;
struct NativeModeCallbackScope {
    template<class Use> static int run(NativeSurfaceFrameBacking& backing,NativeVpLines& lines,
        NativeSfcReleaseSelection& selection,NativeModeRetirementAuthority& authority,Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeModeReleaseContext&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeModeReleaseContext&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        const uintptr_t child=reinterpret_cast<uintptr_t>(selection.child);
        const uint64_t generation=selection.owner.allocationGeneration();
        if(!authority.scope || !authority.admit || !generation || !child ||
            !selection.owner.hasPrivateChildLease(child) || backing.auxiliary().retention()())return 5;
        NativeModeReleaseContext mode(selection.child,backing,lines,selection,authority);
        NativeModeReleaseScope scope(selection.child,mode);
        if(!scope.bound())return 5;
        // Consumer inspects mode.status() after any attempted unwind; refusal
        // requires retaining its frame. This scope never certifies completion.
        const int status=use(mode);
        if(!selection.owner.hasPrivateChildLease(child) || selection.owner.allocationGeneration()!=generation)
            return status?status:5;
        return status?status:(mode.releaseAttempted()?mode.status():0);
    }
};
// The context and its frame-local authority must enclose the native callback.
// Preserve native allocation failure; caller also inspects mode.status() and
// retains the frame on any refused/partial cleanup. No synchronous-success default.
struct NativeAllocationUnwind {
    NativeModeReleaseContext& mode;
    void operator()(void* child,uintptr_t,const TglOwnedSurfaceStorage&) const noexcept {
        if(mode.owns(child))mode.freeResources();
    }
};
template<class SurfaceOwner> using NativeAllocationPlannerFor=TglOwnedVeboxAllocationFramePlanner<TglDarwinStmmSurfaceApi,
    SurfaceOwner,NativeRendererChildOwner,NativeExecutionOwner,decltype(&readMemory),
    NativeAllocationSfcFormat,NativeAllocationSfcColor,NativeAllocationParameterKindFor<SurfaceOwner>,
    TglOwnedFfdiCompressionFormatQuery,NativeAllocationRectangleFor<SurfaceOwner>,NativeDataAdmission,NativeAllocationUnwind>;
using NativeAllocationPlanner=NativeAllocationPlannerFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeAllocationContextFor=TglOwnedVeboxRenderAllocationContext<NativeRendererChildOwner,
    NativeAllocationPlannerFor<SurfaceOwner>>;
using NativeAllocationContext=NativeAllocationContextFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeAllocationResolveFor=TglOwnedVeboxScopedContext<NativeAllocationContextFor<SurfaceOwner>>;
using NativeAllocationResolve=NativeAllocationResolveFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeAllocationCallbackScopeFor {
    static int allocate(void* child) noexcept {
        return TglOwnedVeboxAllocationCallbacks<NativeAllocationResolveFor<SurfaceOwner>>::allocate(child);
    }
    template<class Use> static int run(NativeAllocationPlannerFor<SurfaceOwner>& planner,NativeModeReleaseContext& mode,
        TglOwnedVeboxFrameRetention& renderRetention,Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeAllocationContextFor<SurfaceOwner>&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeAllocationContextFor<SurfaceOwner>&>())),int>);
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        void* child=planner.child;
        const auto address=reinterpret_cast<uintptr_t>(child);
        if(!child || NativeModeReleaseScope::get(child)!=&mode || !mode.owns(child) ||
            &planner.unwind.mode!=&mode || !planner.childOwner.hasPrivateChildLease(address) ||
            !planner.executionOwner.hasExecutionLease(planner.execution))return 5;
        TglOwnedVeboxAllocationRenderRetention keep{renderRetention,planner.storage.auxiliary().retention()};
        if(keep())return 5;
        // The captured planner owns its allocation unwind. Do not execute
        // another cleanup from the callback wrapper or infer GPU retirement.
        NativeAllocationContextFor<SurfaceOwner> allocation(planner.childOwner,planner,child,{},keep);
        NativeAllocationResolveFor<SurfaceOwner> scope(child,allocation);
        if(!scope.bound() || !allocation.owns(child))return 5;
        const int status=use(allocation);
        return status?status:(allocation.owns(child) && NativeModeReleaseScope::get(child)==&mode?0:5);
    }
};
using NativeAllocationCallbackScope=NativeAllocationCallbackScopeFor<NativeContextSurfaceOwner>;
template struct NativeAllocationCallbackScopeFor<NativeTemporarySurfaceOwner>;
template int NativeAllocationCallbackScopeFor<NativeTemporarySurfaceOwner>::run<
    int(*)(NativeAllocationContextFor<NativeTemporarySurfaceOwner>&) noexcept>(
    NativeAllocationPlannerFor<NativeTemporarySurfaceOwner>&,NativeModeReleaseContext&,
    TglOwnedVeboxFrameRetention&,int(*)(NativeAllocationContextFor<NativeTemporarySurfaceOwner>&) noexcept) noexcept;
template int NativeAllocationCallbackScope::run<int(*)(NativeAllocationContext&) noexcept>(
    NativeAllocationPlanner&,NativeModeReleaseContext&,TglOwnedVeboxFrameRetention&,
    int(*)(NativeAllocationContext&) noexcept) noexcept;
template<class SurfaceOwner> struct NativeAllocatedAdmissionRenderFor {
    NativePreparedAdmissionRenderFor<SurfaceOwner>& render;
    NativeAllocationPlannerFor<SurfaceOwner>& planner;NativeModeReleaseContext& mode;
    bool attempted=false;
    typename NativeAdmissionStagesFor<SurfaceOwner>::Result run() noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        auto& admission=render.admission;
        if(attempted || planner.child!=admission.reset.child ||
            &planner.childOwner!=&admission.reset.childOwner ||
            &planner.executionOwner!=&admission.reset.executionOwner || planner.execution!=admission.reset.execution ||
            &planner.input!=&admission.feature.sourceOwner || planner.target!=&admission.feature.targetOwner ||
            &planner.source!=&admission.feature.source || &planner.unwind.mode!=&mode ||
            NativeModeReleaseScope::get(planner.child)!=&mode || !mode.owns(planner.child))return result;
        attempted=true;
        const int status=NativeAllocationCallbackScopeFor<SurfaceOwner>::run(planner,mode,admission.reset.backing.retention(),
            [&](NativeAllocationContextFor<SurfaceOwner>& allocation) noexcept -> int {
                if(!allocation.owns(planner.child))return 5;
                result=render.run();
                return result.status;
            });
        if(status) {result.status=status;result.admitted=false;}
        return result;
    }
};
using NativeAllocatedAdmissionRender=NativeAllocatedAdmissionRenderFor<NativeContextSurfaceOwner>;
template struct NativeAllocatedAdmissionRenderFor<NativeTemporarySurfaceOwner>;
template<class SurfaceOwner> struct NativeModeAllocationPlanFor {
    NativeSurfaceFrameBacking& storage;NativeVpLines& lines;
    NativeSfcReleaseSelection& selection;NativeModeRetirementAuthority& retirement;
    SurfaceOwner& input;SurfaceOwner& target;
    SurfaceOwner* secondary;SurfaceOwner* previous;
    NativeExecutionOwner& execution;uintptr_t report;
    const TglOwnedSurfaceStorage& source;bool reinitializeStmm,alignDi64;
    using Use=typename NativeAdmissionStagesFor<SurfaceOwner>::Result(*)(void*,NativeAllocationPlannerFor<SurfaceOwner>&,NativeModeReleaseContext&) noexcept;
    typename NativeAdmissionStagesFor<SurfaceOwner>::Result run(void* scope,Use use) noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        const auto child=reinterpret_cast<uintptr_t>(selection.child);
        const auto generation=selection.owner.allocationGeneration();uintptr_t vp=0;
        if(!scope || !use || !input.hasSurfaceLease(source) || !execution.hasExecutionLease(report) ||
            !selection.owner.readLeasedStorage(child,generation,child+0x80,&vp,8) ||
            !vp || !selection.graph.ownsStorage(reinterpret_cast<void*>(vp)))return result;
        const int status=NativeModeCallbackScope::run(storage,lines,selection,retirement,
            [&](NativeModeReleaseContext& mode) noexcept -> int {
                NativeAllocationPlannerFor<SurfaceOwner> planner{storage,input,&target,secondary,previous,selection.owner,
                    execution,selection.child,report,source,&readMemory,
                    NativeAllocationSfcFormat{selection.owner,selection.child,reinterpret_cast<void*>(vp),generation},
                    NativeAllocationSfcColor{selection.owner,selection.child,reinterpret_cast<void*>(vp),generation},
                    NativeAllocationParameterKindFor<SurfaceOwner>{input,source},TglOwnedFfdiCompressionFormatQuery{},
                    NativeAllocationRectangleFor<SurfaceOwner>{input,target,selection.owner,execution},
                    reinitializeStmm,alignDi64,NativeDataAdmission{},NativeAllocationUnwind{mode}};
                result=use(scope,planner,mode);
                return result.status;
            });
        if(status) {result.status=status;result.admitted=false;}
        return result;
    }
};
using NativeModeAllocationPlan=NativeModeAllocationPlanFor<NativeContextSurfaceOwner>;
template struct NativeModeAllocationPlanFor<NativeTemporarySurfaceOwner>;
// Stable, per-frame capabilities keep construction-time hook types identical
// to execution-time producer resolvers. No success/default producer exists.
struct NativeDiIecpCapture {
    using Consume=int(*)(void*,const TglDiIecpInputs&) noexcept;
    void* scope=nullptr;int(*capture)(void*,void*,Consume) noexcept=nullptr;
    template<class Use> int operator()(Use use) const noexcept {
        static_assert(noexcept(use(std::declval<const TglDiIecpInputs&>())) &&
            std::is_same_v<decltype(use(std::declval<const TglDiIecpInputs&>())),int>);
        if(!scope || !capture || !ownedExecutableMemory(reinterpret_cast<uintptr_t>(capture),1))return 5;
        struct Call {Use& use;bool entered=false,repeated=false;int result=5;};
        Call call{use};
        const int status=capture(scope,&call,[](void* context,const TglDiIecpInputs& input) noexcept -> int {
            auto& call=*static_cast<Call*>(context);
            if(call.entered) {call.repeated=true;return 5;}
            call.entered=true;call.result=call.use(input);return call.result;
        });
        if(call.repeated || !call.entered)return 5;
        return status?status:call.result;
    }
};
struct NativeFrameSurfaceAdmission {
    void* scope=nullptr;int(*admit)(void*,const TglOwnedSurfaceStorage&) noexcept=nullptr;
    int operator()(const TglOwnedSurfaceStorage& surface) const noexcept {
        return scope && admit && ownedExecutableMemory(reinterpret_cast<uintptr_t>(admit),1)?admit(scope,surface):5;
    }
};
template<class SurfaceOwner> struct NativeRetainedDiIecpCaptureFor {
    NativeAllocationPlannerFor<SurfaceOwner>& planner;NativeFrameDataRead& read;
    uintptr_t image;NativeDenoiseParameters* parameters;
    bool busy=false;
    const TglDiIecpInputs* active=nullptr;uint64_t generation=0;
    bool live() const noexcept {
        return busy && active && generation && planner.childOwner.allocationGeneration()==generation &&
            planner.childOwner.hasPrivateChildImageLease(reinterpret_cast<uintptr_t>(planner.child),image) &&
            planner.executionOwner.hasExecutionLease(planner.execution) &&
            tglValidateOriginalDiIecpLeases(*active,planner.input,planner.source,planner.target,planner.previous);
    }
    static int admitRole(void* context,const TglOwnedSurfaceStorage& surface,size_t role) noexcept {
        auto* self=static_cast<NativeRetainedDiIecpCaptureFor*>(context);
        if(!self || !self->live() || role>=5)return 5;
        const auto& input=*self->active;TglOwnedSurfaceInputs selected;
        if(!tglPrepareSurfaceInputs(selected,input.current,input.target,input.pipe,input.di,input.iecp,
            input.dnNeeded,input.indices,input.ffdi,input.ffdn,input.stmm) || selected.surfaces[role]!=&surface ||
            !tglNativeBackingPresent(surface.resource))return 5;
        return 0;
    }
    static int admitDi(void* context,const TglOwnedSurfaceStorage& surface) noexcept {
        auto* self=static_cast<NativeRetainedDiIecpCaptureFor*>(context);
        if(!self || !self->live() || !tglNativeBackingPresent(surface.resource))return 5;
        const auto& input=*self->active;
        bool known=&surface==input.current || &surface==input.target || &surface==input.statistics ||
            (input.referenceValid && &surface==input.previous);
        for(const auto* item:input.ffdi)known=known || item==&surface;
        for(const auto* item:input.ffdn)known=known || item==&surface;
        for(const auto* item:input.stmm)known=known || item==&surface;
        return known?0:5;
    }
    NativeDiIecpCapture capability() noexcept {return {this,&capture};}
    static int capture(void* context,void* sink,NativeDiIecpCapture::Consume consume) noexcept {
        auto* self=static_cast<NativeRetainedDiIecpCaptureFor*>(context);
        if(!self || !sink || !consume || self->busy || !self->read.scope || !self->read.resolve)return 5;
        auto& planner=self->planner;
        if(planner.read!=&readMemory ||
            !planner.childOwner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(planner.child)) ||
            !planner.executionOwner.hasExecutionLease(planner.execution) ||
            !planner.input.hasSurfaceLease(planner.source))return 5;
        std::array<uint32_t,11> controls{};
        if(!tglReadLeasedDiIecpCacheControls(planner.childOwner,
            reinterpret_cast<uintptr_t>(planner.child),self->image,controls))return 5;
        self->busy=true;struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{self->busy};
        // Allocation selection is read at producer use, not guessed from a
        // pre-allocation report. Every tail view remains inside this consume.
        return tglRunRetainedDiIecpFrameInput(planner.storage,planner.input,planner.target,
            planner.secondary,planner.previous,planner.childOwner,planner.executionOwner,
            planner.child,planner.execution,planner.source,self->read,planner.alignDi64,controls,
            [&](const TglDiIecpInputs& input) noexcept -> int {
                if(self->active)return 5;
                self->active=&input;self->generation=planner.childOwner.allocationGeneration();
                struct Revoke {NativeRetainedDiIecpCaptureFor& self;
                    ~Revoke(){self.active=nullptr;self.generation=0;}} revoke{*self};
                if(!self->live())return 5;
                const int result=consume(sink,input);
                return result?result:(self->live()?0:5);
            },self->parameters);
    }
};
using NativeRetainedDiIecpCapture=NativeRetainedDiIecpCaptureFor<NativeContextSurfaceOwner>;
template struct NativeRetainedDiIecpCaptureFor<NativeTemporarySurfaceOwner>;
struct NativeFrameSurfaceRoleAdmission {
    void* scope=nullptr;int(*admit)(void*,const TglOwnedSurfaceStorage&,size_t) noexcept=nullptr;
    int operator()(const TglOwnedSurfaceStorage& surface,size_t role) const noexcept {
        return scope && admit && role<5 && ownedExecutableMemory(reinterpret_cast<uintptr_t>(admit),1)?
            admit(scope,surface,role):5;
    }
};
using NativeVeboxControlPolicy=TglOwnedDiIecpControlPolicyReader<NativeFrameDataRead>;
using NativeVeboxLivePacketFrame=TglOwnedVeboxLivePacketContext<NativeDiIecpCapture,NativeFrameDataRead,
    NativeFrameSurfaceRoleAdmission,NativeFrameSurfaceAdmission,TglNativeDiIecpRegistration,
    TglNativeDiIecpControl<NativeVeboxControlPolicy>>;
template<class SurfaceOwner> struct NativeVeboxPacketBridgeFor : TglOwnedVeboxNativePacketBridge<NativeVeboxLivePacketFrame,NativeStackPacketAdmission> {
    using Base=TglOwnedVeboxNativePacketBridge<NativeVeboxLivePacketFrame,NativeStackPacketAdmission>;
    SurfaceOwner& sourceOwner;SurfaceOwner& targetOwner;
    NativeRendererChildOwner& childOwner;uintptr_t image;
    NativeVeboxPacketBridgeFor(NativeVeboxLivePacketFrame& frame,NativeStackPacketAdmission admit,
        SurfaceOwner& source,SurfaceOwner& target,
        NativeRendererChildOwner& child,uintptr_t nativeImage) noexcept
        :Base(frame,admit),sourceOwner(source),targetOwner(target),childOwner(child),image(nativeImage) {}
    int state(bool di,void* packet) noexcept {
        if(!leases.target)return 5;
        bool temporalPolicy=false;
        if(!tglReadLeasedTemporalDenoisePolicy(childOwner,leases.child,image,temporalPolicy))return 5;
        frame.disableTemporalDenoise=temporalPolicy; // refresh at actual producer use
        return transfer(leases.state,packet,[&]() noexcept -> int {
            return frame.stateWithBorrow(di,&leases.state,[&](auto produce) noexcept -> int {
                return tglWithMutableVeboxPacketRoots(sourceOwner,leases.source,targetOwner,*leases.target,
                    [&](auto&,auto&) noexcept -> int {return produce();});
            });
        });
    }
};
using NativeVeboxPacketBridge=NativeVeboxPacketBridgeFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeVeboxProducerTypesFor=TglOwnedVeboxFrameProducerTypes<NativeVeboxPacketBridgeFor<SurfaceOwner>,NativeRendererChildOwner,
    NativeExecutionOwner,decltype(&readMemory),TglNativeDndiStackAdmission,NativeVeboxCscProducerFor<SurfaceOwner>>;
using NativeVeboxProducerTypes=NativeVeboxProducerTypesFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> using NativeVeboxProducerResolveFor=typename NativeVeboxProducerTypesFor<SurfaceOwner>::Resolve;
using NativeVeboxProducerResolve=NativeVeboxProducerResolveFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeVeboxFrameCapabilityScopeFor {
    using Use=typename NativeAdmissionStagesFor<SurfaceOwner>::Result(*)(void*,NativeVeboxLivePacketFrame&) noexcept;
    static typename NativeAdmissionStagesFor<SurfaceOwner>::Result run(NativeRendererCpuAssembly& assembly,
        const TglNativeVphalMhwView& view,NativeRetainedDiIecpCaptureFor<SurfaceOwner>& capture,
        TglOwnedVeboxPacketLeases leases,void* scope,Use use) noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        auto& planner=capture.planner;
        if(!scope || !use || capture.busy || capture.active || !leases.target ||
            leases.child!=reinterpret_cast<uintptr_t>(planner.child) || leases.execution!=planner.execution ||
            &leases.source!=&planner.source || leases.renderHal!=view.renderHal ||
            !planner.target || !planner.target->hasSurfaceLease(*leases.target) ||
            !planner.input.hasSurfaceLease(leases.source))return result;
        bool disableTemporalDenoise=false;
        if(!tglReadLeasedTemporalDenoisePolicy(planner.childOwner,leases.child,view.renderer.image,
            disableTemporalDenoise))return result;
        const int status=assembly.cpu().withBorrowedCreatorDiIecpServices(view,&readMemory,
            NativeTglImageAdmission{view.renderer.image},&executableMemory,
            [&](const TglNativeDiIecpServices& services) noexcept -> int {
                NativeVeboxLivePacketFrame frame{{},leases,disableTemporalDenoise,capture.capability(),capture.read,
                    {&capture,&NativeRetainedDiIecpCaptureFor<SurfaceOwner>::admitRole},
                    {&capture,&NativeRetainedDiIecpCaptureFor<SurfaceOwner>::admitDi},
                    TglNativeDiIecpRegistration{services},
                    TglNativeDiIecpControl<NativeVeboxControlPolicy>{services,
                        {leases.child,leases.execution,leases.target,capture.read}}};
                result=use(scope,frame);return result.status;
            });
        if(status) {result.status=status;result.admitted=false;}
        return result;
    }
};
using NativeVeboxFrameCapabilityScope=NativeVeboxFrameCapabilityScopeFor<NativeContextSurfaceOwner>;
template<class SurfaceOwner> struct NativeVeboxPacketProducerScopeFor {
    struct DnRead {
        SurfaceOwner& owner;NativeDenoiseParameters& parameters;
        const TglOwnedSurfaceStorage& source;
        static bool resolve(void* context,const TglOwnedSurfaceStorage& actual,uintptr_t& output) noexcept {
            auto* self=static_cast<DnRead*>(context);
            if(!self || &actual!=&self->source || !self->owner.hasSurfaceLease(actual))return false;
            uintptr_t pointer=0;std::memcpy(&pointer,actual.prefix.data()+0x90,8);
            if(pointer && (!tglHasNativeDenoiseAdmissionGrant(self->owner,actual,self->parameters) ||
                pointer!=self->parameters.get() || self->parameters.size()<8))return false;
            output=pointer;return true;
        }
    };
    // Construction and execution name this SAME resolver specialization.
    // This is only the producer portion, never a complete publishable table.
    static std::array<uintptr_t,20> producerHooks(std::array<uintptr_t,20> hooks) noexcept {
        hooks=tglBindOwnedVeboxKernelSetupHook<NativeVeboxProducerResolveFor<SurfaceOwner>>(hooks);
        hooks=tglBindOwnedVeboxProducerHooks<NativeVeboxProducerResolveFor<SurfaceOwner>>(hooks);
        hooks=tglBindOwnedVeboxCscHook<NativeVeboxProducerResolveFor<SurfaceOwner>>(hooks);
        return tglBindOwnedVeboxDndiHook<NativeVeboxProducerResolveFor<SurfaceOwner>>(hooks);
    }
    static typename NativeAdmissionStagesFor<SurfaceOwner>::Result run(NativeVeboxLivePacketFrame& frame,NativeDenoiseParameters& parameters,
        NativeVeboxCscProducerFor<SurfaceOwner> csc,NativeAllocatedAdmissionRenderFor<SurfaceOwner>& render) noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        auto& admission=render.render.admission;auto& child=admission.reset.childOwner;
        auto& execution=admission.reset.executionOwner;
        const auto& leases=frame.leases;
        if(leases.child!=reinterpret_cast<uintptr_t>(admission.reset.child) ||
            leases.execution!=admission.reset.execution || &leases.source!=&admission.feature.source ||
            leases.target!=&admission.feature.target || &csc.health.childOwner!=&child ||
            &csc.health.executionOwner!=&execution || &csc.health.source!=&leases.source ||
            &csc.health.target!=leases.target || frame.read.scope!=admission.read.scope ||
            frame.read.resolve!=admission.read.resolve || &parameters!=&admission.feature.parameters)return result;
        DnRead dn{admission.feature.sourceOwner,parameters,leases.source};
        NativeVeboxPacketBridgeFor<SurfaceOwner> bridge(frame,NativeStackPacketAdmission{},
            admission.feature.sourceOwner,admission.feature.targetOwner,child,render.render.binding.image);
        const int status=tglWithOwnedVeboxFrameProducers(bridge,child,execution,0,&readMemory,
            TglNativeDndiStackAdmission{},csc,[&](void* object,auto& producer) noexcept -> int {
                static_assert(std::is_same_v<std::remove_reference_t<decltype(producer)>,typename NativeVeboxProducerTypesFor<SurfaceOwner>::Context>);
                if(object!=admission.reset.child || NativeVeboxProducerResolveFor<SurfaceOwner>::get(object)!=&producer)return 5;
                producer.frame.frame.parameterScope=&dn;
                producer.frame.frame.parameterRead=&DnRead::resolve;
                std::array<uintptr_t,20> hooks{};
                using Lifetime=TglOwnedVeboxScopedLifetimeResolve<NativeRendererChildLifetime>;
                if(!tglBuildOwnedVeboxCompleteHooks<Lifetime,NativeAdmissionResolveFor<SurfaceOwner>,NativeAllocationResolveFor<SurfaceOwner>,
                    NativeVeboxProducerResolveFor<SurfaceOwner>,NativeModeReleaseScope,TglOwnedFormatReader>(object,hooks) ||
                    !child.matchesPrivateVtableHooks(hooks))return 5;
                return producer.pass.withBuilder([&]() noexcept -> int {result=render.run();return result.status;});
            });
        if(status) {result.status=status;result.admitted=false;}
        return result;
    }
};
using NativeVeboxPacketProducerScope=NativeVeboxPacketProducerScopeFor<NativeContextSurfaceOwner>;
template struct NativeVeboxFrameCapabilityScopeFor<NativeTemporarySurfaceOwner>;
template struct NativeVeboxPacketProducerScopeFor<NativeTemporarySurfaceOwner>;
template<class SurfaceOwner> struct NativeCompleteFrameConsumerFor {
    NativeRendererCpuAssembly& assembly;const TglNativeVphalMhwView& view;
    NativeRetainedDiIecpCaptureFor<SurfaceOwner>& capture;TglOwnedVeboxPacketLeases leases;
    NativeDenoiseParameters& parameters;NativeVeboxCscProducerFor<SurfaceOwner> csc;
    NativeAllocatedAdmissionRenderFor<SurfaceOwner>& render;
    bool attempted=false;
    typename NativeAdmissionStagesFor<SurfaceOwner>::Result run() noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        auto& admission=render.render.admission;
        if(attempted || &capture.planner!=&render.planner || &capture.read!=&admission.read ||
            capture.parameters!=&parameters || &parameters!=&admission.feature.parameters ||
            &admission.reset.backing!=&assembly.backing() || view.renderer.image!=render.render.binding.image ||
            assembly.backing().retention()())return result;
        attempted=true;
        return NativeVeboxFrameCapabilityScopeFor<SurfaceOwner>::run(assembly,view,capture,leases,this,
            [](void* context,NativeVeboxLivePacketFrame& frame) noexcept -> typename NativeAdmissionStagesFor<SurfaceOwner>::Result {
                auto& self=*static_cast<NativeCompleteFrameConsumerFor*>(context);
                return NativeVeboxPacketProducerScopeFor<SurfaceOwner>::run(frame,self.parameters,self.csc,self.render);
            });
    }
    static TglOwnedVeboxCommandTransactionResult consume(void* context,NativeRendererCpuAssembly& cpu,
        const TglNativeCreatedVeboxRendererView& pinned,
        SurfaceOwner& source,SurfaceOwner& target,
        SurfaceOwner*,SurfaceOwner*,NativeDenoiseParameters& dn,
        NativeRendererChildLifetime& first,NativeRendererChildLifetime& second) noexcept {
        TglOwnedVeboxCommandTransactionResult result;
        if(!context)return result;
        auto& self=*static_cast<NativeCompleteFrameConsumerFor*>(context);
        auto& admission=self.render.render.admission;
        using Life=TglOwnedVeboxScopedContext<NativeRendererChildLifetime>;
        auto* current=Life::get(admission.reset.child);
        if(&self.assembly!=&cpu || &self.parameters!=&dn ||
            &admission.feature.sourceOwner!=&source || &admission.feature.targetOwner!=&target ||
            self.view.renderer.image!=pinned.image || self.view.parent!=pinned.creator ||
            self.view.renderer.renderer!=pinned.renderer ||
            !self.view.matchesCreated(pinned,&readMemory,NativeDataAdmission{},NativeTglImageAdmission{pinned.image}) ||
            current!=(admission.reset.index==0?&first:admission.reset.index==1?&second:nullptr) ||
            !current)return result;
        const auto completed=self.run();
        result=completed.native; // retain acquisition/native-entry evidence on failure
        result.status=completed.status;
        return result;
    }
};
using NativeCompleteFrameConsumer=NativeCompleteFrameConsumerFor<NativeContextSurfaceOwner>;
template struct NativeCompleteFrameConsumerFor<NativeTemporarySurfaceOwner>;
// Construct the real per-frame execution objects inside the caller's original
// surface/data/mode leases. Every reference below survives through native
// Render; nothing is published as a dangling callback or copied to a fixture.
template<class SurfaceOwner> struct NativeFrameExecutionAssemblyFor {
    struct Inputs {
        NativeRendererCpuAssembly& assembly;const TglNativeVphalMhwView& view;
        NativeVpProducerFrameFor<SurfaceOwner>& vp;NativeAdmissionStagesFor<SurfaceOwner>& admission;
        NativeAllocationPlannerFor<SurfaceOwner>& planner;NativeModeReleaseContext& mode;
        const TglVeboxRenderBinding& binding;void* argument;
        TglOwnedVeboxPacketLeases leases;
        NativeDenoiseParameters& parameters;NativeVeboxCscProducerFor<SurfaceOwner> csc;
    };
    static typename NativeAdmissionStagesFor<SurfaceOwner>::Result run(Inputs& in) noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        if(!in.admission.read.scope || !in.admission.read.resolve ||
            &in.planner.childOwner!=&in.admission.reset.childOwner ||
            &in.parameters!=&in.admission.feature.parameters)return result;
        const int status=in.vp.run([&](NativeVpPreparedCallbacksFor<SurfaceOwner>& callbacks) noexcept -> int {
            NativePreparedAdmissionRenderFor<SurfaceOwner> prepared{callbacks,in.admission,in.binding,in.argument};
            NativeAllocatedAdmissionRenderFor<SurfaceOwner> allocated{prepared,in.planner,in.mode};
            NativeRetainedDiIecpCaptureFor<SurfaceOwner> capture{in.planner,in.admission.read,in.view.renderer.image,&in.parameters};
            NativeCompleteFrameConsumerFor<SurfaceOwner> consumer{in.assembly,in.view,capture,in.leases,in.parameters,
                in.csc,allocated};
            result=consumer.run();
            return result.status;
        });
        if(status){result.status=status;result.admitted=false;}
        return result;
    }
};
using NativeFrameExecutionAssembly=NativeFrameExecutionAssemblyFor<NativeContextSurfaceOwner>;
template struct NativeFrameExecutionAssemblyFor<NativeTemporarySurfaceOwner>;
template<class SurfaceOwner> struct NativeModeFrameExecutionFor {
    struct Inputs {
        NativeRendererCpuAssembly& assembly;const TglNativeVphalMhwView& view;
        NativeVpProducerFrameFor<SurfaceOwner>& vp;NativeAdmissionFrameAssemblyFor<SurfaceOwner>& admission;
        NativeModeAllocationPlanFor<SurfaceOwner>& allocation;
        const TglVeboxRenderBinding& binding;void* argument;
        TglOwnedVeboxPacketLeases leases;
        NativeDenoiseParameters& parameters;NativeVeboxCscProducerFor<SurfaceOwner> csc;
    };
    static typename NativeAdmissionStagesFor<SurfaceOwner>::Result run(Inputs& in) noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        const int status=in.admission.run([&](NativeAdmissionStagesFor<SurfaceOwner>& stages) noexcept -> int {
            auto& plan=in.allocation;
            if(&plan.retirement!=&stages.reset.retirement || plan.selection.child!=stages.reset.child ||
                &plan.selection.owner!=&stages.reset.childOwner || plan.report!=stages.reset.execution ||
                &plan.execution!=&stages.reset.executionOwner || &plan.input!=&stages.feature.sourceOwner ||
                &plan.target!=&stages.feature.targetOwner || &plan.source!=&stages.feature.source)return 5;
            struct Call {Inputs& in;NativeAdmissionStagesFor<SurfaceOwner>& stages;};
            Call call{in,stages};
            result=plan.run(&call,[](void* context,NativeAllocationPlannerFor<SurfaceOwner>& planner,
                NativeModeReleaseContext& mode) noexcept -> typename NativeAdmissionStagesFor<SurfaceOwner>::Result {
                auto& call=*static_cast<Call*>(context);auto& in=call.in;
                typename NativeFrameExecutionAssemblyFor<SurfaceOwner>::Inputs execution{in.assembly,in.view,in.vp,call.stages,
                    planner,mode,in.binding,in.argument,in.leases,in.parameters,in.csc};
                return NativeFrameExecutionAssemblyFor<SurfaceOwner>::run(execution);
            });
            return result.status;
        });
        if(status){result.status=status;result.admitted=false;}
        return result;
    }
};
using NativeModeFrameExecution=NativeModeFrameExecutionFor<NativeContextSurfaceOwner>;
template struct NativeModeFrameExecutionFor<NativeTemporarySurfaceOwner>;
template<class SurfaceOwner> struct NativePolicyFrameExecutionFor {
    struct Inputs {
        NativeRendererCpuAssembly& assembly;const TglNativeVphalMhwView& view;
        NativeVpProducerFrameFor<SurfaceOwner>& vp;NativeOutputPipeFrameScopeFor<SurfaceOwner>& policy;NativeFrameDataRead& read;
        const typename NativeAdmissionFrameAssemblyFor<SurfaceOwner>::Inputs& admission;
        NativeModeAllocationPlanFor<SurfaceOwner>& allocation;
        const TglVeboxRenderBinding& binding;void* argument;
        TglOwnedVeboxPacketLeases leases;
        NativeDenoiseParameters& parameters;NativeVeboxCscProducerFor<SurfaceOwner> csc;
    };
    static typename NativeAdmissionStagesFor<SurfaceOwner>::Result run(Inputs& in) noexcept {
        typename NativeAdmissionStagesFor<SurfaceOwner>::Result result;
        const auto& a=in.admission;auto& p=in.policy;
        if(&p.read!=&in.read || &p.health.childOwner!=&a.childOwner ||
            &p.health.executionOwner!=&a.executionOwner || &p.health.sourceOwner!=&a.sourceOwner ||
            &p.health.targetOwner!=&a.targetOwner || p.health.child!=reinterpret_cast<uintptr_t>(a.child) ||
            p.health.execution!=a.execution || &p.health.source!=&a.source || &p.health.target!=&a.target ||
            !a.params.data() || !a.pass.data() || p.parameters!=reinterpret_cast<uintptr_t>(a.params.data()) ||
            &p.compositionNeeded!=a.pass.data()+8 || p.vp.bytes.data()!=a.vp || &in.parameters!=&a.parameters)return result;
        const int status=p.run([&](NativeOutputPipeSelectorFor<SurfaceOwner>& selector) noexcept -> int {
            NativeAdmissionFrameAssemblyFor<SurfaceOwner> admission(a,selector,in.read);
            typename NativeModeFrameExecutionFor<SurfaceOwner>::Inputs mode{in.assembly,in.view,in.vp,admission,in.allocation,
                in.binding,in.argument,in.leases,in.parameters,in.csc};
            result=NativeModeFrameExecutionFor<SurfaceOwner>::run(mode);
            return result.status;
        });
        if(status){result.status=status;result.admitted=false;}
        return result;
    }
};
using NativePolicyFrameExecution=NativePolicyFrameExecutionFor<NativeContextSurfaceOwner>;
template struct NativePolicyFrameExecutionFor<NativeTemporarySurfaceOwner>;
// Select the permanent callback domain at child construction, not by swapping
// tables during a frame. Execution must use that SAME SurfaceOwner domain.
template<class SurfaceOwner> struct NativeRendererCompleteHooksFor {
    using LifetimeResolve=TglOwnedVeboxScopedLifetimeResolve<NativeRendererChildLifetime>;
    static int install(NativeRendererCpuAssembly& assembly,const TglNativeVphalMhwView& view) noexcept {
        return assembly.withChildLifetimeScopes([&](void*,void*,auto&,auto&) noexcept -> int {
            return tglInstallOwnedRendererCompleteTablesFromCreator<LifetimeResolve,NativeAdmissionResolveFor<SurfaceOwner>,
                NativeAllocationResolveFor<SurfaceOwner>,NativeVeboxProducerResolveFor<SurfaceOwner>,NativeModeReleaseScope,TglOwnedFormatReader>(
                    assembly.cpu(),view,&readMemory,NativeTglImageAdmission{view.renderer.image},&executableMemory);
        });
    }
};
using NativeRendererCompleteHooks=NativeRendererCompleteHooksFor<NativeContextSurfaceOwner>;
using NativeTemporaryRendererCompleteHooks=NativeRendererCompleteHooksFor<NativeTemporarySurfaceOwner>;
template struct NativeRendererCompleteHooksFor<NativeTemporarySurfaceOwner>;
// Frame surfaces retain their original codec contexts; CPU children remain
// in the permanent VPHAL owner and are entered only through its pinned access.
using NativeBorrowedFrontend=TglNativeVeboxBorrowedFrontendWithCpuAllocator<NativeVphalFramePin,
    NativeContextSurfaceOwner,NativeContextSurfaceOwner,TglDarwinTypedCfReferenceApi,
    NativeFrontendEnvironment,NativeFrontendAccess>;
using NativeContextRendererFrame=TglNativeContextRendererFrameStorage<NativeCodecBackend::Registry,
    NativeBorrowedFrontend>;
using NativePhysicalRegistry=TglOwnedVeboxPhysicalFrameRegistry<NativeContextRendererFrame>;
using NativePhysicalFrameScope=typename NativePhysicalRegistry::Scope;
using NativePhysicalCallbacks=TglOwnedPhysicalCallbackCoordinator<NativePhysicalRegistry,
    decltype(&readMemory),NativeTglImageAdmission,decltype(&executableMemory)>;
struct NativeFrontendRuntimeScope {
    template<class Use> static TglOwnedVeboxCommandTransactionResult run(NativeBorrowedFrontend& frontend,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,Use use) noexcept {
        if constexpr(std::is_pointer_v<Use>)if(!use)return {};
        return frontend.withCpuFrontend([&](NativeRendererCpuAssembly& cpu,const TglNativeCreatedVeboxRendererView& view,
            auto& source,auto& target,auto& parameters) noexcept {
            if(!cpu.readyForFrontend())return TglOwnedVeboxCommandTransactionResult{};
            return tglWithOriginalFrontendOwners(roles,sourceIndex,targetIndex,source,target,temporal,
                cpu.backing().retention(),[&](auto& input,auto& output,auto* secondary,auto* previous) noexcept {
                    TglOwnedVeboxCommandTransactionResult result;
                    const int status=cpu.withChildLifetimeScopes([&](void*,void*,auto& firstLife,auto& secondLife) noexcept -> int {
                        if constexpr(std::is_nothrow_invocable_r_v<TglOwnedVeboxCommandTransactionResult,Use&,
                            NativeRendererCpuAssembly&,const TglNativeCreatedVeboxRendererView&,decltype(source),
                            decltype(input),decltype(output),decltype(secondary),decltype(previous),
                            decltype(parameters),decltype(firstLife),decltype(secondLife)>)
                            result=use(cpu,view,source,input,output,secondary,previous,parameters,firstLife,secondLife);
                        else if constexpr(std::is_nothrow_invocable_r_v<TglOwnedVeboxCommandTransactionResult,Use&,
                            NativeRendererCpuAssembly&,const TglNativeCreatedVeboxRendererView&,decltype(input),decltype(output),
                            decltype(secondary),decltype(previous),decltype(parameters),decltype(firstLife),decltype(secondLife)>)
                            result=use(cpu,view,input,output,secondary,previous,parameters,firstLife,secondLife);
                        else {
                            static_assert(noexcept(use(cpu,input,output,secondary,previous,parameters,firstLife,secondLife)) &&
                                std::is_same_v<decltype(use(cpu,input,output,secondary,previous,parameters,firstLife,secondLife)),
                                    TglOwnedVeboxCommandTransactionResult>);
                            result=use(cpu,input,output,secondary,previous,parameters,firstLife,secondLife);
                        }
                        cpu.backing().retention().observe(result);
                        return result.status;
                    });
                    if(!result.status && status)result.status=status;
                    return result;
                });
        });
    }
};
struct NativeRetainedFrontendRuntime {
    using Use=TglOwnedVeboxCommandTransactionResult(*)(void*,NativeRendererCpuAssembly&,
        const TglNativeCreatedVeboxRendererView&,
        NativeContextSurfaceOwner&,NativeContextSurfaceOwner&,NativeContextSurfaceOwner*,NativeContextSurfaceOwner*,
        NativeDenoiseParameters&,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept;
    using UseGraph=TglOwnedVeboxCommandTransactionResult(*)(void*,NativeRendererCpuAssembly&,
        const TglNativeCreatedVeboxRendererView&,NativeSourceGraph&,
        NativeContextSurfaceOwner&,NativeContextSurfaceOwner&,NativeContextSurfaceOwner*,NativeContextSurfaceOwner*,
        NativeDenoiseParameters&,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept;
    static TglOwnedVeboxCommandTransactionResult runWithGraph(NativeContextRendererFrame& frame,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,void* scope,UseGraph use) noexcept {
        return run(frame,roles,sourceIndex,targetIndex,temporal,scope,use);
    }
    struct VpFrontend {
        NativeRendererCpuAssembly& assembly;const TglNativeCreatedVeboxRendererView& pinned;
        size_t childIndex;const TglVeboxAdmissionBorrowedInterfaces& interfaces;
        NativeSourceGraph& sourceGraph;NativeContextSurfaceOwner* secondary;
        NativeContextSurfaceOwner* previous;NativeDenoiseParameters& denoise;
        NativeModeRetirementAuthority& retirement;
    };
    struct VpRequest {
        using Use=TglOwnedVeboxCommandTransactionResult(*)(void*,NativeCoreFrameDataScope&,
            NativeVpGraph&,TglNativeVpLineResourceStorage&,NativeAdmissionParams&,NativeAdmissionPass&,NativeFrameDataRead&,
            NativeVpProducerFrame&,const TglOwnedVpModeState&,VpFrontend&) noexcept;
        size_t childIndex;const TglVeboxAdmissionBorrowedInterfaces& interfaces;
        uintptr_t parameters,pass;const TglOwnedVpEntrySet& catalogue;
        NativeModeRetirementAuthority& retirement;
        void* context;Use use;
    };
    using AdmissionInputUse=int(*)(void*,const NativeAdmissionFrameAssembly::Inputs&) noexcept;
    using PacketInputUse=int(*)(void*,TglOwnedVeboxPacketLeases&) noexcept;
    static int withPacketInputs(VpFrontend& frontend,NativeCoreFrameDataScope& core,
        void* context,PacketInputUse use) noexcept {
        if(!context || !use || !ownedExecutableMemory(reinterpret_cast<uintptr_t>(use),1) ||
            frontend.childIndex>1 || !core.live() || frontend.pinned.image!=core.image ||
            frontend.pinned.originalChildren[frontend.childIndex]!=core.child ||
            frontend.assembly.backing().retention()())return 5;
        const int status=tglWithOriginalVeboxPacketLeases(core.sourceOwner,core.source,core.targetOwner,core.target,
            frontend.assembly.backing().command(),core.child,core.execution,
            frontend.interfaces.renderHal,frontend.interfaces.sku,
            [&](TglOwnedVeboxPacketLeases& leases) noexcept -> int {return use(context,leases);});
        return status?status:core.live()?0:5;
    }
    using AllocationInputUse=int(*)(void*,NativeModeAllocationPlan&) noexcept;
    static int withAllocationInputs(VpFrontend& frontend,NativeCoreFrameDataScope& core,NativeVpGraph& graph,
        TglNativeVpLineResourceStorage& lines,bool alignDi64,
        void* context,AllocationInputUse use) noexcept {
        if(!context || !use || !ownedExecutableMemory(reinterpret_cast<uintptr_t>(use),1) ||
            frontend.childIndex>1 || !core.live() || frontend.pinned.image!=core.image ||
            frontend.pinned.originalChildren[frontend.childIndex]!=core.child ||
            !graph.ownsStorage(core.vp.bytes.data()) || !lines.lines().boundToVp(core.vp) ||
            frontend.assembly.backing().retention()())return 5;
        NativeSfcReleaseSelection selection{core.childOwner,reinterpret_cast<void*>(core.child),
            frontend.interfaces.renderHal,frontend.interfaces.sku,graph};
        NativeModeAllocationPlan allocation{frontend.assembly.backing().command().backing(),lines.resources(),
            selection,frontend.retirement,core.sourceOwner,core.targetOwner,frontend.secondary,frontend.previous,
            // Exact ICL1f7990 initializes each STMM only when allocator's
            // changed flag is set; owner.ensure already implements that lane.
            core.executionOwner,core.execution,core.source,false,alignDi64};
        const int status=use(context,allocation);
        return status?status:(core.live() && graph.ownsStorage(core.vp.bytes.data())?0:5);
    }
    using PolicyInputUse=int(*)(void*,NativeOutputPipeFrameScope&) noexcept;
    static int withPolicyInputs(NativeCoreFrameDataScope& core,NativeAdmissionParams& params,
        NativeAdmissionPass& pass,NativeFrameDataRead& read,const TglVeboxAdmissionBorrowedInterfaces& interfaces,
        const TglOwnedGen12SurfaceFeatures& sourceFeatures,const TglOwnedGen12SurfaceFeatures& targetFeatures,
        TglVeboxPipeAcquisition& acquisition,int& decompressionStatus,
        NativeTargetDecompression& decompression,void* context,PolicyInputUse use) noexcept {
        if(!context || !use || !ownedExecutableMemory(reinterpret_cast<uintptr_t>(use),1) ||
            !core.live() || !params.data() || !pass.data() || !acquisition.healthy() ||
            sourceFeatures.surface!=&core.source || targetFeatures.surface!=&core.target ||
            sourceFeatures.frame!=&acquisition || targetFeatures.frame!=&acquisition)return 5;
        uintptr_t source=0,target=0,di=0,fill=0;
        const auto parameters=reinterpret_cast<uintptr_t>(params.data());
        if(!read(parameters+8,&source,8) || source!=reinterpret_cast<uintptr_t>(&core.source) ||
            !read(parameters+0x98,&target,8) || target!=reinterpret_cast<uintptr_t>(&core.target) ||
            !read(source+0x88,&di,8) || !read(parameters+0xe0,&fill,8) ||
            fill!=interfaces.paramsE0 || !core.live())return 5;
        NativeTwoPassCscHealth health{core.childOwner,core.executionOwner,core.sourceOwner,core.targetOwner,
            core.source,core.target,core.image,core.child,core.execution,core.generation};
        NativeOutputPipeFrameScope policy{health,read,core.vp,parameters,di,fill,sourceFeatures,targetFeatures,
            pass.data()[8],acquisition,decompressionStatus,decompression};
        const int status=use(context,policy);
        return status?status:(core.live() && acquisition.healthy()?0:5);
    }
    static int withAdmissionInputs(VpFrontend& frontend,NativeCoreFrameDataScope& core,NativeVpGraph& graph,
        NativeAdmissionParams& params,NativeAdmissionPass& pass,NativeFrameDataRead& read,
        const TglOwnedVpEntrySet& catalogue,const TglOwnedVpModeState& mode,
        void* context,AdmissionInputUse use) noexcept {
        if(!context || !use || !ownedExecutableMemory(reinterpret_cast<uintptr_t>(use),1) ||
            frontend.childIndex>1 || !core.live() || core.image!=frontend.pinned.image ||
            frontend.pinned.originalChildren[frontend.childIndex]!=core.child ||
            !graph.ownsStorage(core.vp.bytes.data()) || frontend.assembly.backing().retention()())return 5;
        TglVeboxFeatureBinding feature;
        uintptr_t mhw=0,linkedVp=0;
        if(!feature.resolve(core.image,&readMemory,NativeTglImageAdmission{core.image},&executableMemory) ||
            !read(core.child+0x80,&linkedVp,8) || linkedVp!=reinterpret_cast<uintptr_t>(core.vp.bytes.data()) ||
            !read(linkedVp+0x20,&mhw,8) || !mhw || !core.live())return 5;
        // Native12c010 obtains +48's report and passes that SAME report as
        // VP40's renderData argument, not a caller-provided substitute.
        NativeAdmissionFrameAssembly::Inputs inputs{core.childOwner,core.executionOwner,
            frontend.assembly.backing(),frontend.retirement,core.sourceOwner,core.targetOwner,
            frontend.sourceGraph,frontend.denoise,core.source,core.target,feature,params,pass,
            frontend.interfaces,catalogue,mode,frontend.childIndex,reinterpret_cast<void*>(core.child),
            core.execution,core.vp.bytes.data(),mhw,reinterpret_cast<void*>(core.execution),&graph,&core.vp,core.preparedReset};
        const int status=use(context,inputs);
        return status?status:core.live()?0:5;
    }
    static TglOwnedVeboxCommandTransactionResult runVpInputs(NativeContextRendererFrame& frame,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,VpRequest& request) noexcept {
        if(request.childIndex>1 || !request.context || !request.use ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(request.use),1))return {};
        return run(frame,roles,sourceIndex,targetIndex,temporal,&request,
            +[](void* context,NativeRendererCpuAssembly& cpu,const TglNativeCreatedVeboxRendererView& pinned,
                NativeSourceGraph& sourceGraph,
                NativeContextSurfaceOwner& source,NativeContextSurfaceOwner& target,
                NativeContextSurfaceOwner* secondary,NativeContextSurfaceOwner* previous,NativeDenoiseParameters& denoise,
                NativeRendererChildLifetime& first,NativeRendererChildLifetime& second) noexcept {
                auto& request=*static_cast<VpRequest*>(context);
                const auto child=pinned.originalChildren[request.childIndex];
                auto* lifetime=TglOwnedVeboxScopedContext<NativeRendererChildLifetime>::get(
                    reinterpret_cast<void*>(child));
                if(!lifetime || lifetime!=(request.childIndex?&second:&first))
                    return TglOwnedVeboxCommandTransactionResult{};
                NativeLeasedFrameInputScope::Inputs inputs{cpu,request.childIndex,source,target,pinned,
                    request.interfaces,request.parameters,request.pass,&request.retirement};
                VpFrontend frontend{cpu,pinned,request.childIndex,request.interfaces,sourceGraph,
                    secondary,previous,denoise,request.retirement};
                struct Continuation {VpRequest& request;VpFrontend& frontend;} continuation{request,frontend};
                return NativeLeasedVpFrameInputScope::run(inputs,request.catalogue,&continuation,
                    +[](void* context,NativeCoreFrameDataScope& core,NativeVpGraph& graph,TglNativeVpLineResourceStorage& lines,
                        NativeAdmissionParams& params,NativeAdmissionPass& pass,NativeFrameDataRead& read,
                        NativeVpProducerFrame& vp,const TglOwnedVpModeState& mode) noexcept {
                        auto& next=*static_cast<Continuation*>(context);
                        return next.request.use(next.request.context,core,graph,lines,params,pass,read,vp,mode,next.frontend);
                    });
            });
    }
    static TglOwnedVeboxCommandTransactionResult runPrepared(NativeContextRendererFrame& frame,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,NativeModeFrameExecution::Inputs& inputs) noexcept {
        return run(frame,roles,sourceIndex,targetIndex,temporal,&inputs,
            +[](void* context,NativeRendererCpuAssembly& cpu,const TglNativeCreatedVeboxRendererView& pinned,
                NativeContextSurfaceOwner& source,NativeContextSurfaceOwner& target,
                NativeContextSurfaceOwner* secondary,NativeContextSurfaceOwner* previous,
                NativeDenoiseParameters& parameters,NativeRendererChildLifetime& first,
                NativeRendererChildLifetime& second) noexcept -> TglOwnedVeboxCommandTransactionResult {
                TglOwnedVeboxCommandTransactionResult result;
                auto& in=*static_cast<NativeModeFrameExecution::Inputs*>(context);
                auto& plan=in.allocation;
                auto* life=TglOwnedVeboxScopedContext<NativeRendererChildLifetime>::get(plan.selection.child);
                if(&in.assembly!=&cpu || &in.parameters!=&parameters ||
                    &plan.input!=&source || &plan.target!=&target ||
                    plan.secondary!=secondary || plan.previous!=previous ||
                    !life || (life!=&first && life!=&second) ||
                    !in.view.matchesCreated(pinned,&readMemory,NativeDataAdmission{},
                        NativeTglImageAdmission{pinned.image}))return result;
                const auto execution=NativeModeFrameExecution::run(in);
                result=execution.native;result.status=execution.status;
                return result;
            });
    }
    static TglOwnedVeboxCommandTransactionResult runComplete(NativeContextRendererFrame& frame,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,NativeCompleteFrameConsumer& consumer) noexcept {
        return run(frame,roles,sourceIndex,targetIndex,temporal,&consumer,&NativeCompleteFrameConsumer::consume);
    }
    template<class Consumer>
    static TglOwnedVeboxCommandTransactionResult run(NativeContextRendererFrame& frame,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,void* scope,Consumer use) noexcept {
        static_assert(std::is_pointer_v<Consumer>,"owned executable callback must be a function pointer");
        TglOwnedVeboxCommandTransactionResult result;
        if(!scope || !use || !ownedExecutableMemory(reinterpret_cast<uintptr_t>(use),1) ||
            frame.invalidated() || !frame.surfacesReady())return result;
        const int status=frame.withRenderPass([&](NativeBorrowedFrontend& frontend) noexcept -> int {
            result=NativeFrontendRuntimeScope::run(frontend,roles,sourceIndex,targetIndex,temporal,
                [&](NativeRendererCpuAssembly& cpu,const TglNativeCreatedVeboxRendererView& view,NativeSourceGraph& graph,
                    NativeContextSurfaceOwner& source,NativeContextSurfaceOwner& target,
                    NativeContextSurfaceOwner* secondary,NativeContextSurfaceOwner* previous,
                    NativeDenoiseParameters& parameters,NativeRendererChildLifetime& first,
                    NativeRendererChildLifetime& second) noexcept -> TglOwnedVeboxCommandTransactionResult {
                    if constexpr(std::is_nothrow_invocable_r_v<TglOwnedVeboxCommandTransactionResult,Consumer,
                        void*,NativeRendererCpuAssembly&,const TglNativeCreatedVeboxRendererView&,NativeSourceGraph&,
                        NativeContextSurfaceOwner&,NativeContextSurfaceOwner&,NativeContextSurfaceOwner*,NativeContextSurfaceOwner*,
                        NativeDenoiseParameters&,NativeRendererChildLifetime&,NativeRendererChildLifetime&>)
                        return use(scope,cpu,view,graph,source,target,secondary,previous,parameters,first,second);
                    else return use(scope,cpu,view,source,target,secondary,previous,parameters,first,second);
                });
            return result.status;
        });
        // Preserve acquisition/native-entry evidence even on post-frame loss;
        // the same root aggregate remains the quarantine/retirement owner.
        if(status)result.status=status;
        return result;
    }
};
// Join the concrete original-frame producers in one synchronous execution.
// Missing metadata/retirement producers are not supplied by this assembler.
// This is the LOWER admission+Render chain, not a renderer virtual+50 hook.
// Exact TGL 1b1ed0 first uses 1b20d0 to copy 17 source / 8 target entries
// (0x308 allocation, 0x2a8 copied bytes each), then resolves pass roots from
// those temporary entries. 1b2530 copies them BACK even after native failure;
// 1b2940 copies only pass flags/indices at +8/+c/+20/+30, not root pointers.
// An external dispatcher must preserve that conversion/copyback and the
// 131d10 source+134==2 gate, and latch errors ignored by caller 131520.
// Do not substitute original root aliases or execute admission/Render twice.
// Temporary lifetime is separate: 1b2870 frees all 17/8 copied shells;
// 1b2400 prepares two additional 0x308 shells at pass+48/+50, destroyed
// through 1b2b20->1337b0. Successful pass+20 handoff switches root+10 to
// root+18 and 131e20 advances pass+40 modulo two. None of these are leases
// on the original roots or evidence that GPU resource retirement completed.
// Integration dependency: NativeContextSurfaceOwner::hasSurfaceLease pins
// ONLY its original shell, and NativeCoreFrameDataScope / packet.same()
// require that exact identity. A copied shell from TemporaryScope cannot
// pass those checks merely because its resource descriptor is identical.
// Do not weaken those checks or resolve the temporary address in the native
// context registry. The dispatcher needs a derived temporary-shell lease
// under the original context/root pin, including nested metadata lifetime
// and copyback reconciliation, before calling this existing execution path.
// Keep NativeBorrowedFrontend as the ORIGINAL retained frame owner:
// withCpuFrontend retains source graph/target CF backing and parent pin after
// acquisition. Callback-scoped DerivedSurfaceLease is NOT a replacement
// SourceOwnerType/TargetOwnerType for that persistent frame. The temporary
// overlay must be confined to synchronous producer/packet execution while
// this same original frame retains command/resource backing for completion.
struct NativeOriginalFrameExecution {
    using Frontend=NativeRetainedFrontendRuntime;
    struct Request {
        size_t childIndex;const TglVeboxAdmissionBorrowedInterfaces& interfaces;
        uintptr_t parameters,pass;const TglOwnedVpEntrySet& catalogue;
        NativeModeRetirementAuthority& retirement;const TglNativeVphalMhwView& view;
        TglVeboxPipeAcquisition& acquisition;int& decompressionStatus;
        NativeTargetDecompression& decompression;void* argument;
        const TglOwnedGen12SurfaceFeatures* sourceFeatures=nullptr;
        const TglOwnedGen12SurfaceFeatures* targetFeatures=nullptr;
        bool attempted=false;
    };
    // Construct execution Inputs ONLY inside the original retained frontend
    // continuation. No caller-supplied core/graph/line storage or owner copies.
    // Request metadata/capabilities remain explicit; no HDR/default completion
    // or decompression producer is invented by this join.
    static TglOwnedVeboxCommandTransactionResult runRetained(NativeContextRendererFrame& frame,
        const TglNativeOuterSurfaceRoles& roles,size_t sourceIndex,size_t targetIndex,
        const std::array<const TglOwnedSurfaceStorage*,2>& temporal,Request& request) noexcept {
        if(request.attempted)return {};
        request.attempted=true;
        Frontend::VpRequest vpRequest{request.childIndex,request.interfaces,request.parameters,request.pass,
            request.catalogue,request.retirement,&request,
            +[](void* context,NativeCoreFrameDataScope& core,NativeVpGraph& graph,
                TglNativeVpLineResourceStorage& lines,NativeAdmissionParams& params,NativeAdmissionPass& pass,
                NativeFrameDataRead& read,NativeVpProducerFrame& vp,const TglOwnedVpModeState& mode,
                Frontend::VpFrontend& frontend) noexcept -> TglOwnedVeboxCommandTransactionResult {
                auto& request=*static_cast<Request*>(context);
                TglOwnedGen12SurfaceFeatures sourceFeatures,targetFeatures;
                if(!tglBindLeasedUnknownGen12SurfaceFeatures(sourceFeatures,core.sourceOwner,core.source,
                    request.acquisition) ||
                    !tglBindLeasedUnknownGen12SurfaceFeatures(targetFeatures,core.targetOwner,core.target,
                    request.acquisition))return {};
                // Optional caller-produced evidence is still checked for the
                // EXACT original surface/frame identities by withPolicyInputs.
                if(request.sourceFeatures)sourceFeatures=*request.sourceFeatures;
                if(request.targetFeatures)targetFeatures=*request.targetFeatures;
                Inputs input{frontend,core,graph,lines,params,pass,read,vp,request.catalogue,mode,request.view,
                    sourceFeatures,targetFeatures,request.acquisition,request.decompressionStatus,
                    request.decompression,request.argument};
                return run(input);
            }};
        return Frontend::runVpInputs(frame,roles,sourceIndex,targetIndex,temporal,vpRequest);
    }
    struct Inputs {
        Frontend::VpFrontend& frontend;NativeCoreFrameDataScope& core;NativeVpGraph& graph;
        TglNativeVpLineResourceStorage& lines;NativeAdmissionParams& params;NativeAdmissionPass& pass;
        NativeFrameDataRead& read;NativeVpProducerFrame& vp;
        const TglOwnedVpEntrySet& catalogue;const TglOwnedVpModeState& mode;
        const TglNativeVphalMhwView& view;
        const TglOwnedGen12SurfaceFeatures& sourceFeatures;const TglOwnedGen12SurfaceFeatures& targetFeatures;
        TglVeboxPipeAcquisition& acquisition;int& decompressionStatus;
        NativeTargetDecompression& decompression;
        void* argument;
    };
    static TglOwnedVeboxCommandTransactionResult run(Inputs& in) noexcept {
        TglOwnedVeboxCommandTransactionResult result;
        auto& core=in.core;const NativeTglImageAdmission qualify{core.image};
        TglVeboxRenderBinding render;TglCscCoefficientsBinding coefficients;bool alignDi64=false;
        if(!core.preparedReset || !core.live() ||
            !in.view.matchesCreated(in.frontend.pinned,&readMemory,NativeDataAdmission{},qualify) ||
            !render.resolve(core.image,&readMemory,qualify,&executableMemory) ||
            !coefficients.resolve(core.image,&readMemory,qualify,&executableMemory) ||
            !in.view.hardware.readBoundaryAlignDi64(core.image,&readMemory,qualify,&executableMemory,
                NativeDataAdmission{},alignDi64) ||
            !in.view.matchesCreated(in.frontend.pinned,&readMemory,NativeDataAdmission{},qualify))return result;
        NativeTwoPassCscHealth health{core.childOwner,core.executionOwner,core.sourceOwner,core.targetOwner,
            core.source,core.target,core.image,core.child,core.execution,core.generation};
        struct State {
            Inputs& input;const TglVeboxRenderBinding& render;NativeVeboxCscProducer csc;
            bool alignDi64;
            NativeAdmissionStages::Result result;
        } state{in,render,{health,coefficients},alignDi64,{}};
        const int status=Frontend::withPacketInputs(in.frontend,core,&state,
            +[](void* context,TglOwnedVeboxPacketLeases& leases) noexcept -> int {
                auto& state=*static_cast<State*>(context);auto& in=state.input;
                struct Packet {State& state;TglOwnedVeboxPacketLeases& leases;} packet{state,leases};
                return Frontend::withAllocationInputs(in.frontend,in.core,in.graph,in.lines,
                    state.alignDi64,&packet,
                    +[](void* context,NativeModeAllocationPlan& allocation) noexcept -> int {
                        auto& packet=*static_cast<Packet*>(context);auto& in=packet.state.input;
                        struct Allocation {Packet& packet;NativeModeAllocationPlan& plan;} stage{packet,allocation};
                        return Frontend::withPolicyInputs(in.core,in.params,in.pass,in.read,in.frontend.interfaces,
                            in.sourceFeatures,in.targetFeatures,in.acquisition,in.decompressionStatus,in.decompression,&stage,
                            +[](void* context,NativeOutputPipeFrameScope& policy) noexcept -> int {
                                auto& stage=*static_cast<Allocation*>(context);auto& in=stage.packet.state.input;
                                struct Policy {Allocation& stage;NativeOutputPipeFrameScope& policy;} next{stage,policy};
                                return Frontend::withAdmissionInputs(in.frontend,in.core,in.graph,in.params,in.pass,
                                    in.read,in.catalogue,in.mode,&next,
                                    +[](void* context,const NativeAdmissionFrameAssembly::Inputs& admission) noexcept -> int {
                                        auto& next=*static_cast<Policy*>(context);auto& stage=next.stage;
                                        auto& state=stage.packet.state;auto& in=state.input;
                                        NativePolicyFrameExecution::Inputs execution{in.frontend.assembly,in.view,in.vp,
                                            next.policy,in.read,admission,stage.plan,state.render,in.argument,
                                            stage.packet.leases,in.frontend.denoise,state.csc};
                                        state.result=NativePolicyFrameExecution::run(execution);
                                        return state.result.status;
                                    });
                            });
                    });
            });
        result=state.result.native; // keep acquisition/native-entry evidence on every failure
        result.status=status?status:state.result.status;
        return result;
    }
};
// Callback table entries carry no Entry/closure argument. Route the immutable
// qualified native image and actual physical generation, never "current Entry".
// Prepared backend only; factory callback publication remains disabled.
struct NativePhysicalBackend {
    inline static std::atomic<uintptr_t> image_{0};
    static NativePhysicalRegistry& registry() noexcept {return NativePhysicalRegistry::processLifetime();}
    static bool binding(TglNativePhysicalCallbackBinding& output) noexcept {
        const uintptr_t image=image_.load(std::memory_order_acquire);
        return image && output.resolve(image,&readMemory,NativeTglImageAdmission{image},&executableMemory);
    }
    static bool bind(const TglNativePhysicalCallbackBinding& candidate) noexcept {
        TglNativePhysicalCallbackBinding checked;
        if(!candidate.image || !checked.resolve(candidate.image,&readMemory,
            NativeTglImageAdmission{candidate.image},&executableMemory) ||
            checked.initialize!=candidate.initialize || checked.destroy!=candidate.destroy)return false;
        uintptr_t expected=0;
        return image_.compare_exchange_strong(expected,candidate.image,std::memory_order_acq_rel) ||
            expected==candidate.image;
    }
    static NativePhysicalCallbacks::InitResult initialize(void** output,const void* input) noexcept {
        TglNativePhysicalCallbackBinding native;
        if(!output || !input || !binding(native))return {};
        NativePhysicalCallbacks callbacks(registry(),native,&readMemory,NativeTglImageAdmission{native.image},&executableMemory);
        return callbacks.initialize(output,input);
    }
    static int destroy(uintptr_t physical) noexcept {
        TglNativePhysicalCallbackBinding native;
        if(!physical || !binding(native))return 5;
        NativePhysicalCallbacks callbacks(registry(),native,&readMemory,NativeTglImageAdmission{native.image},&executableMemory);
        return callbacks.destroy(physical);
    }
    template<class Use> static int withScope(uintptr_t physical,Use use) noexcept {
        if constexpr(std::is_pointer_v<Use>)if(!use)return 5;
        TglNativePhysicalCallbackBinding native;
        if(!physical || !binding(native))return 5;
        const auto ticket=registry().lookup(native.image,physical);
        if(!ticket.generation)return 5;
        NativePhysicalCallbacks callbacks(registry(),native,&readMemory,NativeTglImageAdmission{native.image},&executableMemory);
        return callbacks.withFrameScope(ticket,physical,[&](NativePhysicalFrameScope& scope) noexcept -> int {
            static_assert(noexcept(use(scope,ticket.generation)) &&
                std::is_same_v<decltype(use(scope,ticket.generation)),int>);
            return use(scope,ticket.generation);
        });
    }
    template<class Native> static int createContexts(uintptr_t physical,const void* input,Native nativeCreate) noexcept {
        using AdoptionScope=typename NativePhysicalRegistry::DependencyAdoptionScope;
        static_assert(noexcept(nativeCreate(uintptr_t{},uintptr_t{},input,std::declval<AdoptionScope&>())) &&
            std::is_same_v<decltype(nativeCreate(uintptr_t{},uintptr_t{},input,std::declval<AdoptionScope&>())),int>);
        if constexpr(std::is_pointer_v<Native>)if(!nativeCreate)return 5;
        TglNativePhysicalCallbackBinding native;
        if(!physical || !input || !binding(native))return 5;
        const auto ticket=registry().lookup(native.image,physical);
        if(!ticket.generation)return 5;
        NativePhysicalCallbacks callbacks(registry(),native,&readMemory,NativeTglImageAdmission{native.image},&executableMemory);
        return callbacks.createContextsAndAdoptBatchScoped(ticket,physical,input,
            [](AdoptionScope& scope,uintptr_t address,const NativeCodecBackend::Batch& batch) noexcept -> int {
                return NativeCodecBackend::adopt(scope,address,batch);
            },nativeCreate);
    }
};
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
    using ConstructionParent=TglNativeBorrowedVphalMhwLease<TglOwnedCreatorConstructionLease>;
    bool constructionResourceEnvironment(TglOwnedCreatorConstructionLease&& pin,uintptr_t creator,
        ConstructionParent& parent,TglNativeVphalResourceEnvironment& output) const noexcept {
        output={};
        if(!nativeCpu.image || !pin.get() || !parent.acquire(std::move(pin),nativeCpu.image,creator,creator,
            &readMemory,NativeDataAdmission{},NativeTglImageAdmission{nativeCpu.image},&executableMemory,
            [](const auto& identity,const TglNativeVphalMhwView& view) noexcept {
                return identity.matchesNativeParent(view,&readMemory);
            }))return false;
        // Consume/adopt the environment within the successful creator handoff.
        // Permanent owner/lifetime publication precedes child initialization;
        // native error unwind must not free OS/MHW behind retained children.
        return rendererResourceEnvironment(parent,output);
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
    template<class PhysicalScope,class Resolve,class Admit>
    NativeBorrowedFrontend* createBorrowedFrontend(PhysicalScope& physical,uintptr_t address,
        uint64_t generation,uintptr_t creator,uintptr_t source,Resolve resolve,
        NativeContextSurfaceOwner& target,Admit admit) const noexcept {
        if constexpr(std::is_pointer_v<Resolve>)if(!resolve)return nullptr;
        if constexpr(std::is_pointer_v<Admit>)if(!admit)return nullptr;
        const uintptr_t image=nativeCpu.image;
        if(!image || !physical.matches(image,address,generation))return nullptr;
        NativeBorrowedFrontend* result=nullptr;
        const int status=tglWithVphalRouterIdentity(physical,image,address,generation,creator,[&]() noexcept -> int {
            auto environment=frontendCpuEnvironment();
            auto types=[&](TglNativeFrontendCfTypes& output) noexcept {
                return physical.matches(image,address,generation) && expectedFrontendTypes(output);
            };
            auto pin=[&]() noexcept {return NativeVphalRouter::pinFrame(image,address,generation,creator);};
            auto qualified=[&](NativeRendererCpuAssembly& cpu,const TglNativeCreatedVeboxRendererView& view,
                const TglOwnedSurfaceStorage* src,const TglOwnedSurfaceStorage* dst) noexcept -> bool {
                static_assert(noexcept(admit(cpu,view,src,dst)) &&
                    std::is_same_v<decltype(admit(cpu,view,src,dst)),bool>);
                return physical.matches(image,address,generation) && view.image==image && view.creator==creator &&
                    cpu.readyForFrontend() && src && dst && admit(cpu,view,src,dst) &&
                    physical.matches(image,address,generation);
            };
            NativeFrontendAccess access(NativeDataAdmission{},NativeTglImageAdmission{image},&executableMemory);
            auto construct=tglMakeNativeBorrowedFrontendConstructor<NativeBorrowedFrontend>(
                TglDarwinTypedCfReferenceApi{},environment,types,pin,access,qualified);
            result=construct(source,resolve,target);
            return result?0:5;
        });
        return status?nullptr:result;
    }
    std::array<uintptr_t, 17> callbacks{};
    void *native = nullptr;
};
struct NativeCreatorPreparation {
    struct Record {
        uintptr_t image=0,physical=0,creator=0,renderer=0;uint64_t generation=0;
        NativeRendererCpuAssembly* storage=nullptr;NativeVphalOwner* owner=nullptr;
        bool used=false,adopted=false,lifetimePublished=false,childrenReady=false;
        bool publicationAttempted=false,childrenPublished=false;
    };
    struct Ledger {std::atomic_flag gate=ATOMIC_FLAG_INIT;std::array<Record,64> records{};};
    static Ledger& ledger() noexcept {static Ledger value;return value;}
    // Component status only. Caller MUST initialize/publish complete owned
    // children before returning success from the full native creator handoff.
    template<class PhysicalScope>
    static int prepareOwner(PhysicalScope& physical,const Entry& factory,uintptr_t image,
        uintptr_t address,uint64_t generation,uintptr_t creator) noexcept {
        return TglOwnedCreatorConstructionLease::withCreatorHandoff(physical,image,address,generation,creator,
            [&](TglOwnedCreatorConstructionLease&& pin) noexcept -> int {
                auto& storage=ledger();
                if(storage.gate.test_and_set(std::memory_order_acquire))return 5;
                struct Unlock {std::atomic_flag& gate;~Unlock(){gate.clear(std::memory_order_release);}} unlock{storage.gate};
                Record* record=nullptr;
                for(auto& candidate:storage.records) {
                    if(candidate.used && candidate.creator==creator)return 5;
                    if(!candidate.used && !record)record=&candidate;
                }
                if(!record)return 5;
                record->used=true;record->image=image;record->physical=address;
                record->generation=generation;record->creator=creator;
                Entry::ConstructionParent parent;TglNativeVphalResourceEnvironment environment;
                if(!factory.constructionResourceEnvironment(std::move(pin),creator,parent,environment) || !parent.view()) {
                    *record={};return 5; // No owned candidate/publication exists.
                }
                record->renderer=parent.view()->renderer.renderer;
                try {record->storage=new(std::nothrow) NativeRendererCpuAssembly(image,environment);}
                catch(...) {*record={};return 7;} // Failed C++ construction unwinds its members.
                if(!record->storage) {*record={};return 7;}
                // From here onward a real candidate exists. Keep ALL failure
                // records until qualified teardown, including failed adoption.
                auto dependency=physical.retainPhysicalDependency();
                record->owner=NativeVphalOwner::create(image,address,generation,creator,record->renderer,
                    dependency,record->storage,&readMemory,NativeDataPointerWriter{},NativeDataAdmission{},
                    NativeRendererChildAdmission{},NativeVphalDestructorInvoker{});
                if(!record->owner)return 5; // Persistent ledger owns failed candidate, never stack cleanup.
                if(!NativeVphalRouter::adopt(image,address,generation,record->owner))return 5;
                record->adopted=true; // Router now owns owner; record pointer is identity only.
                return tglWithVphalRouterIdentity(physical,image,address,generation,creator,[&]() noexcept -> int {
                    auto lease=NativeVphalRouter::pinForInitialization(creator);
                    if(lease.get()!=record->owner || !lease.exclusive())return 5;
                    if(!record->owner->publishLifetime(NativeVphalRouter::Callbacks::destroyEntry(),
                        NativeVphalRouter::Callbacks::deletingDestroyEntry(),&readMemory,
                        NativeTglImageAdmission{image},&executableMemory,&ownedExecutableMemory,NativeDataAdmission{}))return 5;
                    record->lifetimePublished=true;return 0;
                });
            });
    }
    template<class PhysicalScope>
    static int initializeChildren(PhysicalScope& physical,uintptr_t image,uintptr_t address,
        uint64_t generation,uintptr_t creator) noexcept {
        auto* handoff=TglOwnedScopedContext<TglOwnedCreatorHandoffContext,TglOwnedCreatorHandoffDomain>::get(reinterpret_cast<void*>(creator));
        if(!handoff || handoff->image!=image || !physical.matches(image,address,generation))return 5;
        auto& state=ledger();if(state.gate.test_and_set(std::memory_order_acquire))return 5;
        struct Unlock {std::atomic_flag& gate;~Unlock(){gate.clear(std::memory_order_release);}} unlock{state.gate};
        Record* record=nullptr;
        for(auto& candidate:state.records)if(candidate.used && candidate.image==image && candidate.physical==address &&
            candidate.generation==generation && candidate.creator==creator)record=&candidate;
        if(!record || !record->adopted || !record->lifetimePublished || record->childrenReady)return 5;
        return tglWithVphalRouterIdentity(physical,image,address,generation,creator,[&]() noexcept -> int {
            auto lease=NativeVphalRouter::pinForInitialization(creator);
            if(lease.get()!=record->owner || !lease.exclusive())return 5;
            return record->owner->withCreatorStorage(lease,NativeDataAdmission{},NativeTglImageAdmission{image},&executableMemory,
                [&](NativeRendererCpuAssembly& assembly,const TglNativeCreatedVeboxRendererView&) noexcept -> int {
                    TglNativeBorrowedVphalMhwLease<decltype(lease)> parent;
                    if(!parent.acquireOwned(std::move(lease),image,creator,creator,record->owner->lifetime(),
                        &readMemory,NativeDataAdmission{},NativeTglImageAdmission{image},&executableMemory,
                        [](const NativeVphalOwner& owner,const TglNativeVphalMhwView& view) noexcept {
                            return owner.context()==view.parent && owner.lifetime().object()==view.parent;
                        }) || !parent.view())return 5;
                    const int bases=assembly.initializeNativeBases(*parent.view(),record->owner->lifetime());
                    if(bases)return bases;
                    const int status=assembly.initializeNativeVpPair(*parent.view(),record->owner->lifetime());
                    record->childrenReady=status==0;return status;
                });
        });
    }
    template<class PhysicalScope>
    static int publishChildren(PhysicalScope& physical,uintptr_t image,uintptr_t address,
        uint64_t generation,uintptr_t creator) noexcept {
        auto* handoff=TglOwnedScopedContext<TglOwnedCreatorHandoffContext,TglOwnedCreatorHandoffDomain>::get(reinterpret_cast<void*>(creator));
        if(!handoff || handoff->image!=image || !physical.matches(image,address,generation))return 5;
        auto& state=ledger();if(state.gate.test_and_set(std::memory_order_acquire))return 5;
        struct Unlock {std::atomic_flag& gate;~Unlock(){gate.clear(std::memory_order_release);}} unlock{state.gate};
        Record* record=nullptr;
        for(auto& candidate:state.records)if(candidate.used && candidate.image==image && candidate.physical==address &&
            candidate.generation==generation && candidate.creator==creator)record=&candidate;
        if(!record || !record->childrenReady || !record->lifetimePublished || !record->adopted ||
            record->publicationAttempted || record->childrenPublished)return 5;
        record->publicationAttempted=true; // Never overwrite a partially installed/pinned pair.
        return tglWithVphalRouterIdentity(physical,image,address,generation,creator,[&]() noexcept -> int {
            auto lease=NativeVphalRouter::pinForInitialization(creator);
            if(lease.get()!=record->owner || !lease.exclusive())return 5;
            return record->owner->withCreatorStorage(lease,NativeDataAdmission{},NativeTglImageAdmission{image},&executableMemory,
                [&](NativeRendererCpuAssembly& assembly,const TglNativeCreatedVeboxRendererView&) noexcept -> int {
                    TglNativeBorrowedVphalMhwLease<decltype(lease)> parent;
                    if(!parent.acquireOwned(std::move(lease),image,creator,creator,record->owner->lifetime(),
                        &readMemory,NativeDataAdmission{},NativeTglImageAdmission{image},&executableMemory,
                        [](const NativeVphalOwner& owner,const TglNativeVphalMhwView& view) noexcept {
                            return owner.context()==view.parent && owner.lifetime().object()==view.parent;
                        }) || !parent.view() || assembly.backing().retention()())return 5;
                    const int installed=NativeRendererCompleteHooks::install(assembly,*parent.view());
                    if(installed)return installed;
                    auto configuration=[&](uintptr_t renderer,uint64_t epoch,uintptr_t slot) noexcept {
                        return physical.matches(image,address,generation) && renderer==record->renderer &&
                            epoch==generation && renderer<=UINTPTR_MAX-0x2cb8 && slot==renderer+0x2cb0 &&
                            writableDataMemory(slot,8);
                    };
                    auto publish=[&](void* renderer,uint64_t epoch,uintptr_t first,uintptr_t second) noexcept {
                        return physical.matches(image,address,generation) && reinterpret_cast<uintptr_t>(renderer)==record->renderer &&
                            epoch==generation && first==reinterpret_cast<uintptr_t>(assembly.children().first().owner().constructedBase()) &&
                            second==reinterpret_cast<uintptr_t>(assembly.children().second().owner().constructedBase()) &&
                            writableDataMemory(record->renderer,0x928);
                    };
                    record->childrenPublished=assembly.children().publishFromCreatorOwned(image,creator,generation,
                        record->owner->lifetime(),&readMemory,NativeDataAdmission{},NativeTglImageAdmission{image},
                        &executableMemory,configuration,publish);
                    return record->childrenPublished?0:5;
                });
        });
    }
    // Full synchronous construction handoff, not a component-success alias.
    // Each component keeps its persistent partial state; failure never retries
    // construction or destroys objects still owned by the native creator.
    template<class PhysicalScope>
    static int complete(PhysicalScope& physical,const Entry& factory,uintptr_t image,
        uintptr_t address,uint64_t generation,uintptr_t creator) noexcept {
        const int owner=prepareOwner(physical,factory,image,address,generation,creator);
        if(owner)return owner;
        if(!physical.matches(image,address,generation))return 5;
        const int children=initializeChildren(physical,image,address,generation,creator);
        if(children)return children;
        if(!physical.matches(image,address,generation))return 5;
        return publishChildren(physical,image,address,generation,creator);
    }
};
std::map<void *, std::unique_ptr<Entry>> registry;
// Prepared invocation only: factory callbacks remain unchanged until complete
// child hooks/publication and frame retirement are supplied. Keep native status
// separate from bridge admission so a missed call cannot masquerade as handoff.
struct NativeCreatorScopedInvocation {
    struct Result {
        int nativeResult=5,handoffResult=5;
        bool nativeInvoked=false,selected=false,metadataCopied=false,handoffCompleted=false;
    };
    using Scope=typename NativePhysicalRegistry::DependencyAdoptionScope;
    using AdmitFn=bool(*)(void*) noexcept;
    using HandoffFn=int(*)(void*) noexcept;
    struct BatchResult {
        int status=5;
        Result creator;
    };
    // Join construction to the existing physical dependency-adoption scope.
    // Native output publication is adopted only after the complete creator
    // handoff; a native success with missing handoff is an ambiguous failure,
    // not permission to release partial owners or retry native construction.
    // Prepared entry only: this does not replace any exported factory slot.
    template<class Admit>
    static BatchResult createAndAdopt(const Entry& factory,uintptr_t entry,
        uintptr_t physical,const void* input,Admit& admit) noexcept {
        BatchResult result;
        TglNativePhysicalCallbackBinding selected;
        if(!factory.physical.matchesCreatorInputs(factory.nativeCpu,entry) ||
            !NativePhysicalBackend::binding(selected) || selected.image!=factory.physical.image ||
            selected.initialize!=factory.physical.initialize || selected.destroy!=factory.physical.destroy)
            return result;
        result.status=NativePhysicalBackend::createContexts(physical,input,
            [&](uintptr_t image,uintptr_t actual,const void* request,Scope& scope) noexcept -> int {
                if(actual!=physical || request!=input || scope.image()!=image)return 5;
                result.creator=runComplete(factory,entry,actual,request,scope,admit);
                if(!result.creator.nativeInvoked)return 5;
                if(result.creator.nativeResult)return result.creator.nativeResult;
                return result.creator.handoffCompleted?0:5;
            });
        return result;
    }
    template<class Admit>
    static Result runComplete(const Entry& factory,uintptr_t entry,uintptr_t physical,
        const void* input,Scope& scope,Admit& admit) noexcept {
        const uintptr_t image=scope.image();
        const uint64_t generation=scope.generation();
        if(image!=factory.physical.image || !factory.physical.matchesCreatorInputs(factory.nativeCpu,entry))
            return {};
        // Borrow the mutation capability from the SAME batch adoption scope;
        // never synthesize a second physical owner or let authority escape.
        auto handoff=[&](void* creator) noexcept -> int {
            if(!scope.matches(image,physical,generation))return 5;
            return scope.withMutationAuthority([&](TglOwnedPhysicalMutationAuthority& authority) noexcept -> int {
                return NativeCreatorPreparation::complete(authority,factory,image,physical,generation,
                    reinterpret_cast<uintptr_t>(creator));
            });
        };
        return run(entry,physical,input,scope,admit,handoff);
    }
    template<class Admit,class Handoff>
    static Result run(uintptr_t entry,uintptr_t physical,const void* input,Scope& physicalScope,
        Admit& admit,Handoff& handoff) noexcept {
        static_assert(noexcept(admit(static_cast<void*>(nullptr))) &&
            noexcept(handoff(static_cast<void*>(nullptr))));
        Result result;
        const uintptr_t image=physicalScope.image();
        const uint64_t generation=physicalScope.generation();
        if(!image || image>UINTPTR_MAX-0x75bb78 || entry!=image+0x13e43 || !input ||
            !physicalScope.matches(image,physical,generation) || !executableMemory(entry,1))return result;
        if constexpr(std::is_pointer_v<Admit>)if(!admit)return result;
        if constexpr(std::is_pointer_v<Handoff>)if(!handoff)return result;
        auto qualifiedCreator=[&](void* creator) noexcept -> bool {
            return physicalScope.matches(image,physical,generation) && admit(creator);
        };
        auto completeHandoff=[&](void* creator) noexcept -> int {
            if(!physicalScope.matches(image,physical,generation))return 5;
            return handoff(creator); // Full handoff, NOT prepareOwner component status.
        };
        TglNativeCreatorCallInvoker nativeCreate;
        TglOwnedCreatorCallScope<decltype(nativeCreate),decltype(completeHandoff),decltype(qualifiedCreator)>
            creatorScope(nativeCreate,completeHandoff,qualifiedCreator);
        if(!creatorScope.prepare(image,&readMemory,NativeTglImageAdmission{image},
            &executableMemory,&ownedExecutableMemory))return result;
        // Keep the proven metadata repair alive across the SAME native call.
        // Separate observers cannot own the thread's debug state concurrently.
        constexpr uint8_t copyAnchor[]={0xe8,0x9c,0x42,0xf3,0xff};
        constexpr uint8_t creatorAnchor[]={0xff,0x51,0x18};
        uint8_t actualCopy[sizeof(copyAnchor)]{};
        uint8_t actualCreator[sizeof(creatorAnchor)]{};
        std::vector<uint8_t> metadata;
        try { metadata.resize(TglKernelMetadataAdapter::blobBytes); }
        catch (...) { return result; }
        if(!executableMemory(image+0x133c0f,sizeof(creatorAnchor)) ||
            !readMemory(image+0x133c0f,actualCreator,sizeof(actualCreator)) ||
            std::memcmp(actualCreator,creatorAnchor,sizeof(creatorAnchor)) ||
            !executableMemory(image+0x13260f,sizeof(copyAnchor)) ||
            !readMemory(image+0x13260f,actualCopy,sizeof(actualCopy)) ||
            std::memcmp(actualCopy,copyAnchor,sizeof(copyAnchor)) ||
            !readMemory(image+0x531550,metadata.data(),metadata.size()) ||
            !TglKernelMetadataAdapter::translate(metadata))return result;
        NativeReturnObserver observer({image+0x13260f,image+0x133c0f,0,0},
            0,0,0,0,0,0,
            {image+0x531550,reinterpret_cast<uintptr_t>(metadata.data()),metadata.size()},0,
            {creatorScope.table().source(),creatorScope.table().addressPoint(),1});
        if(!observer.isArmed())return result;
        result.nativeInvoked=true;
        result.nativeResult=reinterpret_cast<int(*)(void*,const void*)>(entry)(
            reinterpret_cast<void*>(physical),input);
        result.selected=observer.observed(1,1) && creatorScope.consumed();
        result.metadataCopied=observer.observed(0,1);
        result.handoffResult=creatorScope.result();
        result.handoffCompleted=result.selected && result.metadataCopied && creatorScope.handoffCompleted() &&
            physicalScope.matches(image,physical,generation);
        return result;
    }
};
template NativeCreatorScopedInvocation::Result NativeCreatorScopedInvocation::run<
    NativeCreatorScopedInvocation::AdmitFn,NativeCreatorScopedInvocation::HandoffFn>(
    uintptr_t,uintptr_t,const void*,NativeCreatorScopedInvocation::Scope&,
    NativeCreatorScopedInvocation::AdmitFn&,NativeCreatorScopedInvocation::HandoffFn&) noexcept;
template NativeCreatorScopedInvocation::Result NativeCreatorScopedInvocation::runComplete<
    NativeCreatorScopedInvocation::AdmitFn>(const Entry&,uintptr_t,uintptr_t,const void*,
    NativeCreatorScopedInvocation::Scope&,NativeCreatorScopedInvocation::AdmitFn&) noexcept;
template NativeCreatorScopedInvocation::BatchResult NativeCreatorScopedInvocation::createAndAdopt<
    NativeCreatorScopedInvocation::AdmitFn>(const Entry&,uintptr_t,uintptr_t,const void*,
    NativeCreatorScopedInvocation::AdmitFn&) noexcept;
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
bool ownedExecutableMemory(uintptr_t address,size_t length) noexcept {
    Dl_info candidate{},owned{};
    return ownedImageHandle && dladdr(reinterpret_cast<void*>(address),&candidate) && candidate.dli_fbase &&
        dladdr(reinterpret_cast<void*>(&NGRN_ObservedCreateContexts),&owned) &&
        candidate.dli_fbase==owned.dli_fbase && executableMemory(address,length);
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
template class TglOwnedVphalProcessRouter<NativeVphalOwner,NativeVphalRoutingPolicy>;
template struct TglOwnedVeboxSfcModeQuery<true,NativeRendererChildOwner,NativeVpModeResolve>;
template struct TglOwnedVeboxSfcModeQuery<false,NativeRendererChildOwner,NativeVpModeResolve>;
template class TglOwnedVeboxModeReleaseContext<NativeSurfaceFrameBacking,NativeVpLines,NativeModeRetirementAuthority>;
template int NativeModeCallbackScope::run<int(*)(NativeModeReleaseContext&) noexcept>(
    NativeSurfaceFrameBacking&,NativeVpLines&,NativeSfcReleaseSelection&,NativeModeRetirementAuthority&,
    int(*)(NativeModeReleaseContext&) noexcept) noexcept;
template struct TglOwnedVeboxAllocationFramePlanner<TglDarwinStmmSurfaceApi,
    NativeContextSurfaceOwner,NativeRendererChildOwner,NativeExecutionOwner,decltype(&readMemory),
    NativeAllocationSfcFormat,NativeAllocationSfcColor,NativeAllocationParameterKind,
    TglOwnedFfdiCompressionFormatQuery,NativeAllocationRectangle,NativeDataAdmission,NativeAllocationUnwind>;
template int tglCopyCapturedVeboxDirectRectangle<NativeContextSurfaceOwner,NativeContextSurfaceOwner,
    NativeRendererChildOwner,NativeExecutionOwner,decltype(&readMemory)>(NativeContextSurfaceOwner&,
    const TglOwnedSurfaceStorage&,NativeContextSurfaceOwner&,TglOwnedSurfaceStorage&,
    NativeRendererChildOwner&,NativeExecutionOwner&,void*,uintptr_t,decltype(&readMemory)) noexcept;
template int NativeRendererCpuAssembly::withChildExecution<
    int(*)(NativeRendererChildOwner&,NativeExecutionOwner&,void*,uintptr_t) noexcept>(size_t,
    int(*)(NativeRendererChildOwner&,NativeExecutionOwner&,void*,uintptr_t) noexcept) noexcept;
template class TglNativeVeboxBorrowedFrontendWithCpuAllocator<NativeVphalFramePin,
    NativeContextSurfaceOwner,NativeContextSurfaceOwner,TglDarwinTypedCfReferenceApi,
    NativeFrontendEnvironment,NativeFrontendAccess>;
template class TglNativeContextRendererFrameStorage<NativeCodecBackend::Registry,NativeBorrowedFrontend>;
template class TglOwnedVeboxPhysicalFrameRegistry<NativeContextRendererFrame>;
template class TglOwnedPhysicalCallbackCoordinator<NativePhysicalRegistry,
    decltype(&readMemory),NativeTglImageAdmission,decltype(&executableMemory)>;
template int NativePhysicalBackend::withScope<int(*)(NativePhysicalFrameScope&,uint64_t) noexcept>(
    uintptr_t,int(*)(NativePhysicalFrameScope&,uint64_t) noexcept) noexcept;
using NativeScopedContextCreate=int(*)(uintptr_t,uintptr_t,const void*,NativePhysicalRegistry::DependencyAdoptionScope&) noexcept;
template int NativePhysicalBackend::createContexts<NativeScopedContextCreate>(
    uintptr_t,const void*,NativeScopedContextCreate) noexcept;
using NativeFrontendResolve=NativeContextSurfaceOwner*(*)(uintptr_t) noexcept;
using NativeFrontendConsumer=TglOwnedVeboxCommandTransactionResult(*)(NativeRendererCpuAssembly&,
    NativeContextSurfaceOwner&,NativeContextSurfaceOwner&,NativeContextSurfaceOwner*,NativeContextSurfaceOwner*,
    TglTrackedCpuOwner<TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>>&,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept;
template TglOwnedVeboxCommandTransactionResult NativeFrontendRuntimeScope::run<NativeFrontendConsumer>(
    NativeBorrowedFrontend&,const TglNativeOuterSurfaceRoles&,size_t,size_t,
    const std::array<const TglOwnedSurfaceStorage*,2>&,NativeFrontendConsumer) noexcept;
using NativeFrontendAdmit=bool(*)(NativeRendererCpuAssembly&,const TglNativeCreatedVeboxRendererView&,
    const TglOwnedSurfaceStorage*,const TglOwnedSurfaceStorage*) noexcept;
template NativeBorrowedFrontend* Entry::createBorrowedFrontend<TglOwnedPhysicalMutationAuthority,
    NativeFrontendResolve,NativeFrontendAdmit>(TglOwnedPhysicalMutationAuthority&,uintptr_t,uint64_t,
    uintptr_t,uintptr_t,NativeFrontendResolve,NativeContextSurfaceOwner&,NativeFrontendAdmit) const noexcept;
template NativeBorrowedFrontend* Entry::createBorrowedFrontend<NativePhysicalFrameScope,
    NativeFrontendResolve,NativeFrontendAdmit>(NativePhysicalFrameScope&,uintptr_t,uint64_t,
    uintptr_t,uintptr_t,NativeFrontendResolve,NativeContextSurfaceOwner&,NativeFrontendAdmit) const noexcept;
using NativeChildVpResourceUse=TglOwnedVeboxCommandTransactionResult(*)(NativeRendererChildOwner&,
    NativeExecutionOwner&,void*,uintptr_t,NativeVpGraph&,TglOwnedVpSfcCallerStorage&,
    TglNativeVpLineResourceStorage&) noexcept;
template TglOwnedVeboxCommandTransactionResult NativeRendererCpuAssembly::withChildVpResources<NativeChildVpResourceUse>(
    size_t,NativeChildVpResourceUse) noexcept;
template int NativeRendererCpuAssembly::withChildLifetimeScopes<
    int(*)(void*,void*,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept>(
    int(*)(void*,void*,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept) noexcept;
template int NativeCreatorPreparation::prepareOwner<TglOwnedPhysicalMutationAuthority>(
    TglOwnedPhysicalMutationAuthority&,const Entry&,uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;
template int NativeCreatorPreparation::initializeChildren<TglOwnedPhysicalMutationAuthority>(
    TglOwnedPhysicalMutationAuthority&,uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;
template int NativeCreatorPreparation::publishChildren<TglOwnedPhysicalMutationAuthority>(
    TglOwnedPhysicalMutationAuthority&,uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;
template int NativeCreatorPreparation::complete<TglOwnedPhysicalMutationAuthority>(
    TglOwnedPhysicalMutationAuthority&,const Entry&,uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;

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
        if(!retainOwnedHookImage() ||
            !ownedExecutableMemory(reinterpret_cast<uintptr_t>(&NGRN_ObservedCreateContexts),1))return 10;
        // Owned callbacks are pinned RX/no-W code before native factory effects.
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
