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
using NativeSourceGraph=TglOwnedFrontendSourceGraph<NativeContextSurfaceOwner,TglDarwinTypedCfReferenceApi>;
using NativeAdmissionHistoryStage=TglOwnedVeboxHistoryAdmissionStage<NativeRendererChildOwner,
    NativeExecutionOwner,NativeSourceGraph,NativeDataAdmission,NativeDataAdmission>;
struct NativeAdmissionHistorySetup {
    NativeAdmissionHistoryStage stage;
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
using NativeDenoiseParameters=TglTrackedCpuOwner<TglTrackedCpuBackend<TglNativeTrackedCpuInvoker>>;
using NativeAdmissionFeatureStage=TglOwnedVeboxFeatureAdmissionStage<NativeContextSurfaceOwner,
    NativeContextSurfaceOwner,NativeRendererChildOwner,NativeExecutionOwner,NativeDenoiseParameters,
    decltype(&readMemory),NativeTglImageAdmission,decltype(&executableMemory)>;
struct NativeAdmissionFeaturePreparation {
    template<class Use> static int run(NativeAdmissionFeatureStage& stage,Use use) noexcept {
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
    static bool apply(NativeAdmissionFeatureStage& stage,TglLeasedAdmissionExecution& report) noexcept {
        if(stage.read!=&readMemory || stage.executable!=&executableMemory ||
            !stage.binding.image || stage.qualify.image!=stage.binding.image ||
            !stage.parameters.hasNativeReleaseSlot(stage.source.prefix.data()+0x90))return false;
        return stage(report);
    }
};
template int NativeAdmissionFeaturePreparation::run<int(*)(NativeAdmissionFeatureStage&) noexcept>(
    NativeAdmissionFeatureStage&,int(*)(NativeAdmissionFeatureStage&) noexcept) noexcept;
struct NativeAllocationParameterKind {
    NativeContextSurfaceOwner& owner;const TglOwnedSurfaceStorage& source;
    bool operator()(uintptr_t parameter,uint32_t& output) const noexcept {
        return owner.readLeasedDiParameterKind(source,parameter,output,&readMemory);
    }
};
struct NativeAllocationRectangle {
    NativeContextSurfaceOwner& input;NativeContextSurfaceOwner& output;
    NativeRendererChildOwner& child;NativeExecutionOwner& execution;
    int operator()(void* actual,uintptr_t report,const TglOwnedSurfaceStorage& source,
        const TglOwnedFfdiTailViews&,const TglOwnedFfdnTailViews&) const noexcept {
        return output.withMutableSurface([&](auto& target) noexcept -> int {
            return tglCopyCapturedVeboxDirectRectangle(input,source,output,target,child,
                execution,actual,report,&readMemory);
        });
    }
};
struct NativeModeRetirementAuthority {
    void* scope=nullptr;int(*admit)(void*) noexcept=nullptr;
    int operator()() const noexcept {return scope && admit?admit(scope):5;}
};
using NativeRendererBacking=std::remove_reference_t<decltype(std::declval<NativeRendererCpu&>().backing())>;
struct NativeAdmissionReset {
    NativeRendererChildOwner& childOwner;
    NativeExecutionOwner& executionOwner;
    NativeRendererBacking& backing;
    NativeModeRetirementAuthority& retirement;
    size_t index;void* child;uintptr_t execution;void* vp;
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
        VpReset reset{childOwner,backing,index,child,vp,admitRetirement};
        TglOwnedVeboxAdmissionResetStage<NativeRendererChildOwner,NativeExecutionOwner,VpReset>
            stage{childOwner,executionOwner,child,execution,reset};
        const bool accepted=stage();
        return accepted && childOwner.allocationGeneration()==generation &&
            childOwner.hasPrivateChildLease(address) && executionOwner.hasExecutionLease(execution) &&
            !backing.retention()();
    }
};
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
        return status;
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
using NativeAllocationPlanner=TglOwnedVeboxAllocationFramePlanner<TglDarwinStmmSurfaceApi,
    NativeContextSurfaceOwner,NativeRendererChildOwner,NativeExecutionOwner,decltype(&readMemory),
    NativeAllocationSfcFormat,NativeAllocationSfcColor,NativeAllocationParameterKind,
    TglOwnedFfdiCompressionFormatQuery,NativeAllocationRectangle,NativeDataAdmission,NativeAllocationUnwind>;
using NativeAllocationContext=TglOwnedVeboxRenderAllocationContext<NativeRendererChildOwner,
    NativeAllocationPlanner>;
using NativeAllocationResolve=TglOwnedVeboxScopedContext<NativeAllocationContext>;
struct NativeAllocationCallbackScope {
    static int allocate(void* child) noexcept {
        return TglOwnedVeboxAllocationCallbacks<NativeAllocationResolve>::allocate(child);
    }
    template<class Use> static int run(NativeAllocationPlanner& planner,NativeModeReleaseContext& mode,
        TglOwnedVeboxFrameRetention& renderRetention,Use use) noexcept {
        static_assert(noexcept(use(std::declval<NativeAllocationContext&>())) &&
            std::is_same_v<decltype(use(std::declval<NativeAllocationContext&>())),int>);
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
        NativeAllocationContext allocation(planner.childOwner,planner,child,{},keep);
        NativeAllocationResolve scope(child,allocation);
        if(!scope.bound() || !allocation.owns(child))return 5;
        const int status=use(allocation);
        return status?status:(allocation.owns(child) && NativeModeReleaseScope::get(child)==&mode?0:5);
    }
};
template int NativeAllocationCallbackScope::run<int(*)(NativeAllocationContext&) noexcept>(
    NativeAllocationPlanner&,NativeModeReleaseContext&,TglOwnedVeboxFrameRetention&,
    int(*)(NativeAllocationContext&) noexcept) noexcept;
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
        return frontend.withCpuFrontend([&](NativeRendererCpuAssembly& cpu,auto& source,auto& target,auto& parameters) noexcept {
            if(!cpu.readyForFrontend())return TglOwnedVeboxCommandTransactionResult{};
            return tglWithOriginalFrontendOwners(roles,sourceIndex,targetIndex,source,target,temporal,
                cpu.backing().retention(),[&](auto& input,auto& output,auto* secondary,auto* previous) noexcept {
                    TglOwnedVeboxCommandTransactionResult result;
                    const int status=cpu.withChildLifetimeScopes([&](void*,void*,auto& firstLife,auto& secondLife) noexcept -> int {
                        static_assert(noexcept(use(cpu,input,output,secondary,previous,parameters,firstLife,secondLife)) &&
                            std::is_same_v<decltype(use(cpu,input,output,secondary,previous,parameters,firstLife,secondLife)),
                                TglOwnedVeboxCommandTransactionResult>);
                        result=use(cpu,input,output,secondary,previous,parameters,firstLife,secondLife);
                        cpu.backing().retention().observe(result);
                        return result.status;
                    });
                    if(!result.status && status)result.status=status;
                    return result;
                });
        });
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
                if(!factory.constructionResourceEnvironment(std::move(pin),creator,parent,environment) || !parent.view())return 5;
                record->renderer=parent.view()->renderer.renderer;
                try {record->storage=new(std::nothrow) NativeRendererCpuAssembly(image,environment);}
                catch(...) {return 7;}
                if(!record->storage)return 7;
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
};
std::map<void *, std::unique_ptr<Entry>> registry;
// Prepared invocation only: factory callbacks remain unchanged until complete
// child hooks/publication and frame retirement are supplied. Keep native status
// separate from bridge admission so a missed call cannot masquerade as handoff.
struct NativeCreatorScopedInvocation {
    struct Result {
        int nativeResult=5,handoffResult=5;
        bool nativeInvoked=false,selected=false,handoffCompleted=false;
    };
    using Scope=typename NativePhysicalRegistry::DependencyAdoptionScope;
    using AdmitFn=bool(*)(void*) noexcept;
    using HandoffFn=int(*)(void*) noexcept;
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
        NativeReturnObserver observer({image+0x133c0f,0,0,0},0,0,0,0,0,0,{},0,
            {creatorScope.table().source(),creatorScope.table().addressPoint()});
        if(!observer.isArmed())return result;
        result.nativeInvoked=true;
        result.nativeResult=reinterpret_cast<int(*)(void*,const void*)>(entry)(
            reinterpret_cast<void*>(physical),input);
        result.selected=observer.observed(0,1) && creatorScope.consumed();
        result.handoffResult=creatorScope.result();
        result.handoffCompleted=result.selected && creatorScope.handoffCompleted() &&
            physicalScope.matches(image,physical,generation);
        return result;
    }
};
template NativeCreatorScopedInvocation::Result NativeCreatorScopedInvocation::run<
    NativeCreatorScopedInvocation::AdmitFn,NativeCreatorScopedInvocation::HandoffFn>(
    uintptr_t,uintptr_t,const void*,NativeCreatorScopedInvocation::Scope&,
    NativeCreatorScopedInvocation::AdmitFn&,NativeCreatorScopedInvocation::HandoffFn&) noexcept;
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
template int NativeRendererCpuAssembly::withChildLifetimeScopes<
    int(*)(void*,void*,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept>(
    int(*)(void*,void*,NativeRendererChildLifetime&,NativeRendererChildLifetime&) noexcept) noexcept;
template int NativeCreatorPreparation::prepareOwner<TglOwnedPhysicalMutationAuthority>(
    TglOwnedPhysicalMutationAuthority&,const Entry&,uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;
template int NativeCreatorPreparation::initializeChildren<TglOwnedPhysicalMutationAuthority>(
    TglOwnedPhysicalMutationAuthority&,uintptr_t,uintptr_t,uint64_t,uintptr_t) noexcept;

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
