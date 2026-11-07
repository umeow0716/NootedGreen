//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_tgl_accel.hpp"
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include "kern_guc_ring.hpp"
#include "kern_vf_cpu_range.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_ggtt_bounds.hpp"
#include "kern_ggtt_rotation.hpp"
#include "kern_vf_ggtt_pte.hpp"
#include "kern_vf_irq_gate.hpp"
#include "kern_vf_accelerator_stop.hpp"
#include "kern_vf_context_shutdown.hpp"
#include "kern_vf_submission_gate.hpp"
#include "kern_avc_gen12_commands.hpp"
#include "kern_vf_runtime.hpp"
#include "kern_vf_runtime_patch.hpp"
#include "kern_vf_mmio_response.hpp"
#include "kern_vf_legacy_ctb.hpp"
#include "kern_vf_tlb_patch.hpp"
#include "kern_vf_standalone_patch.hpp"
#include "kern_vf_guc_factory_patch.hpp"
#include "kern_vf_blit3d_scratch_patch.hpp"
#include "kern_unaligned_patch.hpp"
#include "kern_framebuffer_patch.hpp"
#include "kern_vf_guc_event.hpp"
#include "kern_vf_guc_request.hpp"
#include "kern_vf_memirq.hpp"
#include "kern_context_pool.hpp"
#include "kern_context_descriptor.hpp"
#include "kern_workqueue_unwind.hpp"
#include "kern_binary_identity.hpp"
#include "kern_ioaccel_command_pool.hpp"
#include "kern_event_vector.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include "kern_green.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSSet.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <i386/machine_routines.h>

// V373: on this VF setup xnu (x86) sometimes removes a task's mapping of a
// shared-cache __DATA page (shared by several tasks, so its PV entry is a
// hashed one) without unlinking that PV entry. The entry outlives the pmap; once
// the pmap memory is reused, a walk of the page's PV list (phys_attribute_test
// via mach_vm_page_range_query: heap at a shutdown stall, debugserver) finds no
// PTE and panics. pmap_remove_options is wrapped: present PTEs in the shared
// region are noted before the removal, and any PV entry still naming the pmap
// at a now-empty PTE afterwards is unlinked under xnu's PV and hash bit locks.
// The unlinked hashed entry (48 bytes) is left allocated.
#include "kern_vf_internal.hpp"

// TGL FB — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLFB[] = {
    "/Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FBT {"com.xxxxx.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11FBTA {"com.apple.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
	{false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.xxxxx (loaded from /Library/Extensions/)
// Loaded: kernelmanagerd sometimes loads it from the AuxKC (with
// IOAcceleratorFamily2) before Lilu activates; Lilu then skips it unless the
// handler also takes already loaded kexts, and it starts unpatched.
static const char *pathsTGLHW[] = {
    "/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
};
static KernelPatcher::KextInfo kextG11HWT {"com.xxxxx.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
    {true, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11HWTA {"com.apple.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
	{true, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// Resolve the native accelerator lifecycle API from the running Tahoe
// IOAcceleratorFamily2 image.  The VF wrapper calls these named methods instead
// of writing IOGraphicsAccelerator2 private state at hardcoded offsets.
static const char *pathsIOAcceleratorFamily2[] = {
	"/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2",
};
static KernelPatcher::KextInfo kextIOAcceleratorFamily2 {
	"com.apple.iokit.IOAcceleratorFamily2", pathsIOAcceleratorFamily2, 1,
	{true, false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded
};

// Resolve the Tahoe PCI interrupt allocator from its owning fileset.  Do not
// search KernelPatcher::KernelID: IOPCIDevice lives in IOPCIFamily even when
// both images are linked into the same BootKC container.
static const char *pathsIOPCIFamily[] = {
	"/System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily",
};
static KernelPatcher::KextInfo kextIOPCIFamily {
	"com.apple.iokit.IOPCIFamily", pathsIOPCIFamily, 1,
	{true, false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded
};

TglAccel *TglAccel::callback = nullptr;

void TglAccel::init() {
	callback = this;
	const bool tglRequested = checkKernelArgument("-ngreentglfb") ||
		checkKernelArgument("-ngreentglwithgfx") ||
		checkKernelArgument("-ngreentglgfx");
	if (tglRequested) {
		lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
		lilu.onKextLoadForce(&kextIOPCIFamily);
	}

	if (checkKernelArgument("-ngreentglfb") || checkKernelArgument("-ngreentglwithgfx")) {
		SYSLOG("ngreen", "TglAccel::init: FB tier → TGL");
		lilu.onKextLoadForce(&kextG11FBT);
		lilu.onKextLoadForce(&kextG11FBTA);
		if (checkKernelArgument("-ngreentglwithgfx")) {
			SYSLOG("ngreen", "TglAccel::init: HW tier → TGL");
			lilu.onKextLoadForce(&kextG11HWT);
			lilu.onKextLoadForce(&kextG11HWTA);
		}
	} else if (checkKernelArgument("-ngreentglgfx")) {
		SYSLOG("ngreen", "TglAccel::init: HW tier → TGL");
		lilu.onKextLoadForce(&kextG11HWT);
		lilu.onKextLoadForce(&kextG11HWTA);
	}
}

static void *vfRejectPhysicalFramebufferProbe(void *, void *, int *) {
	return nullptr;
}

static bool vfRejectPhysicalFramebufferStart(void *, void *) {
	return false;
}

// V368: when TGL was loaded before Lilu activated, Lilu replays the loaded
// kexts newest first, so TGL is processed before IOPCIFamily and
// IOAcceleratorFamily2. Publishing the accelerator personality starts it, and
// its start needs both (MSI configurator, lifecycle API, routes): publish once
// all three are processed, whatever their order.
static const char *gPendingAcceleratorBundle;
static bool gIoAccelLifecycleResolved; // IOAcceleratorFamily2 fully processed

// Lilu handles a kext load by processing only the head of the kmod list. When
// kernelmanagerd loads IOAcceleratorFamily2 and TGL back to back, the first
// notification can already find the second kext at the head: one of them is
// never processed (no accelerator, or a start without its dependencies). A
// while after the early TGL load, process whatever was missed the way Lilu
// would have, in dependency order.
static volatile UInt32 gTglProcessed, gIoAccelProcessing;

void TglAccel::recoverMissedKexts(KernelPatcher &patcher) {
	auto **kmods = reinterpret_cast<kmod_info_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_kmod"));
	patcher.clearError();
	KernelPatcher::KextInfo *const order[] {&kextIOPCIFamily, &kextIOAcceleratorFamily2, &kextG11HWT, &kextG11HWTA};
	for (auto *info : order) {
		const bool done = info == &kextIOPCIFamily ? this->ioPciConfigureInterrupts != 0 :
			info == &kextIOAcceleratorFamily2 ? gIoAccelProcessing != 0 : gTglProcessed != 0;
		if (done || info->loadIndex == KernelPatcher::KextInfo::Unloaded || !kmods)
			continue;
		kmod_info_t *kmod = *kmods;
		while (kmod && strncmp(kmod->name, info->id, KMOD_MAX_NAME))
			kmod = kmod->next;
		if (!kmod)
			continue;
		const auto address = static_cast<mach_vm_address_t>(kmod->address);
		const size_t size = patcher.updateRunningInfo(info->loadIndex, address, kmod->size);
		patcher.clearError();
		SYSLOG("ngreen", "V368: load of %s was missed; processing it now", info->id);
		NGreen::callback->processKext(patcher, info->loadIndex, address, size);
	}
}

void TglAccel::publishAcceleratorWhenReady() {
	if (!gPendingAcceleratorBundle || !this->ioPciConfigureInterrupts || !gIoAccelLifecycleResolved)
		return;
	const char *bundleId = gPendingAcceleratorBundle;
	gPendingAcceleratorBundle = nullptr;
	PANIC_COND(!injectAcceleratorPersonality(bundleId), "ngreen",
		"Cannot publish complete TGL accelerator personality");
	if (gVfIdentity == VfIdentity::Virtual)
		ngArmV349MediaExecObservation();
}

bool TglAccel::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index == kextIOPCIFamily.loadIndex) {
		if (!this->ioPciConfigureInterrupts)
			this->ioPciConfigureInterrupts = patcher.solveSymbol(
				index, "__ZN11IOPCIDevice19configureInterruptsEjjjj",
				address, size);
		PANIC_COND(!this->ioPciConfigureInterrupts, "ngreen",
			"Cannot resolve the exported Tahoe PCI MSI configurator");
		SYSLOG("ngreen", "V246: resolved Tahoe IOPCIFamily PCI interrupt allocator");
		publishAcceleratorWhenReady();
		return true;
	}

	if (index == kextIOAcceleratorFamily2.loadIndex) {
		if (!OSCompareAndSwap(0, 1, &gIoAccelProcessing))
			return true;
		const auto *ioAccelImage = reinterpret_cast<const uint8_t *>(address);
		PANIC_COND(!NGBinaryIdentity::matchesKextUuid(
		               ioAccelImage, size,
		               NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid),
		           "ngreen", "Unsupported Tahoe IOAcceleratorFamily2 private ABI");
		V373::install(patcher);
		KernelPatcher::SolveRequest lifecycle[] = {
			{"__ZN22IOGraphicsAccelerator217enableAcceleratorEv",
			 this->ioGraphicsEnableAccelerator},
			{"__ZN22IOGraphicsAccelerator218disableAcceleratorEv",
			 this->ioGraphicsDisableAccelerator},
			{"__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent",
			 this->ioAccelEventMachineInitEvent},
		};
		PANIC_COND(!patcher.solveMultiple(index, lifecycle, address, size),
			"ngreen", "Cannot resolve native IOAccelerator lifecycle API");
		KernelPatcher::RouteRequest callbackOwnerTeardown[] = {
			{"__ZN22IOGraphicsAccelerator24stopEP9IOService",
			 vfBaseAcceleratorStop, this->oVfBaseAcceleratorStop},
			{"__ZN24IOAccelEventMachineFast215finishAllStampsEv",
			 vfEventMachineFinishAllStamps,
			 this->oVfEventMachineFinishAllStamps},
			{"__ZN20IOAccelEventMachine24stopEv",
			 vfEventMachineStop, this->oVfEventMachineStop},
		};
		PANIC_COND(!patcher.routeMultiple(index, callbackOwnerTeardown,
		                                      address, size),
		           "ngreen", "Cannot route VF callback-owner teardown boundary");
		mach_vm_address_t poolInitStart = 0, growthStart = 0, growthEnd = 0;
		mach_vm_address_t getterStart = 0, getterEnd = 0;
		KernelPatcher::SolveRequest commandPoolBounds[] = {
			{"__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj",
			 poolInitStart},
			{"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv", growthStart},
			{"__ZN25IOAccelCommandBufferPool24freeEv", growthEnd},
			{"__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj", getterStart},
			{"__ZN25IOAccelCommandBufferPool212submitBufferEv", getterEnd},
		};
		PANIC_COND(!patcher.solveMultiple(index, commandPoolBounds, address, size) ||
		           growthStart <= poolInitStart ||
		           growthStart - poolInitStart != 0x182 ||
		           growthEnd <= growthStart ||
		           !NGIOAccelCommandPool::hasReviewedGrowthContract(
		               reinterpret_cast<const uint8_t *>(growthStart),
		               growthEnd - growthStart) ||
		           getterEnd <= getterStart ||
		           !NGIOAccelCommandPool::hasReviewedGetBufferContract(
		               reinterpret_cast<const uint8_t *>(getterStart),
		               getterEnd - getterStart),
		           "ngreen", "Changed IOAccelerator command-pool contract");
		KernelPatcher::RouteRequest commandPoolRoutes[] = {
			{"__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj",
			 ngVfCommandPoolInitBridge, gIOAccelCommandPoolInit},
			{"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv",
			 vfAllocMoreCommandBuffers, this->oIOAccelAllocMoreCommandBuffers},
			{"__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj",
			 vfGetCommandBufferPtrNoInc, this->oIOAccelGetCommandBufferPtrNoInc},
		};
		PANIC_COND(!patcher.routeMultiple(index, commandPoolRoutes, address, size),
		           "ngreen", "Cannot route VF command-pool postconditions");
		SYSLOG("ngreen", "V272: guarded VF command-pool growth and returned capacity");
		// These are the unique outer roots of every admitted inherited
		// resource producer.  Wrappers take one invocation lease before native
		// mutexes or waits; lower prepare/load/page/submit helpers deliberately
		// remain unhooked because final retirement must still use them after the
		// gate closes.  Every nonmatching/PF receiver is an exact pass-through.
		KernelPatcher::RouteRequest externalProducerRoutes[] = {
			{"__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs",
			 vfCommandQueueSubmit, this->oVfCommandQueueSubmit},
			{"__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy",
			 vfContextSubmit, this->oVfContextSubmit},
			{"__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits",
			 vf2DSetSurface, this->oVf2DSetSurface},
			{"__ZN17IOAccel2DContext26finishEj",
			 vf2DFinish, this->oVf2DFinish},
			{"__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy",
			 vf2DBlit, this->oVf2DBlit},
			{"__ZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfSurfaceExternalMethod, this->oVfSurfaceExternalMethod},
			{"__ZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfSharedExternalMethod, this->oVfSharedExternalMethod},
			{"__ZN17IOAccelGLContext214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfGLContextExternalMethod, this->oVfGLContextExternalMethod},
			{"__ZN27IOAccelGLDrawableUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfGLDrawableExternalMethod, this->oVfGLDrawableExternalMethod},
			{"__ZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfSurfaceMtlExternalMethod, this->oVfSurfaceMtlExternalMethod},
			{"__ZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfMemoryInfoExternalMethod, this->oVfMemoryInfoExternalMethod},
			{"__ZN29IOAccelDisplayPipeUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
			 vfDisplayPipeExternalMethod, this->oVfDisplayPipeExternalMethod},
			{"__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_",
			 vfDisplayChangeHandler, this->oVfDisplayChangeHandler},
			{"__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei",
			 vfGartCollector, this->oVfGartCollector},
			{"__ZN22IOGraphicsAccelerator218finalize_interruptEP22IOInterruptEventSourcei",
			 vfFinalizeInterrupt, this->oVfFinalizeInterrupt},
			{"__ZN22IOGraphicsAccelerator218deviceCacheControlEP20IOSurfaceDeviceCachejyy",
			 vfDeviceCacheControl, this->oVfDeviceCacheControl},
			{"__ZN22IOGraphicsAccelerator220emitFirstFlushEventsEv",
			 vfEmitFirstFlushEvents, this->oVfEmitFirstFlushEvents},
		};
		PANIC_COND(!patcher.routeMultiple(
		               index, externalProducerRoutes, address, size),
		           "ngreen", "Cannot route VF external-producer admission roots");
		SYSLOG("ngreen", "V317: routed 16 counted VF external-producer roots plus one lifecycle finalizer");
		gIoAccelLifecycleResolved = true;
		publishAcceleratorWhenReady();
		return true;
	}

	const bool physicalFramebuffer = index == kextG11FBT.loadIndex ||
		index == kextG11FBTA.loadIndex;
	const auto *image = reinterpret_cast<const uint8_t *>(address);
	const bool framebufferProduction = physicalFramebuffer &&
		NGBinaryIdentity::matchesKextUuid(
			image, size, NGBinaryIdentity::tglFramebufferProductionUuid);
	const bool framebufferDebug = physicalFramebuffer &&
		NGBinaryIdentity::matchesKextUuid(
			image, size, NGBinaryIdentity::tglFramebufferDebugUuid);
	PANIC_COND(physicalFramebuffer && !framebufferProduction && !framebufferDebug,
		"ngreen", "Unsupported TGL framebuffer payload ABI; refusing private-layout routes");
	if (physicalFramebuffer &&
	    (!NGreen::callback || !NGreen::callback->isRealTGL)) {
		// A VF has no physical display controller, and a later-generation PF is
		// not permission to run a UUID-pinned TGL display ABI against different
		// registers. Refusing only DMC or MMIO
		// helpers is too late: native probe/start have their own raw accesses. The
		// production payload overrides probe in AppleIntelFramebufferController,
		// while the debug payload inherits its AppleIntelBaseController override;
		// install the UUID-specific entry instead of requiring a symbol absent from
		// the production binary. This is admission rejection, not a virtual
		// framebuffer implementation.
		if (framebufferProduction) {
			KernelPatcher::RouteRequest reject[] = {
				{"__ZN31AppleIntelFramebufferController5probeEP9IOServicePi", vfRejectPhysicalFramebufferProbe},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService", vfRejectPhysicalFramebufferStart},
			};
			PANIC_COND(!patcher.routeMultiple(index, reject, address, size),
				"ngreen", "Cannot contain production TGL framebuffer on unsupported GPU");
		} else {
			KernelPatcher::RouteRequest reject[] = {
				{"__ZN24AppleIntelBaseController5probeEP9IOServicePi", vfRejectPhysicalFramebufferProbe},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService", vfRejectPhysicalFramebufferStart},
			};
			PANIC_COND(!patcher.routeMultiple(index, reject, address, size),
				"ngreen", "Cannot contain debug TGL framebuffer on unsupported GPU");
		}
		SYSLOG("ngreen", "TGL framebuffer probe/start rejected for VF/non-TGL/unknown device");
		return true;
	}
	if (kextG11FBT.loadIndex == index || kextG11FBTA.loadIndex == index) {
		auto *activeKext = (kextG11FBTA.loadIndex == index) ? &kextG11FBTA : &kextG11FBT;
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL framebuffer BAR0");
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		// Both admitted variants checked a 64-bit read against length-4.
		// Require the full eight-byte operand before dereferencing it.
		mach_vm_address_t read64Start = 0, read64End = 0;
		KernelPatcher::SolveRequest read64SymbolBounds[] = {
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister64Em",
			 read64Start},
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister64EPVvm",
			 read64End},
		};
		PANIC_COND(!patcher.solveMultiple(
		               index, read64SymbolBounds, address, size) ||
		           read64End <= read64Start || read64End - read64Start > 0x100,
		           "ngreen", "Invalid ReadRegister64 patch bounds");
		LookupPatchPlus const read64Bounds = framebufferProduction ?
			LookupPatchPlus {activeKext, NGFramebufferPatch::productionFind,
			 NGFramebufferPatch::productionReplace, 1} :
			LookupPatchPlus {activeKext, NGFramebufferPatch::debugFind,
			 NGFramebufferPatch::debugReplace, 1};
		PANIC_COND(!read64Bounds.apply(
		               patcher, read64Start, read64End - read64Start), "ngreen",
			"Failed to apply UUID-pinned ReadRegister64 bounds patch");

		return true;
		
	} else if (kextG11HWT.loadIndex == index || kextG11HWTA.loadIndex == index) {
		if (!OSCompareAndSwap(0, 1, &gTglProcessed))
			return true;
		// Every route below depends on private Tahoe object offsets, symbol ABIs or
		// exact instruction sequences. PF ownership does not make an unknown
		// payload layout safe, so use the same fail-closed identity gate for both
		// physical and virtual devices.
		PANIC_COND(!NGBinaryIdentity::matchesKextUuid(
			reinterpret_cast<const uint8_t *>(address), size,
			NGBinaryIdentity::tglVfPayloadUuid), "ngreen",
			"Unsupported TGL accelerator payload ABI; refusing private-layout routes");
		auto *activeKext = (kextG11HWTA.loadIndex == index) ? &kextG11HWTA : &kextG11HWT;
		SYSLOG("ngreen", "init AppleIntelTGLGraphics (HW accelerator)");
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL accelerator BAR0");
		const auto identity = vfIdentifyDevice();
		PANIC_COND(identity == VfIdentity::Invalid, "ngreen",
			"Cannot classify TGL accelerator as a physical function or VF");
		const bool vfActive = identity == VfIdentity::Virtual;
		const uint32_t gpuDevice = NGreen::callback->getOriginalDeviceId();
		gVfUsesMemoryIrq = vfActive &&
			NGGpuCapabilities::hasIovMemoryIrq(gpuDevice);
		const bool tglGeneration = NGGpuCapabilities::isTigerLake(gpuDevice);
		PANIC_COND(!NGGpuCapabilities::supportsPinnedTigerLakePayload(
			gpuDevice, vfActive), "ngreen",
			"Refusing TGL native accelerator payload on a non-TGL physical function");
		if (vfActive) {
			SYSLOG("ngreen", "V251: VF interrupt transport=%s device=%04x",
			       gVfUsesMemoryIrq ? "memory" : "Gen11 virtual MMIO",
			       gpuDevice);
		}
		// The payload is patched before its personality is published. Complete the
		// one-shot VF bootstrap here so every embedded fuse immediate comes from
		// the same PF snapshot that owns this VF's GuC/GGTT assignment.
		PANIC_COND(vfActive && !vfBootstrapDirectGgtt(), "ngreen",
			"Cannot bootstrap VF runtime/GGTT before accelerator publication");
		SYSLOG("ngreen", "V165: setRMMIO done, starting symbol resolve");

		if (vfActive) {
			// Tahoe's telemetry manager and per-stamp usage objects are created
			// before startGraphicsEngine(). TelemetryDisable only gates the trace
			// stream and context-image patchers; native manager initialization,
			// IOReport, sysctl and user-client OA paths still access PF-owned MMIO.
			// Keep Apple's object/lifetime shape, but make every externally reachable
			// hardware telemetry edge fail closed for a VF.
			KernelPatcher::RouteRequest telemetryRoutes[] = {
				{"__ZN18IGTelemetryManager14printDashboardEy",
				 vfTelemetryPrintDashboard},
				{"__ZN18IGTelemetryManager19initWithAcceleratorEP16IntelAcceleratorj",
				 vfTelemetryInitWithAccelerator},
				{"__ZN18IGTelemetryManager15telemetryRetainEv",
				 vfTelemetryRetain},
				{"__ZN18IGTelemetryManager16telemetryReleaseEv",
				 vfTelemetryRelease},
				{"__ZN18IGTelemetryManager15calcGlobalUsageEy",
				 vfTelemetryCalcGlobalUsage},
				{"__ZN18IGTelemetryManager9operationEyxP18TelemetryOperationP19TelemetryConnectionP4task",
				 vfTelemetryOperation},
				{"__ZN18IGTelemetryManager20patchContextImageRCSEP27SGfxHardwareContextImageRCS",
				 vfTelemetryPatchContextImage},
				{"__ZN18IGTelemetryManager16onConnectionStopER19TelemetryConnectionP4task",
				 vfTelemetryOnConnectionStop},
				{"__ZN18IGTelemetryManager21telemetryInitOaBufferER19TelemetryConnectionP19MDAPIInitOABufferOpS3_yPy",
				 vfTelemetryInitOaBuffer},
				{"__ZN18IGTelemetryManager21telemetryReadOaBufferEP21MDAPIReadOABufferOpInP22MDAPIReadOABufferOpOutPyR19TelemetryConnection",
				 vfTelemetryReadOaBuffer},
				{"__ZN18IGTelemetryManager26telemetryMapOaBufferMemoryEP27IntelDeviceMapStatsMemInOutS1_yPyP4task",
				 vfTelemetryMapOaBufferMemory},
				{"__ZN16IGTelemetryUsage13allocUsageMemEv",
				 vfTelemetryUsageAlloc},
				{"__ZN16IGTelemetryUsage8logUsageEP17IGHardwareContexty",
				 vfTelemetryUsageLog},
				{"__ZN16IGTelemetryUsage17reportGlobalUsageEv",
				 vfTelemetryUsageReportGlobal},
				{"__ZN16IGTelemetryUsage11startSampleEP20IGHardwareRingBufferjjyyyy",
				 vfTelemetryUsageStartSample},
				{"__ZN16IGTelemetryUsage10stopSampleEP20IGHardwareRingBufferjj",
				 vfTelemetryUsageStopSample},
				{"__ZN16IGTelemetryUsage16frameCalcGPUBusyEP16IntelAcceleratorj",
				 vfTelemetryUsageFrameCalc},
				// TelemetryDisable gates trace bodies, but native initSysctl has
				// already registered 55 writable debug OIDs and published this
				// accelerator to the global idvar control plane before checking it.
				// Suppress registration and its paired teardown together on a VF.
				{"__ZN16IntelAccelerator10initSysctlEv",
				 vfDisableDebugSysctl},
				{"__ZN16IntelAccelerator16unregisterSysctlEv",
				 vfDisableDebugSysctl},
			};
			PANIC_COND(!patcher.routeMultiple(
			               index, telemetryRoutes, address, size), "ngreen",
			           "Failed to isolate VF telemetry and OA hardware paths");
			SYSLOG("ngreen", "V260: disabled PF-owned telemetry/OA and debug-sysctl paths for VF");
			KernelPatcher::SolveRequest solveRequests[] = {
				{"__ZN13IGHardwareGuC16initSchedControlEv", this->orgInitSchedControl},
				{"__ZN11IGScheduler12initFirmwareEv", this->vfSchedulerInitFirmware},
				{"__ZN21IGHardwareGuCCTBuffer32handleSoftwareGuCToHostInterruptEv",
				 this->vfCtbSoftwareInterrupt},
				// V233: single-LRC GuC submission must publish the new tail in
				// the context image before scheduling it.  Resolve the accessor
				// used by IGHardwareContext::updateRingTail so the bridge can do
				// the same update without touching physical memory.
				{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv",
				 this->vfSharedMappedBufferGetVirtualAddress},
				// Resolve the callback dispatcher used by the MTL/ARL memory-IRQ
				// replacement. TGL/ADL/RPL retain the native virtual-MMIO filter.
				{"__ZN17IGInterruptBridge17serviceInterruptsERK8IGBitSetILm46EE",
				 this->vfServiceInterrupts},
				// V216: The TGL driver assigns the old value returned by
				// OSAddAtomic64 to IGAccelTask+0x258.  Failed accelerator start
				// candidates consume value 0 without restoring this global, so a
				// later real kernel task is misclassified as a user task throughout
				// initWithOptions.  Resolve the counter so it can be repaired before
				// the next bootstrap allocation starts.
				{"__ZN11IGAccelTask12fTaskCounterE", this->igAccelTaskCounter},
			};
			PANIC_COND(!patcher.solveMultiple(index, solveRequests, address, size), "ngreen",
			           "Failed to resolve mandatory TGL accelerator bootstrap symbols");
		}

		// V229: apply this byte patch before routing readDoorbellSQIDIConfig.
		// routeFunction overwrites the function entry and therefore overlaps the
		// exact sequence below (which starts at entry + 4).  Patching afterwards
		// made V228 panic before the driver could start.  Calls emitted as local
		// rel32 targets now remain safe even when they bypass the routed entry.
		// 0x001f00ff encodes SQIDI mask 0xff and 32 doorbells per SQIDI.
		if (vfActive) {
			KernelPatcher::SolveRequest bufferAccessors[] = {
				{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv",
				 this->vfSharedMappedBufferGetVirtualAddress},
				{"__ZNK14IGMappedBuffer20getGPUVirtualAddressEv",
				 this->vfMappedBufferGetGPUVirtualAddress},
				{"__ZNK14IGMappedBuffer9getMemoryEv",
				 this->oIGMappedBuffergetMemory},
				// getMemory() returns the private IGAccelMemory owner, not an
				// IOMemoryDescriptor.  The exact system-memory subclass method
				// applies the same IOMapper options as Tahoe's GGTT commit path.
				{"__ZN16IGAccelSysMemory18getPhysicalSegmentEyPy",
				 this->vfAccelSysMemoryGetPhysicalSegment},
				{"__ZN13IGHardwareGuC12allocContextEyb",
				 this->vfAllocContext},
				{"__ZN13IGHardwareGuC14releaseContextEj",
				 this->vfReleaseContext},
				{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj",
				 this->vfSharedMappedBufferWithOptions},
				{"__ZN22IGHardwareGuCWorkQueue11withOptionsEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 this->vfWorkQueueWithOptions},
			};
			PANIC_COND(!patcher.solveMultiple(index, bufferAccessors, address, size),
			           "ngreen", "Cannot resolve VF buffer accessors or proxy-context lifecycle");
			this->vfOSObjectFree = patcher.solveSymbol(KernelPatcher::KernelID,
				"__ZN8OSObject4freeEv");
			PANIC_COND(!this->vfOSObjectFree, "ngreen",
			           "Cannot resolve base destructor for failed VF workqueues");

			// Pool init returns bool, but this UUID-pinned constructor discarded it
			// before backing allocation and base setup. The routed System-KC bridge
			// clears cleanup-sensitive record state, preserves AL in ZF and restores
			// the task in RDI; this same-size patch branches false to the constructor's
			// existing epilogue without a code cave.
			mach_vm_address_t extendedInit = 0, extendedFree = 0;
			KernelPatcher::SolveRequest extendedBounds[] = {
				{"__ZN25IGHardwareExtendedContext15initWithOptionsEP11IGAccelTaskRK31IGHardwareExtendedContextParams",
				 extendedInit},
				{"__ZN25IGHardwareExtendedContext4freeEv", extendedFree},
			};
			PANIC_COND(!patcher.solveMultiple(index, extendedBounds, address, size) ||
			           extendedFree <= extendedInit ||
			           !NGIOAccelCommandPool::hasReviewedExtendedInitContract(
			               reinterpret_cast<const uint8_t *>(extendedInit),
			               extendedFree - extendedInit),
			           "ngreen", "Changed VF extended-context pool-init contract");
			LookupPatchPlus const extendedInitPatch {
				activeKext, NGIOAccelCommandPool::extendedInitFind,
				NGIOAccelCommandPool::extendedInitReplace, 1,
			};
			PANIC_COND(!extendedInitPatch.apply(
			               patcher, extendedInit, extendedFree - extendedInit),
			           "ngreen", "Failed to propagate VF command-pool init failure");
			SYSLOG("ngreen", "V269: guarded VF extended-context pool construction");

			// The rect-list request is 64-byte aligned, but native admitted exactly
			// 64 KiB while this pool reserves its final eight bytes. Bound only this
			// producer to the configured usable capacity; PF remains byte-identical.
			mach_vm_address_t rectListCapacityStart = 0, rectListCapacityEnd = 0;
			KernelPatcher::SolveRequest rectListCapacityBounds[] = {
				{"__ZL22blit3d_submit_rectlistP23IGHardwareBlit3DContextP15blit3d_params_tPK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyE",
				 rectListCapacityStart},
				{"__ZL19IsSurfaceCompressedj", rectListCapacityEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, rectListCapacityBounds, address, size) ||
			           rectListCapacityEnd <= rectListCapacityStart ||
			           !NGIOAccelCommandPool::hasReviewedRectListCapacity(
			               reinterpret_cast<const uint8_t *>(rectListCapacityStart),
			               rectListCapacityEnd - rectListCapacityStart),
			           "ngreen", "Changed VF rect-list command-pool capacity contract");
			LookupPatchPlus const rectListCapacityPatch {
				activeKext, NGIOAccelCommandPool::rectListCapacityFind,
				NGIOAccelCommandPool::rectListCapacityReplace, 1,
			};
			PANIC_COND(!rectListCapacityPatch.apply(
			               patcher, rectListCapacityStart,
			               rectListCapacityEnd - rectListCapacityStart),
			           "ngreen", "Failed to reserve the VF rect-list pool tail");
			SYSLOG("ngreen", "V270: bounded VF rect-list command requests to 0xfff8 bytes");

			// HIZ assembly uses a 4 KiB window relative to the pointer returned by
			// getBufferPtrNoInc(). Its native dynamic request always fit the current
			// tail and therefore failed to rotate the 0x9b8-byte initialization prefix.
			// Require one complete configured usable region so the native getter
			// submits a partial slot before any HIZ command is written.
			mach_vm_address_t resolveHizStart = 0, resolveHizEnd = 0;
			KernelPatcher::SolveRequest resolveHizBounds[] = {
				{"__Z14resolve_hiz_g7P25IOAccelCommandBufferPool2P14IGMappedBufferP22depth_resolve_params_tR15resolve_phase_tRjS7_yb",
				 resolveHizStart},
				{"__ZL25hiz_first_instr_optimizedP24g8_hiz_resolve_cmd_buf_tPK22depth_resolve_params_tbjjjjR22SGfx3dStateDepthBufferR26SGfx3dStateHierDepthBuffery",
				 resolveHizEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, resolveHizBounds, address, size) ||
			           resolveHizEnd <= resolveHizStart ||
			           !NGIOAccelCommandPool::hasReviewedResolveHizCapacity(
			               reinterpret_cast<const uint8_t *>(resolveHizStart),
			               resolveHizEnd - resolveHizStart),
			           "ngreen", "Changed VF resolve-HIZ command-pool capacity contract");
			LookupPatchPlus const resolveHizCapacityPatch {
				activeKext, NGIOAccelCommandPool::resolveHizCapacityFind,
				NGIOAccelCommandPool::resolveHizCapacityReplace, 1,
			};
			PANIC_COND(!resolveHizCapacityPatch.apply(
			               patcher, resolveHizStart, resolveHizEnd - resolveHizStart),
			           "ngreen", "Failed to require a fresh VF resolve-HIZ command slot");
			SYSLOG("ngreen", "V271: require full usable VF resolve-HIZ command capacity");

			// Tahoe's GuC factory releases the object after initWithOptions()
			// already called virtual free() on the same failure path. XNU's
			// OSObject::free() deletes the instance, so the second virtual dispatch
			// dereferences freed storage. Keep the native cleanup and null return,
			// but remove only that second dispatch in the UUID-pinned factory.
			mach_vm_address_t gucFactory = 0, gucInit = 0;
			KernelPatcher::SolveRequest gucFactoryBounds[] = {
				{"__ZN13IGHardwareGuC11withOptionsEP16IntelAccelerator",
				 gucFactory},
				{"__ZN13IGHardwareGuC15initWithOptionsEP16IntelAccelerator",
				 gucInit},
			};
			PANIC_COND(!patcher.solveMultiple(index, gucFactoryBounds, address, size) ||
			           gucInit <= gucFactory || gucInit - gucFactory > 0x100,
			           "ngreen", "Invalid VF GuC factory patch bounds");
			LookupPatchPlus const gucFactoryPatch {
				activeKext, NGVfGuCFactoryPatch::releaseAfterFailedInitFind,
				NGVfGuCFactoryPatch::releaseAfterFailedInitReplace, 1,
			};
			PANIC_COND(!gucFactoryPatch.apply(
			               patcher, gucFactory, gucInit - gucFactory), "ngreen",
			           "Failed to remove VF GuC factory double destruction");
			mach_vm_address_t schedulerInit = 0, schedulerWait = 0;
			// Pools here are newly initialized and have no allocated GPU pages.
			// Preserve native release/free cleanup, but visit only the created
			// prefix instead of walking forward past the failed factory slot.
			mach_vm_address_t poolInit = 0, managerFree = 0;
			KernelPatcher::SolveRequest poolBounds[] = {
				{"__ZN15IGMemoryManager12initPagePoolEv", poolInit},
				{"__ZN15IGMemoryManager4freeEv", managerFree},
			};
			PANIC_COND(!patcher.solveMultiple(index, poolBounds, address, size) ||
			           managerFree <= poolInit || managerFree - poolInit != 0xcc,
			           "ngreen", "Invalid VF page-pool init patch bounds");
			PANIC_COND(!NGVfPagePoolPatch::prefixUnwindPreflight(
			               reinterpret_cast<const uint8_t *>(poolInit), managerFree - poolInit),
			           "ngreen", "Changed or ambiguous VF page-pool unwind");
			LookupPatchPlus const poolUnwindPatch {
				activeKext, NGVfPagePoolPatch::prefixUnwindFind,
				NGVfPagePoolPatch::prefixUnwindReplace, 1,
			};
			PANIC_COND(!poolUnwindPatch.apply(patcher, poolInit, managerFree - poolInit),
			           "ngreen", "Failed to repair VF page-pool prefix unwind");
			KernelPatcher::SolveRequest schedulerBounds[] = {
				{"__ZN12IGScheduler419initWithAcceleratorEP22IOGraphicsAccelerator2", schedulerInit},
				{"__ZN12IGScheduler414waitForGpuIdleEv", schedulerWait},
			};
			PANIC_COND(!patcher.solveMultiple(index, schedulerBounds, address, size) ||
			           schedulerWait <= schedulerInit || schedulerWait - schedulerInit != 0xc2,
			           "ngreen", "Invalid VF scheduler failed-init patch bounds");
			PANIC_COND(!NGVfGuCFactoryPatch::schedulerInitPreflight(
			               reinterpret_cast<const uint8_t *>(schedulerInit), 0xaf),
			           "ngreen", "Ambiguous or changed VF scheduler failed-init patch site");
			LookupPatchPlus const schedulerInitPatch {
				activeKext, NGVfGuCFactoryPatch::schedulerInitFreeFind,
				NGVfGuCFactoryPatch::schedulerInitFreeReplace, 1,
			};
			PANIC_COND(!schedulerInitPatch.apply(patcher, schedulerInit, 0xaf),
			           "ngreen", "Failed to preserve single-owner VF scheduler init cleanup");
			// Metal's first Blit3D context fills scratch blend states through
			// offset 0xd20f, but a VF-only shared-buffer mapping stops at
			// 0xd000. Keep the native initializer and publish a page-rounded
			// allocation size instead; the private factory only reads the
			// recorded length back from this same global. Bound the rewrite by
			// stable exported neighbours enclosing the private global constructor
			// and require the exact UUID-pinned anchor, including its const-table
			// RIP target.
			mach_vm_address_t blit3dBoundsStart = 0, blit3dBoundsEnd = 0;
			KernelPatcher::SolveRequest blit3dScratchBounds[] = {
				{"__ZN23IGHardwareBlit2DContext10initializeEv", blit3dBoundsStart},
				{"__ZN21IGAccelDisplayMachine9MetaClassC1Ev", blit3dBoundsEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, blit3dScratchBounds, address, size) ||
			           blit3dBoundsEnd <= blit3dBoundsStart ||
			           blit3dBoundsEnd - blit3dBoundsStart > 0x400,
			           "ngreen", "Invalid Blit3D scratch patch bounds");
			LookupPatchPlus const blit3dScratchPatch {
				activeKext, NGVfBlit3dScratchPatch::scratchSizeFind,
				NGVfBlit3dScratchPatch::scratchSizeReplace, 1,
			};
			PANIC_COND(!blit3dScratchPatch.apply(
			               patcher, blit3dBoundsStart,
			               blit3dBoundsEnd - blit3dBoundsStart), "ngreen",
			           "Failed to enlarge Blit3D scratch allocation");
			// V371: a TGL load Lilu missed is processed after the kext's static
			// constructors already ran (V368), so the patched constructor never
			// executes and the published size is still 0xd240. Correct it.
			auto *blit3dParams = reinterpret_cast<volatile uint64_t *>(patcher.solveSymbol(
				index, "__ZN23IGHardwareBlit3DContext17ExtendedCtxParamsE", address, size));
			PANIC_COND(!blit3dParams, "ngreen", "Missing Blit3D context parameters");
			if (blit3dParams[2] == 0xd240) {
				blit3dParams[2] = 0xe000;
				SYSLOG("ngreen", "V371: Blit3D constructor already ran; scratch size set to 0xe000");
			}
			SYSLOG("ngreen", "V250: enlarged Blit3D scratch allocation 0xd240→0xe000");
			mach_vm_address_t ccsStart = 0, ccsEnd = 0;
			KernelPatcher::SolveRequest ccsBounds[] = {
				{"__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh", ccsStart},
				{"__ZN15IGAccelResource24disableRenderCompressionEPNS_17ResourceInfoEntryE", ccsEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, ccsBounds, address, size) ||
			           ccsEnd <= ccsStart || ccsEnd - ccsStart != 0x554,
			           "ngreen", "Invalid CCS allocation repair bounds");
			PANIC_COND(!NGVfBlit3dScratchPatch::ccsAllocationPreflight(
			               reinterpret_cast<const uint8_t *>(ccsStart), ccsEnd - ccsStart),
			           "ngreen", "Changed or ambiguous CCS allocation repair anchors");
			LookupPatchPlus const ccsAllocationPatch {
				activeKext, NGVfBlit3dScratchPatch::ccsAllocationFind,
				NGVfBlit3dScratchPatch::ccsAllocationReplace, 1,
			};
			PANIC_COND(!ccsAllocationPatch.apply(patcher, ccsStart, ccsEnd - ccsStart),
			           "ngreen", "Failed to preserve CCS allocation-failure cleanup");

			// All eight event-pointer vector grow copies return false when no growth
			// was needed as well as on allocation failure. Lilu cannot range-select
			// duplicate symbols, so derive each exact target from unique owner anchors
			// in the admitted payload and validate every body before the first route.
			mach_vm_address_t twoDEventOwnerStart = 0, twoDEventOwnerEnd = 0;
			mach_vm_address_t acceleratorEventOwnerStart = 0, acceleratorEventOwnerEnd = 0;
			mach_vm_address_t renderEventOwnerStart = 0, renderEventOwnerEnd = 0;
			mach_vm_address_t blitEventOwnerStart = 0, blitEventOwnerEnd = 0;
			mach_vm_address_t glEventOwnerStart = 0, glEventOwnerEnd = 0;
			mach_vm_address_t resourceEventOwnerStart = 0, resourceEventOwnerEnd = 0;
			mach_vm_address_t sharedEventOwnerStart = 0, sharedEventOwnerEnd = 0;
			mach_vm_address_t surfaceEventOwnerStart = 0, surfaceEventOwnerEnd = 0;
			KernelPatcher::SolveRequest eventOwnerBounds[] = {
				{"__ZN16IGAccel2DContext8blitCopyEP12IOAccelEventP16IOAccelResource2S3_P22IOAccel2DBlitRectStrucj", twoDEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccel2DContext.cpp", twoDEventOwnerEnd},
				{"__ZN19IGBlockFenceManagerD0Ev", acceleratorEventOwnerStart},
				{"__ZL18kDisplayVar_sysctlP10sysctl_oidPviP10sysctl_req", acceleratorEventOwnerEnd},
				{"__Z25process_MSAABufferResolveR26IGAccelSegmentResourceListR24IGAccelCommandDescriptorR22IOGraphicsAccelerator2RKN14IntelMTLRender20sResolveResourceDescES8_NS5_14eResolveFilterE10BufferTypeR12IOAccelEvent", renderEventOwnerStart},
				{"__Z40surfaceStateFillMemoryObjectControlStateR22SGfxRenderSurfaceStateP15IGAccelResourceP15IGMemoryManager", renderEventOwnerEnd},
				{"__ZN21IntelMTLBlitFunctions7executeEN12IntelMTLBlit7eTokensER19IGAccelCommandQueueR26IGAccelSegmentResourceListRK20IOAccelKernelCommandR24IGAccelCommandDescriptorR13IGHeapsAccessR22IOGraphicsAccelerator2R17IGHardwareContextR12IOAccelEvent", blitEventOwnerStart},
				{"__ZN14IGTelemetryKMD4initEP16IntelAcceleratory", blitEventOwnerEnd},
				{"__ZN16IGAccelGLContext32process_token_ResolveDepthBufferER24IOAccelCommandStreamInfo", glEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelGLContext.cpp", glEventOwnerEnd},
				{"__ZN15IGAccelResource22updateMappingCacheTypeEj", resourceEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelResource.cpp", resourceEventOwnerEnd},
				{"__ZN23IGAccelSharedUserClient9MetaClassD0Ev", sharedEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelSharedUserClient.cpp", sharedEventOwnerEnd},
				{"__ZN14IGAccelSurface9MetaClassD0Ev", surfaceEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelSurface.cpp", surfaceEventOwnerEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, eventOwnerBounds, address, size),
			           "ngreen", "Invalid VF event-vector owner bounds");
			const mach_vm_address_t twoDEventGrow = NGEventVector::locateReviewedGrow(
				twoDEventOwnerStart, twoDEventOwnerEnd, 0xA74);
			const mach_vm_address_t acceleratorEventGrow = NGEventVector::locateReviewedGrow(
				acceleratorEventOwnerStart, acceleratorEventOwnerEnd, 0x244);
			const mach_vm_address_t renderEventGrow = NGEventVector::locateReviewedGrow(
				renderEventOwnerStart, renderEventOwnerEnd, 0x764);
			const mach_vm_address_t blitEventGrow = NGEventVector::locateReviewedGrow(
				blitEventOwnerStart, blitEventOwnerEnd, 0x16C8);
			const mach_vm_address_t glEventGrow = NGEventVector::locateReviewedGrow(
				glEventOwnerStart, glEventOwnerEnd, 0x4AC8);
			const mach_vm_address_t resourceEventGrow = NGEventVector::locateReviewedGrow(
				resourceEventOwnerStart, resourceEventOwnerEnd, 0x60C);
			const mach_vm_address_t sharedEventGrow = NGEventVector::locateReviewedGrow(
				sharedEventOwnerStart, sharedEventOwnerEnd, 0xA);
			const mach_vm_address_t surfaceEventGrow = NGEventVector::locateReviewedGrow(
				surfaceEventOwnerStart, surfaceEventOwnerEnd, 0xA);
			PANIC_COND(!twoDEventGrow || !acceleratorEventGrow || !renderEventGrow ||
			           !blitEventGrow || !glEventGrow || !resourceEventGrow ||
			           !sharedEventGrow || !surfaceEventGrow,
			           "ngreen", "Changed VF event-vector growth contracts");
			ExactRouteRequest eventGrowRoutes[] = {
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 twoDEventGrow, vf2DEventVectorGrow, this->oVf2DEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 acceleratorEventGrow, vfAcceleratorEventVectorGrow,
				 this->oVfAcceleratorEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 renderEventGrow, vfRenderEventVectorGrow, this->oVfRenderEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 blitEventGrow, vfBlitEventVectorGrow, this->oVfBlitEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 glEventGrow, vfGLEventVectorGrow, this->oVfGLEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 resourceEventGrow, vfResourceEventVectorGrow,
				 this->oVfResourceEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 sharedEventGrow, vfSharedEventVectorGrow, this->oVfSharedEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 surfaceEventGrow, vfSurfaceEventVectorGrow, this->oVfSurfaceEventVectorGrow},
			};
			PANIC_COND(!routeExactMultiple(patcher, eventGrowRoutes),
			           "ngreen", "Failed to guard VF event-vector growth");
			SYSLOG("ngreen", "V274: guarded all eight VF event-vector growth copies");
			KernelPatcher::RouteRequest workQueueInitRoute[] = {
				{"__ZN22IGHardwareGuCWorkQueue19initWithAcceleratorEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 vfWorkQueueInit, this->oVfWorkQueueInit},
				{"__ZN22IGHardwareGuCWorkQueue4freeEv",
				 vfWorkQueueFree, this->oVfWorkQueueFree},
				{"__ZN21IGHardwareGuCCTBuffer4freeEv",
				 vfCtbFree, this->oVfCtbFree},
			};
			PANIC_COND(!patcher.routeMultiple(index, workQueueInitRoute, address, size),
			           "ngreen", "Failed to install VF workqueue allocation unwind");
			// Preserve Apple's complete event-source and callback lifecycle.  Its
			// Gen11 virtual-MMIO path is the native TGL/ADL/RPL VF protocol; only
			// MTL/ARL memory-IRQ VFs must suppress the surrounding master accesses.
			mach_vm_address_t irqEnable = 0, irqDisable = 0;
			KernelPatcher::SolveRequest irqLifecycle[] = {
				{"__ZN17IGInterruptBridge6enableEv", irqEnable},
				{"__ZN17IGInterruptBridge7disableEv", irqDisable},
			};
			PANIC_COND(!patcher.solveMultiple(index, irqLifecycle, address, size),
			           "ngreen", "Cannot resolve VF IRQ lifecycle");
			this->vfInterruptBridgeEnable = irqEnable;
			this->vfInterruptBridgeDisable = irqDisable;
			if (gVfUsesMemoryIrq) {
				mach_vm_address_t irqEnableRegs = 0, irqDisableRegs = 0;
				KernelPatcher::SolveRequest irqBounds[] = {
					{"__ZN17IGInterruptBridge16enableInterruptsEv", irqEnableRegs},
					{"__ZN17IGInterruptBridge17disableInterruptsEv", irqDisableRegs},
				};
				PANIC_COND(!patcher.solveMultiple(index, irqBounds, address, size) ||
				           irqEnableRegs <= irqEnable || irqDisableRegs <= irqDisable ||
				           irqEnableRegs - irqEnable > 0x400 ||
				           irqDisableRegs - irqDisable > 0x400,
				           "ngreen", "Invalid memory-IRQ lifecycle patch bounds");
				static const uint8_t masterDisable[] = {
					0x81, 0xa0, 0x10, 0x00, 0x19, 0x00, 0xff, 0xff, 0xff, 0x7f};
				static const uint8_t masterEnable[] = {
					0x81, 0x88, 0x10, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x80};
				static const uint8_t noMasterAccess[] = {
					0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
				LookupPatchPlus const masterPatches[] = {
					{activeKext, masterDisable, noMasterAccess, 1},
					{activeKext, masterEnable, noMasterAccess, 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(
				               patcher, masterPatches, irqEnable,
				               irqEnableRegs - irqEnable) ||
				           !LookupPatchPlus::applyAll(
				               patcher, masterPatches, irqDisable,
				               irqDisableRegs - irqDisable),
				           "ngreen", "Failed to isolate memory-IRQ lifecycle from GFX_MSTR_IRQ");
			}
			// These entry points can call framebuffer force-wake instead of the
			// multithreaded accelerator route. A VF has no guest-owned domains.
			KernelPatcher::RouteRequest vfWakeRoutes[] = {
				{"__ZN16IntelAccelerator13SafeForceWakeEbj", wrapSafeForceWake},
				{"__ZN16IntelAccelerator22SafeForceWakeInterruptEbj", wrapSafeForceWake},
				{"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake},
			};
			PANIC_COND(!patcher.routeMultiple(index, vfWakeRoutes, address, size),
			           "ngreen", "Failed to isolate VF force-wake entry points");

			// The VF has no physical framebuffer by design. Keep Apple's complete
			// standalone fallback, but make its waitForMatchingService lookup
			// nonblocking instead of stalling accelerator start for 30 seconds.
			mach_vm_address_t fbRegistration = 0, fbRegistrationEnd = 0;
			KernelPatcher::SolveRequest fbRegistrationBounds[] = {
				{"__ZN16IntelAccelerator33registerWithFramebufferControllerEv",
				 fbRegistration},
				{"__ZN16IntelAccelerator23initHardwareWorkaroundsEv",
				 fbRegistrationEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, fbRegistrationBounds, address, size) ||
			           fbRegistrationEnd <= fbRegistration ||
			           fbRegistrationEnd - fbRegistration > 0x300,
			           "ngreen", "Invalid VF framebuffer-registration patch bounds");
			LookupPatchPlus const vfFramebufferWait {
				activeKext, NGVfStandalonePatch::waitFind,
				NGVfStandalonePatch::waitReplace, 1,
			};
			PANIC_COND(!vfFramebufferWait.apply(
			               patcher, fbRegistration,
			               fbRegistrationEnd - fbRegistration), "ngreen",
			           "Failed to remove VF physical-framebuffer wait");
			static const uint8_t vfDoorbellTopologyFind[] = {
				0x48, 0x8b, 0x47, 0x38,
				0x48, 0x8b, 0x80, 0x40, 0x12, 0x00, 0x00,
				0x8b, 0x80, 0x08, 0x0d, 0x00, 0x00,
				0xc6, 0x87, 0xe2, 0x09, 0x00, 0x00, 0x00,
			};
			static const uint8_t vfDoorbellTopologyReplace[] = {
				0x48, 0x8b, 0x47, 0x38,
				0x48, 0x8b, 0x80, 0x40, 0x12, 0x00, 0x00,
				0xb8, 0xff, 0x00, 0x1f, 0x00, 0x90,
				0xc6, 0x87, 0xe2, 0x09, 0x00, 0x00, 0x00,
			};
			mach_vm_address_t doorbellReadStart = 0, doorbellReadEnd = 0;
			KernelPatcher::SolveRequest doorbellReadBounds[] = {
				{"__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv",
				 doorbellReadStart},
				{"__ZN13IGHardwareGuC15acquireDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_RECb",
				 doorbellReadEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, doorbellReadBounds, address, size) ||
			           doorbellReadEnd <= doorbellReadStart ||
			           doorbellReadEnd - doorbellReadStart > 0x100,
			           "ngreen", "Invalid VF DISTRDB patch bounds");
			LookupPatchPlus const vfDoorbellTopologyPatch {
				activeKext, vfDoorbellTopologyFind, vfDoorbellTopologyReplace, 1,
			};
			PANIC_COND(!vfDoorbellTopologyPatch.apply(
			               patcher, doorbellReadStart,
			               doorbellReadEnd - doorbellReadStart),
			           "ngreen", "Failed to replace VF DISTRDB read");
			SYSLOG("ngreen", "V229: replaced physical DISTRDB read before GuC routing");
		}

		if (vfActive) {
			KernelPatcher::SolveRequest pageTableRollback[] = {
				{"__ZNK11IGAccelTask29getHardwareContextAddressModeEv",
				 this->vfGetHardwareContextAddressMode},
				{"__ZN31IGHardwarePerProcessPageTable3211withOptionsEP16IntelAcceleratorP11IGAccelTask",
				 this->vfPpgtt32WithOptions},
				{"__ZN31IGHardwarePerProcessPageTable6411withOptionsEP16IntelAcceleratorP11IGAccelTask",
				 this->vfPpgtt64WithOptions},
				{"__ZN16IntelAccelerator27flushHardwareAfterGttUpdateEv",
				 this->vfFlushHardwareAfterGttUpdate},
				{"__ZN10IGPagePool14PageDescriptor6retainEv",
				 this->vfPageDescriptorRetain},
				{"__ZN10IGPagePool14PageDescriptor7releaseEv",
				 this->vfPageDescriptorRelease},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, pageTableRollback, address, size), "ngreen",
			           "Cannot resolve VF page-table rollback boundary");

			// Tahoe's base IGMemoryManager::init reaches the TGL eDRAM detector
			// through vtable slot 0x148 after the global GGTT is live.  The V320
			// runtime boundary proved that this concrete dispatch can remain live
			// even though the public detector symbol is routed below.  Remove the
			// one void dispatch at its owner while retaining the symbol route as a
			// second boundary for any independent virtual/external caller.  Base
			// init has already zeroed both eDRAM capability bytes at this point.
			mach_vm_address_t memoryManagerInit = 0;
			mach_vm_address_t memoryManagerInitEnd = 0;
			KernelPatcher::SolveRequest memoryManagerInitBounds[] = {
				{"__ZN15IGMemoryManager4initEP16IntelAcceleratorRK18IntelSharedMemInfoRK14_stolenMemInfo",
				 memoryManagerInit},
				{"__ZN15IGMemoryManager16initDeviceMemoryEv",
				 memoryManagerInitEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, memoryManagerInitBounds, address, size) ||
			           memoryManagerInitEnd - memoryManagerInit != 0x232,
			           "ngreen", "Changed VF memory-manager init boundary");
			static const uint8_t edramDispatchFind[] = {
				0x49, 0x8b, 0x06, 0x4c, 0x89, 0xf7,
				0xff, 0x90, 0x48, 0x01, 0x00, 0x00,
				0x49, 0x8b, 0x7e, 0x10,
			};
			static const uint8_t edramDispatchReplace[] = {
				0x49, 0x8b, 0x06, 0x4c, 0x89, 0xf7,
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
				0x49, 0x8b, 0x7e, 0x10,
			};
			LookupPatchPlus const edramDispatchPatch {
				activeKext, edramDispatchFind, edramDispatchReplace, 1,
			};
			PANIC_COND(!edramDispatchPatch.apply(
			               patcher, memoryManagerInit,
			               memoryManagerInitEnd - memoryManagerInit), "ngreen",
			           "Failed to close the VF eDRAM dispatch edge");
			SYSLOG("ngreen", "V321: bypassed physical TGL eDRAM dispatch at memory-manager init");

			// Tahoe's native IntelAccelerator::start publishes the service
			// synchronously after engine/CTB setup but before it returns to this
			// driver's outer lifecycle wrapper.  That lets matching clients enter
			// resource creation before V301 has verified the DPSM, periodic and
			// fallback callback owners.  The wrapper already performs the one
			// intended asynchronous publication after those checks.  Remove only
			// this UUID-pinned inner virtual call for a classified VF; PF start and
			// the wrapper's admitted publication remain native.
			mach_vm_address_t acceleratorStart = 0;
			mach_vm_address_t acceleratorStartEnd = 0;
			KernelPatcher::SolveRequest acceleratorStartBounds[] = {
				{"__ZN16IntelAccelerator5startEP9IOService", acceleratorStart},
				{"__ZN16IntelAccelerator10initIdVarsEv", acceleratorStartEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, acceleratorStartBounds, address, size) ||
			           acceleratorStartEnd - acceleratorStart != 0x924,
			           "ngreen", "Changed VF accelerator start publication boundary");
			static const uint8_t nativePublicationFind[] = {
				0x41, 0x80, 0x8d, 0x91, 0x11, 0x00, 0x00, 0x04,
				0x49, 0x8b, 0x45, 0x00, 0x45, 0x31, 0xff,
				0x4c, 0x89, 0xef, 0x31, 0xf6,
				0xff, 0x90, 0xb0, 0x05, 0x00, 0x00,
				0x4c, 0x89, 0xef,
			};
			static const uint8_t nativePublicationReplace[] = {
				0x41, 0x80, 0x8d, 0x91, 0x11, 0x00, 0x00, 0x04,
				0x49, 0x8b, 0x45, 0x00, 0x45, 0x31, 0xff,
				0x4c, 0x89, 0xef, 0x31, 0xf6,
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
				0x4c, 0x89, 0xef,
			};
			LookupPatchPlus const nativePublicationPatch {
				activeKext, nativePublicationFind, nativePublicationReplace, 1,
			};
			PANIC_COND(!nativePublicationPatch.apply(
			               patcher, acceleratorStart,
			               acceleratorStartEnd - acceleratorStart), "ngreen",
			           "Failed to defer native VF service publication");
			SYSLOG("ngreen", "V323: deferred native synchronous VF service publication until callback validation");

			// V347 (opt-in -ngnoccs): the PF exposes CCS0, which shares the render
			// slice and reset domain with RCS. start() drops CCS0 from the engine
			// mask unless a feature bit is set; make that unconditional so compute
			// queues use RCS, as on parts without a CCS.
			static const uint8_t keepCcsFind[] = {
				0x41, 0xf6, 0x46, 0x04, 0x02, 0x75, 0x08,
				0x41, 0x80, 0xa5, 0x00, 0x13, 0x00, 0x00, 0xfd,
			};
			static const uint8_t keepCcsReplace[] = {
				0x41, 0xf6, 0x46, 0x04, 0x02, 0x90, 0x90,
				0x41, 0x80, 0xa5, 0x00, 0x13, 0x00, 0x00, 0xfd,
			};
			LookupPatchPlus const keepCcsPatch {activeKext, keepCcsFind, keepCcsReplace, 1};
			const bool dropped = keepCcsPatch.apply(patcher, acceleratorStart, acceleratorStartEnd - acceleratorStart);
			SYSLOG("ngreen", "V347: drop CCS0 from the engine mask: %d", dropped);

			// The exact V335 media panic reached Tahoe's native
			// calculateIOSurfaceDeviceCacheVRAMBytes with a non-zero plane
			// count but zero bytes-per-element and row-bytes.  The native TGL,
			// ICL and KBL implementations all divide those two values without a
			// guard; their existing zero-plane branch is the surface-level
			// fallback.  Preserve the native calculation byte-for-byte when both
			// divisors are valid, and classify only this otherwise impossible
			// sparse-plane shape as the existing fallback.  This patch is inside
			// the classified-VF block, so PF and non-target devices retain the
			// untouched Apple implementation.
			mach_vm_address_t surfaceBytesStart = 0;
			mach_vm_address_t surfaceBytesEnd = 0;
			KernelPatcher::SolveRequest surfaceBytesBounds[] = {
				{"__ZN15IGAccelResource38calculateIOSurfaceDeviceCacheVRAMBytesEPyS0_",
				 surfaceBytesStart},
				{"__ZN15IGAccelResource14getLevelOffsetEhhPi", surfaceBytesEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, surfaceBytesBounds, address, size) ||
			           surfaceBytesEnd - surfaceBytesStart != 0xb68,
			           "ngreen", "Changed VF IOSurface cache-size boundary");
			static const uint8_t surfacePlaneDivisionFind[] = {
				0x83, 0xbd, 0x24, 0xff, 0xff, 0xff, 0x00,
				0x89, 0x9d, 0x20, 0xff, 0xff, 0xff,
				0x74, 0x1b,
				0x48, 0x8b, 0x4d, 0x80,
				0x89, 0xc8, 0x31, 0xd2, 0x41, 0xf7, 0xf7,
				0x41, 0x89, 0xc4,
				0x89, 0xd8, 0x31, 0xd2, 0xf7, 0xf1,
				0x89, 0xc3,
				0xe9, 0xca, 0x00, 0x00, 0x00,
			};
			static const uint8_t surfacePlaneDivisionReplace[] = {
				0x89, 0x9d, 0x20, 0xff, 0xff, 0xff,
				0x45, 0x85, 0xff,
				0x74, 0x1f,
				0x48, 0x8b, 0x4d, 0x80,
				0x85, 0xc9,
				0x74, 0x17,
				0x89, 0xc8, 0x31, 0xd2, 0x41, 0xf7, 0xf7,
				0x41, 0x89, 0xc4,
				0x89, 0xd8, 0x31, 0xd2, 0xf7, 0xf1,
				0x89, 0xc3,
				0xe9, 0xca, 0x00, 0x00, 0x00,
			};
			static_assert(sizeof(surfacePlaneDivisionFind) ==
			              sizeof(surfacePlaneDivisionReplace),
			              "V338 plane fallback patch must preserve code size");
			LookupPatchPlus const surfacePlaneDivisionPatch {
				activeKext, surfacePlaneDivisionFind,
				surfacePlaneDivisionReplace, 1,
			};
			PANIC_COND(!surfacePlaneDivisionPatch.apply(
			               patcher, surfaceBytesStart,
			               surfaceBytesEnd - surfaceBytesStart), "ngreen",
			           "Failed to guard sparse VF IOSurface plane divisors");
			SYSLOG("ngreen", "V338: admitted sparse VF IOSurface plane through native surface-level fallback");

			KernelPatcher::RouteRequest requests[] = {
			// Tahoe's signed TGL VADriver asks selector 0xb for IntelHwCapsInfo
			// and admits only canonical TGL PCI identities.  The SR-IOV VF has a
			// distinct TGL PCI device id while the exact Tahoe TGL userspace ABI
			// uses the same 0xb/0x100/0x101 user-client selector surface.  Preserve
			// the kernel's real 0x9a49 identity everywhere; only canonicalize the
			// successful, fixed-size user-client reply used to create the media
			// physical-accelerator object.
			{"__ZN13IGAccelDevice11get_hw_capsEP16_IntelHwCapsInfoS1_yPy",
			 vfGetHwCaps, this->oVfGetHwCaps},
			// V341 remains observation-only.  V340 proved selector 0x100 returns
			// successfully but userspace never reaches 0x101.  Record the exact
			// 32-bit accelerator id that the signed TGL VADriver subsequently passes
			// to IOSurfaceBindAccel, while keeping all arguments, results and context
			// state unchanged.  The existing 0x101/0x102 brackets stay in place for
			// the next boundary.
			{"__ZN19IGAccelVideoContext22get_iosurface_accel_idEP37sIntelVideoMethodArgsGetAcceleratorIdPy",
			 vfVideoGetIosurfaceAccelId, this->oVfVideoGetIosurfaceAccelId},
			{"__ZN19IGAccelVideoContext22update_perf_capabilityEP37sIntelVideoMethodArgsPerfCapabilityIny",
			 vfVideoUpdatePerfCapability, this->oVfVideoUpdatePerfCapability},
			{"__ZN19IGAccelVideoContext20set_context_priorityEP39sIntelVideoMethodArgsSetContextPriorityy",
			 vfVideoSetContextPriority, this->oVfVideoSetContextPriority},
			// V217: Query the media-12 PF-provisioned GGTT range, replace Apple's
			// zero/stolen-derived allocator ranges, and validate direct BAR0 PTE
			// mappings plus their required GuC TLB invalidation lifecycle.
			{"__ZN15IGMemoryManager12initSegmentsEv",
			 IGMemoryManagerInitSegments},
			{"__ZN25IGHardwareGlobalPageTable15initWithOptionsEP16IntelAcceleratorRK14IGAddressRangePvyj",
			 IGHardwareGlobalPageTableInitWithOptions,
			 this->oIGHardwareGlobalPageTableInitWithOptions},
			{"__ZN25IGHardwareGlobalPageTable8mapRangeERK14IGAddressRangeyy",
			 IGHardwareGlobalPageTableMapRange},
			{"__ZN25IGHardwareGlobalPageTable15mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory",
			 IGHardwareGlobalPageTableMapRangeRotated},
			{"__ZN25IGHardwareGlobalPageTable10unmapRangeERK14IGAddressRange",
			 IGHardwareGlobalPageTableUnmapRange},
			{"__ZN25IGHardwareGlobalPageTable13mapRangeDummyERK14IGAddressRangey",
			 IGHardwareGlobalPageTableMapRangeDummy},
			// The 32-bit unmapper does not prune table pages, so retire engine
			// translations after its PTE stores. The 64-bit unmapper tail-calls
			// shrinkRange, which can return table pages to the pool; intercept that
			// boundary and retire translations before native pruning/zeroing.
			{"__ZN31IGHardwarePerProcessPageTable3210unmapRangeERK14IGAddressRange",
			 vfPpgtt32UnmapRange, this->oVfPpgtt32UnmapRange},
			{"__ZN31IGHardwarePerProcessPageTable6410unmapRangeERK14IGAddressRange",
			 vfPpgtt64UnmapRange, this->oVfPpgtt64UnmapRange},
			{"__ZN31IGHardwarePerProcessPageTable6411shrinkRangeERK14IGAddressRange",
			 vfPpgtt64ShrinkRange, this->oVfPpgtt64ShrinkRange},
			// Native commit fan-out preserves successful segment/address-space
			// prefixes when a later table fails. Roll every table back while the
			// task, mapping and native caller's ownership scope are still intact.
			{"__ZN15IGMemoryManager26commitIntoPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap",
			 vfCommitPageTablesForTask, this->oVfCommitPageTablesForTask},
			{"__ZN15IGMemoryManager27releaseFromPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap",
			 vfReleasePageTablesForTask, this->oVfReleasePageTablesForTask},
			{"__ZN15IGMemoryManager22updatePageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap",
			 vfUpdatePageTablesForTask, this->oVfUpdatePageTablesForTask},
			// Native synchronization has a void ABI: both the per-entry and the
			// descriptor path can leave a partially populated PPGTT and still return
			// it to the task. Rebuild that one factory transaction with observable
			// map results and release the unpublished table on any failure.
			{"__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask",
			 vfNewPageTableForTask, this->oVfNewPageTableForTask},
			// Display-mode changes synchronize the kernel table and every published
			// task through void helpers. Serialize the complete iterator, make entry
			// failures fail closed, and retain replaced shared descriptors until an
			// acknowledged engine invalidation has completed.
			{"__ZN15IGMemoryManager19synchronizeAllTasksEv",
			 vfSynchronizeAllTasks, this->oVfSynchronizeAllTasks},
			{"__ZN29IGHardwarePerProcessPageTable20synchronizeEachEntryEPK19IGHardwarePageTableRK14IGAddressRangeb",
			 vfSynchronizeEachEntry},
			{"__ZN31IGHardwarePerProcessPageTable6423remapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE",
			 vfPpgtt64RemapDescriptor, this->oVfPpgtt64RemapDescriptor},
			// Tahoe constructs the manager pools without their optional native lock.
			// Put allocation, zero-before-return, prune and final teardown in the
			// same recursive ownership domain as every task/table transaction.
			{"__ZN10IGPagePool12allocatePageEv",
			 vfPagePoolAllocatePage, this->oVfPagePoolAllocatePage},
			{"__ZN10IGPagePool11releasePageEPKNS_14PageDescriptorE",
			 vfPagePoolReleasePage, this->oVfPagePoolReleasePage},
			{"__ZN10IGPagePool5pruneEj",
			 vfPagePoolPrune, this->oVfPagePoolPrune},
			{"__ZN10IGPagePool4freeEv",
			 vfPagePoolFree, this->oVfPagePoolFree},
			{"__ZN15IGMemoryManager15releasePagePoolEv",
			 vfReleasePagePool, this->oVfReleasePagePool},
			// Normal manager destruction independently releases dummy/global page
			// tables and clears every published segment before its base free. Keep
			// that complete owner teardown in the transaction too; locking only the
			// pool helpers would still allow synchronization to borrow a cleared
			// +0x98 table or range while final free was in progress.
			{"__ZN15IGMemoryManager4freeEv",
			 vfMemoryManagerFree, this->oVfMemoryManagerFree},
			// Cache-type requests from Metal, GL, blit and media all converge here.
			// Keep resource flags and every installed page table in one recoverable
			// transaction; the native void tail otherwise hides a partial failure.
			{"__ZN15IGAccelResource22updateMappingCacheTypeEj",
			 vfUpdateMappingCacheType, this->oVfUpdateMappingCacheType},
			// Several native callers ignore a false reservation, while timeout
			// recovery can return true without rechecking capacity. Validate the
			// complete reservation before any common ring writer can run.
			{"__ZN20IGHardwareRingBuffer12waitForSpaceEj",
			 vfWaitForRingSpace, this->oVfWaitForRingSpace},

			// Tahoe's aperture-resource path allocates a legacy hardware fence whose
			// constructor and destructor both write the PF-owned 0x100000 fence
			// register bank under physical force-wake. A VF owns no such registers.
			// Reject at the allocation boundary before a fence object or slot exists;
			// the native callers already unwind a null result transactionally.
			{"__ZN16IGFenceAllocator8allocateERK14IGAddressRangem19GFX3DSTATE_TILEMODE",
			 vfRejectPhysicalFence},

			// Every cache/PAT/MOCS member below writes PF-owned global register
			// state. startGraphicsEngine is already replaced, but route the complete
			// virtual surface too so a future direct or cross-kext dispatch cannot
			// recover the physical initializer on a VF.
			{"__ZN15IGMemoryManager9initCacheEv",
			 vfDisablePhysicalCacheInit},
			{"__ZN15IGMemoryManager20initMOCSTableEntriesEv",
			 vfDisablePhysicalCacheInit},
			{"__ZN21IntelTGLMemoryManager14initGTL3LayoutEv",
			 vfDisablePhysicalCacheInit},
			{"__ZN21IntelTGLMemoryManager20initMOCSTableEntriesEv",
			 vfDisablePhysicalCacheInit},
			{"__ZN21IntelTGLMemoryManager24updatePageAttributeTableEv",
			 vfDisablePhysicalCacheInit},

			// IGMemoryManager::init invokes this TGL virtual before engine start.
			// Native detection reads and may program physical eDRAM registers even
			// after force-wake itself has been suppressed. A VF has no eDRAM aperture;
			// retain the base initializer's false capability state without MMIO.
			{"__ZN21IntelTGLMemoryManager11detectEDRAMEv",
			 vfDisableEdramProbe},

			// The independent PAVP callback reaches physical force-wake and the
			// PF-owned 0x1082c0/0x320fc register pair before protected-media ring
			// submission. A VF cannot implement that protocol, and fabricated success
			// would falsely acknowledge DRM state. Fail at the callback root while a
			// physical function retains Apple's complete native implementation.
			{"__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb",
			 vfRejectPavpCommandCallback},
			// This independently registered power callback can stop/start the
			// accelerator and synchronize display-owned resources without passing
			// through an IOUserClient externalMethod.  Give its complete callback
			// lifetime one lease at the concrete IntelAccelerator receiver.
			{"__ZN16IntelAccelerator20DisplaySleepCallbackE17DisplaySleepCmd_tjj",
			 vfDisplaySleepCallback, this->oVfDisplaySleepCallback},
			// Telemetry is intentionally unavailable on a VF. recognizeFlip has no
			// in-image caller but is exported and tail-calls IGTelemetryKMD::sample,
			// which can submit a main-ring command. Remove that conservative producer
			// root rather than admitting a trace operation which cannot produce data.
			{"__ZN25IGAccelTraceStreamManager13recognizeFlipEv",
			 vfIgnoreTraceRecognizeFlip},
			
			// A VF owns neither engine power/reset nor legacy execlist rings. Keep
			// Apple's lifecycle calls away from PF-owned registers; final stop still
			// performs explicit direct-LRCA and DMA quiescence below.
			{"__ZN16IntelAccelerator19startGraphicsEngineEv", startGraphicsEngine},
			// Tahoe's headless bridge registers its PCI MSI source before the
			// scheduler and HWS mappings exist. IOPCIFamily registration also
			// enables PCI bus mastering. Defer the MSI allocation and source
			// construction to the routed engine-start boundary, while retaining
			// the bridge object now so native scheduler callback ownership remains
			// unchanged.
			{"__ZN17IGInterruptBridge32createFilterInterruptEventSourceEv",
			 vfDeferFilterInterruptEventSource,
			 this->oVfCreateFilterInterruptEventSource},
			{"__ZN11IGScheduler6createEP16IntelAccelerator", vfCreateScheduler,
			 this->originalSchedulerCreate},
			// Scheduler type is already locked to 4 before the native dispatcher,
			// but these factory routes also reject an unexpected direct caller.
			{"__ZN5IGGuC15withAcceleratorEP22IOGraphicsAccelerator2",
			 vfRejectLegacySchedulerFactory},
			{"__ZN5IGGuC15initWithOptionsEP22IOGraphicsAccelerator2",
			 vfRejectLegacySchedulerInit},
			{"__ZN12IGScheduler515withAcceleratorEP22IOGraphicsAccelerator2",
			 vfRejectLegacySchedulerFactory},
			{"__ZN12IGScheduler519initWithAcceleratorEP22IOGraphicsAccelerator2",
			 vfRejectLegacySchedulerInit},
			{"__ZN26IGHardwareCommandStreamer523hardwareCommandStreamerEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler510IGHwCsType",
			 vfRejectLegacyCommandStreamerFactory},
			{"__ZN26IGHardwareCommandStreamer54initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler510IGHwCsType",
			 vfRejectLegacyCommandStreamerInit},
			{"__ZN11IGScheduler15initWithOptionsEjyP22IOGraphicsAccelerator2", vfInitScheduler,
			 this->originalSchedulerInit},
			{"__ZN16IntelAccelerator18stopGraphicsEngineEv",  stopGraphicsEngine},

			// These generic helpers still dereference accelerator+0x1240 after
			// force-wake. Current direct callers are legacy IGGuC-only, but reject at
			// the final sink as well so no indirect caller can touch physical BAR0.
			{"__ZN16IntelAccelerator18SafeReadRegister32Em",
			 vfRejectSafeReadRegister32},
			{"__ZN16IntelAccelerator18SafeReadRegister64Em",
			 vfRejectSafeReadRegister64},
			{"__ZN16IntelAccelerator19SafeWriteRegister32Emj",
			 vfRejectSafeWriteRegister32},
			// Scheduler4 exposes this method through its vtable and writes the
			// PF-owned 0xA210 power register. A VF does not implement that protocol.
			{"__ZN12IGScheduler415setPmAttributesEPKv",
			 vfRejectSchedulerPmAttributes},

			// A VF has no guest-owned INSTDONE state. Derive the watchdog result
			// solely from the tracked GuC context lifecycle.
			{"__ZNK12IGScheduler59isGpuIdleEv", wrapIGScheduler5IsGpuIdle},
			{"__ZNK12IGScheduler49isGpuIdleEv", wrapIGScheduler4IsGpuIdle},

			// Native timeout diagnostics bypass the virtual GuC transport: they
			// halt/resume a physical RING_MI_MODE register, capture the legacy GuC
			// scratch bank and write the physical INSTDONE selector. The ring-buffer
			// diagnosis entry points additionally dump a broad PF-owned MMIO
			// registers. A true event timeout must not return to inherited restart
			// and wait retry with an unproven hardware state. Diagnostics remain
			// inert; the separately verified timeout root fail-stops the guest.
			{"__ZN19IGAccelEventMachine12eventTimeoutEi", vfRejectEventTimeout},
			{"__ZN11IGScheduler19haltCommandStreamerE10IGHwCsType",
			 vfSuppressTimeoutHardwareAction},
			{"__ZN11IGScheduler21resumeCommandStreamerE10IGHwCsType",
			 vfSuppressTimeoutHardwareAction},
			{"__ZN16IntelAccelerator15encodeDebugInfoE15IGTimeoutReason",
			 vfSuppressPhysicalDebugCapture},
			{"__ZN20IGHardwareRingBuffer14doHangAnalysisEv",
			 vfSuppressHangAnalysis},
			{"__ZN20IGHardwareRingBuffer16dumpHangAnalysisEv",
			 vfSuppressHangDump},
			// FIFO recovery dispatches through a ring-buffer virtual that performs
			// a complete physical engine reset, then replays the timed-out work.
			// A VF can neither reset its PF-owned engine nor safely claim that the
			// replay boundary succeeded. Quarantine the root request and make the
			// lower physical primitive fail closed for every caller.
			{"__ZN18IGAccelFIFOChannel22resetHardwareAndReplayEv",
			 vfRejectHardwareResetReplay},
			{"__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext",
			 vfRejectPhysicalEngineReset},

			// Legacy reset-register replay is physical-engine state and is not
			// constructed for a direct-LRCA VF.
			{"__ZN16IntelAccelerator25populateResetRegisterListEv", populateResetRegisterList},

			 // Keep the bootstrap task identity coherent before native allocation.
			 // Allocation failures propagate; no borrowed kernel task is substituted.
			 {"__ZN11IGAccelTask11withOptionsEP16IntelAccelerator", igAccelTaskWithOptions, this->oigAccelTaskWithOptions},
			 // Diagnostic-only V316 brackets distinguish scratch/stamp completion
			 // from the following fixed-size Aux page-table construction. Both
			 // wrappers preserve the native ABI, arguments, result and ownership.
			 {"__ZN11IGAccelTask24initStampAndScratchPagesEv",
			  vfInitStampAndScratchPages, this->oVfInitStampAndScratchPages},
			 {"__ZN14IGAuxPageTable11withOptionsEP16IntelAcceleratorP11IGAccelTask",
			  vfAuxPageTableWithOptions, this->oVfAuxPageTableWithOptions},
			 // V214: During IOAccel bootstrap IntelAccelerator+0x150 is still null.  The
			 // TGL driver otherwise takes the non-kernel branch in newPageTableForTask
			 // and dereferences that null task at +0x260.  Treat only this first VF task
			 // as the kernel task so it synchronizes from Global GTT; once +0x150 is
			 // populated, preserve Apple's classification for every later task.
			 {"__ZNK11IGAccelTask15isKernelGPUTaskEv", IGAccelTaskIsKernelGPUTask, this->oIGAccelTaskIsKernelGPUTask},
			 // Final task destruction releases Aux/PPGTT objects before inherited
			 // cleanup. Retire translations while their owners are still intact.
			 {"__ZN11IGAccelTask4freeEv", vfAccelTaskFree,
			  this->oVfAccelTaskFree},
			 // Route the four native context producers so incomplete allocation is
			 // rejected before Tahoe dereferences each context's FIFO at +0xb8.
			 {"__ZN11IGAccelTask16getBlit3DContextEb", getBlit3DContext, this->ogetBlit3DContext},
		
			 // Preserve native submit work, but validate the borrowed task and every
			 // context object before the pinned Tahoe body dereferences its FIFO.
			 // A non-empty VF submission may not return false: many native ICB,
			 // paging, Surface, GL and Metal callers ignore AL and would otherwise
			 // publish CPU-side progress for work which never reached a ring.
			 {"__ZN16IntelAccelerator10submitBlitEP15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyEP11IGAccelTaskb", submitBlit, this->osubmitBlit},
			 // Resource CCS/depth callers have the same boolean ABI problem. Route
			 // their common pre-publication boundaries so a rejected later plane or
			 // chunk cannot be converted into successful partial resolve state.
			 {"__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh",
			  vfSubmitCCSResolve, this->oVfSubmitCCSResolve},
			 {"__ZN15IGAccelResource18submitDepthResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask17EIntelResolveTypehhtt",
			  vfSubmitDepthResolve, this->oVfSubmitDepthResolve},

			 // barrierSubmission must keep its event/FIFO side effects. The wrapper
			 // admits the original only after all contexts used by that body exist.
			 {"__Z17barrierSubmissionR19IGAccelCommandQueueR16IntelAcceleratorR24IGAccelCommandDescriptorR12IOAccelEventtPKt", barrierSubmission, this->obarrierSubmission},
			 
			 // V117: Hook getBlit2DContext for null-guarding only.
			 // submitBlit expects a real Blit2D context layout and unconditionally reads
			 // [ctx+0xb8], so we must never substitute depth/color/3D context objects here.
			 {"__ZN11IGAccelTask16getBlit2DContextEb", getBlit2DContext, this->ogetBlit2DContext},

			 // Resolve-context getters are used by barrierSubmission and are generally safe.
			 {"__ZN11IGAccelTask22getDepthResolveContextEb", getDepthResolveContext, this->ogetDepthResolveContext},
			 {"__ZN11IGAccelTask22getColorResolveContextEb", getColorResolveContext, this->ogetColorResolveContext},
			 
			};
			SYSLOG("ngreen", "V165: routing %zu VF accelerator symbols", sizeof(requests)/sizeof(requests[0]));
			PANIC_COND(!patcher.routeMultiple(index, requests, address, size),
				"ngreen", "Failed to route VF accelerator symbols");
			SYSLOG("ngreen", "V165: VF accelerator symbols routed OK");
		}

		{
			KernelPatcher::RouteRequest startRoute[] = {
				{"__ZN16IntelAccelerator5startEP9IOService", start, this->ostart},
				{"__ZN16IntelAccelerator4stopEP9IOService", acceleratorStop,
				 this->oAcceleratorStop},
			};
			PANIC_COND(!patcher.routeMultiple(index, startRoute, address, size),
			           "ngreen", "Cannot admit pinned accelerator without lifecycle routes");
			SYSLOG("ngreen", "V242: Hooked IntelAccelerator start/stop lifecycle");
		}

		if (vfActive) {
			// The PF already owns GuC firmware. Replace only the VF transport and
			// lifecycle entry points; a physical GPU keeps Apple's native dispatch.
			KernelPatcher::RouteRequest firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary},
				// V284: these legacy/base producers can bypass the modern
				// IGHardwareGuC transport entirely.  Reject at their common entry
				// points before any PF-owned DMA, CTB, doorbell or execlist MMIO.
				{"__ZN5IGGuC20sendHostToGucMessageEPK18IGHostToGucMessagejU13block_pointerFvvE",
				 vfRejectLegacyGucMessage},
				{"__ZN5IGGuC12dmaHostToGuCEyjjNS_12IGGucDmaTypeEb",
				 vfRejectLegacyGucDma},
				{"__ZN5IGGuC12ringDoorbellE10IGHwCsType",
				 vfRejectLegacyDoorbell},
				{"__ZN21IGHardwareGuCCTBuffer15hostToGuCActionEPKjjiPjb",
				 vfRejectNativeCtbAction},
				{"__ZN26IGHardwareCommandStreamer514submitExecListEj",
				 vfRejectLegacyExecList},
				{"__ZN16IntelAccelerator17transferOwnershipEPK20IGSharedMappedBufferi",
				 vfTransferOwnership},
				// V222: scheduler 4 uses the Gen11 reference GuC transport, but its
				// stock MMIO helper writes the legacy 0xc180 scratch registers.  A VF
				// is provisioned only for the Gen11 0x190240/0x1901f0 mailbox.
				{"__ZN13IGHardwareGuC19mmioHostToGuCActionEPKjjiPj",
				 vfMmioHostToGuCAction},
				{"__ZN13IGHardwareGuC15hostToGuCActionEPKjjiPj",
				 vfLegacyHostToGuCAction},
				{"__ZN13IGHardwareGuC15createUkContextEy25UK_GEN11_CONTEXT_PRIORITY",
				 vfCreateUkContext},
				{"__ZN13IGHardwareGuC14allocContextIdEyb",
				 vfAllocContextId},
				{"__ZN13IGHardwareGuC16releaseContextIdEj",
				 vfReleaseContextId},
				// These routines touch raw physical doorbell registers BEFORE
				// calling hostToGuCAction; guarding the sender alone is too late.
				{"__ZN13IGHardwareGuC15acquireDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_RECb",
				 vfAcquireDoorbell},
				{"__ZN13IGHardwareGuC15releaseDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_REC",
				 vfReleaseDoorbell},
				{"__ZN13IGHardwareGuC15allocUkDoorbellEjb",
				 vfAllocUkDoorbell},
				{"__ZN13IGHardwareGuC17reacquireDoorbellEj",
				 vfReacquireDoorbell},
				{"__ZN13IGHardwareGuC9isGuCIdleEv", vfIsGuCIdle},
				{"__ZN13IGHardwareGuC13isContextIdleEj", vfIsContextIdle},
				{"__ZN13IGHardwareGuC16isKmdContextIdleERK21SGfxContextDescriptor",
				 vfIsKmdContextIdle},
				// V227: initDoorbells consumes DISTRDB before any submission.
				// Bypass the stock routine for a VF so an all-ones MMIO read
				// cannot turn into a 16 x 256 topology and corrupt the object.
				{"__ZN13IGHardwareGuC13initDoorbellsEv",
				 vfInitDoorbells},
				// V226: DISTRDB (0xd08) is outside a VF's MMIO allowlist and
				// reads as all ones.  Feed Apple's allocator the real Gen12
				// eight-by-32 topology after validating the VF's GuC KLV quota.
				{"__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv",
				 vfReadDoorbellSQIDIConfig},
				// V223: enlarge and re-layout Apple's legacy 1 KiB CTB rings before
				// translating their registration to the modern VF KLV ABI.
				{"__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2",
				 vfCtbInitWithAccelerator, this->oVfCtbInitWithAccelerator},
				{"__ZN21IGHardwareGuCCTBuffer13ctChannelInitEv",
				 vfCtbChannelInit},
				{"__ZN21IGHardwareGuCCTBuffer15gucToHostActionEPj",
				 vfCtbGucToHostAction},
				{"__ZN13IGHardwareGuC32handleSoftwareGuCToHostInterruptEP22IOInterruptEventSourcei",
				 vfSoftwareGuCInterrupt},
				// These exported generic helpers accept caller-selected BAR0 offsets.
				// The pinned payload has no genuine direct, vtable or address-taken
				// caller for either helper; reject any future cross-kext entry rather
				// than treating arbitrary offsets as part of the fixed Gen11 VF IRQ
				// register protocol retained below.
				{"__ZN17IGInterruptBridge32clearQueuedHeirarchichalIntrBitsEjjjj",
				 vfRejectDynamicInterruptClear},
				{"__ZN17IGInterruptBridge7readIIREjjj",
				 vfRejectDynamicInterruptRead},
				// startGraphicsEngine enables the bridge before GuC construction so
				// CTB-backed allocations can wait for completions. Tahoe's native
				// request method only appends to a list that enable() services once;
				// immediately run callbacks registered after that one-shot boundary.
				{"__ZN17IGInterruptBridge21requestEnableCallbackEP8OSObjectPFvS1_zE",
				 vfRequestEnableCallback, this->oVfRequestEnableCallback},
				// V237: 0xCEE8 is PF-owned.  A VF invalidates GuC translations
				// through the asynchronous v70 CT action instead.
				{"__ZN13IGHardwareGuC13invalidateTLBEv",
				 vfInvalidateTLB},
				// IGGuC owns context-private GGTT updates and has no CTB pointer.
				// Route its invalidator separately: use the captured hardware GuC once
				// transport is ready, and match i915's pre-ready no-op behavior.
				{"__ZNK5IGGuC13invalidateTLBEv",
				 vfBaseInvalidateTLB},
				{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj",
				 vfCtbMappedBufferWithOptions, this->oVfCtbMappedBufferWithOptions},
				// V230: translate Tahoe's legacy process-wide proxy submission
				// into the one-GuC-ID-per-LRCA lifecycle required by v70 and VFs.
				{"__ZN13IGHardwareGuC29AttachContextDescToGucContextERK21SGfxContextDescriptor",
				 vfAttachContextDesc},
				{"__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor",
				 vfDetachContextDesc},
				// V298: this is the only Tahoe boundary that still carries the
				// submitToRing stamp-present bit. Bind it synchronously to the routed
				// inner CTB publication; normal idle remains conservative below.
				{"__ZN12IGScheduler44pushEP17IGHardwareContextjjbb",
				 vfSchedulerPush, this->oVfSchedulerPush},
				{"__ZN13IGHardwareGuC14submitWorkItemEjRK21SGfxContextDescriptor10IGHwCsTypejjj",
				 vfSubmitWorkItem},
				// V324: merge redirected stamps right before Apple reads them
				// (flush-notify secondary handlers, workloop context).
				{"__ZN16IntelAccelerator17signalStampUpdateERK8IGBitSetILm64EE",
				 vfSignalStampUpdate, this->oVfSignalStampUpdate},
			};
			PANIC_COND(!patcher.routeMultiple(index, firmwareRoute, address, size), "ngreen", "Failed to route VF GuC firmware transport");
			KernelPatcher::RouteRequest submitProbes[] = {
				{"__ZN19IGAccelCommandQueue20processCommandBufferEjj", vfSubmitProbe<0>, gVfSubmitProbeOrg[0]},
				{"__ZN18IGAccelFIFOChannel12submitBufferEP24IOAccelCommandDescriptor", vfSubmitProbe<1>, gVfSubmitProbeOrg[1]},
				{"__ZN19IGAccelCommandQueue19endCoalescedSegmentEv", vfSubmitProbe<2>, gVfSubmitProbeOrg[2]},
			};
			SYSLOG("ngreen", "V354: submit probes routed %d", patcher.routeMultiple(index, submitProbes, address, size));
			patcher.clearError();

			if (gVfUsesMemoryIrq) {
				// Only mtl_info-class VFs use the PF-provisioned memory-IRQ page.
				// Their filter, reader and bridge/scheduler programming must stay
				// entirely memory-backed.
				KernelPatcher::RouteRequest memoryIrqRoutes[] = {
					{"__ZN17IGInterruptBridge22interruptFilterHandlerEP28IOFilterInterruptEventSource",
					 vfInterruptFilterHandler},
					{"__ZN17IGInterruptBridge22readAndClearInterruptsER8IGBitSetILm46EE",
					 vfReadAndClearInterrupts},
					{"__ZN17IGInterruptBridge16enableInterruptsEv",
					 vfEnableInterrupts},
					{"__ZN17IGInterruptBridge17disableInterruptsEv",
					 vfDisableInterrupts},
					{"__ZN12IGScheduler416enableInterruptsEv",
					 vfEnableInterrupts},
					{"__ZN12IGScheduler417disableInterruptsEv",
					 vfDisableInterrupts},
				};
				PANIC_COND(!patcher.routeMultiple(
				               index, memoryIrqRoutes, address, size), "ngreen",
				           "Failed to route VF memory-IRQ transport");
			} else {
				// TGL/ADL/RPL implement the same Gen11 virtual interrupt MMIO
				// protocol consumed by Tahoe's native bridge. Keep its filter,
				// read/clear, masks and logical scheduler callback intact. Only the
				// nested per-engine error helpers touch physical RING_* registers.
				KernelPatcher::RouteRequest virtualMmioIrqRoutes[] = {
					{"__ZN12IGScheduler421enableErrorInterruptsEv",
					 vfSuppressPhysicalErrorInterrupts},
					{"__ZN12IGScheduler422disableErrorInterruptsEv",
					 vfSuppressPhysicalErrorInterrupts},
				};
				PANIC_COND(!patcher.routeMultiple(
				               index, virtualMmioIrqRoutes, address, size), "ngreen",
				           "Failed to isolate VF physical engine error interrupts");
			}
		}

		// V237: IGHardwareGuCCTBuffer::initWithAccelerator directly accesses the
		// PF-owned 0xCEE8 register.  Remove the complete write/read/poll sequence
		// only on a verified VF. Physical hardware retains its native CT ABI.
		if (vfActive) {
			// Resolve every native body that a VF still enters, including bodies
			// captured as originals by a wrapper. Fully replaced routines are not
			// mutated: their entry routes are the isolation boundary. Each live
			// patch below is confined by adjacent symbols and has one pinned anchor.
			mach_vm_address_t workQueueInit = 0, workQueueInitEnd = 0;
			mach_vm_address_t workQueueFree = 0, workQueueFreeEnd = 0;
			mach_vm_address_t ctbInit = 0, ctbInitEnd = 0;
			mach_vm_address_t ctbFree = 0, ctbFreeEnd = 0;
			mach_vm_address_t releaseUkContext = 0, releaseUkContextEnd = 0;
			KernelPatcher::SolveRequest tlbPatchBounds[] = {
				{"__ZN22IGHardwareGuCWorkQueue19initWithAcceleratorEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 workQueueInit},
				{"__ZN22IGHardwareGuCWorkQueue9lockQueueEv", workQueueInitEnd},
				{"__ZN22IGHardwareGuCWorkQueue4freeEv", workQueueFree},
				{"__ZN22IGHardwareGuCWorkQueue18calculateFreeSpaceEv", workQueueFreeEnd},
				{"__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2",
				 ctbInit},
				{"__ZN21IGHardwareGuCCTBuffer9lockQueueE34UK_GEN11_CMD_TRANSPORT_BUFFER_TYPE",
				 ctbInitEnd},
				{"__ZN21IGHardwareGuCCTBuffer4freeEv", ctbFree},
				{"__ZN21IGHardwareGuCCTBuffer15hostToGuCActionEPKjjiPjb", ctbFreeEnd},
				{"__ZN13IGHardwareGuC16releaseUkContextEj", releaseUkContext},
				{"__ZN13IGHardwareGuC13isContextIdleEj", releaseUkContextEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, tlbPatchBounds,
			                                      address, size) ||
			           workQueueInitEnd <= workQueueInit ||
			           workQueueInitEnd - workQueueInit > 0x200 ||
			           workQueueFreeEnd <= workQueueFree ||
			           workQueueFreeEnd - workQueueFree > 0x100 ||
			           ctbInitEnd <= ctbInit || ctbInitEnd - ctbInit > 0x200 ||
			           ctbFreeEnd <= ctbFree || ctbFreeEnd - ctbFree > 0x100 ||
			           releaseUkContextEnd <= releaseUkContext ||
			           releaseUkContextEnd - releaseUkContext > 0x200,
			           "ngreen", "Invalid VF physical-TLB patch bounds");
			LookupPatchPlus const vfCtbTlbPollPatch {
				activeKext, NGVfTlbPatch::ctbInitFind,
				NGVfTlbPatch::ctbInitReplace,
				sizeof(NGVfTlbPatch::ctbInitFind), 1
			};
			PANIC_COND(!vfCtbTlbPollPatch.apply(
			               patcher, ctbInit, ctbInitEnd - ctbInit), "ngreen",
			           "V222: failed to bypass VF CTB TLB poll");
			SYSLOG("ngreen", "V237: removed physical TLB access from VF CTB initialization");

			// Remove 0xCEE8 only from native bodies that are still executed. The
			// seven fully routed routines retain their untouched original bytes and
			// cannot be reached through their public entries on a VF.
			LookupPatchPlus const immediateEcxPatches[] = {
				{activeKext, NGVfTlbPatch::writeImmediateFind,
				 NGVfTlbPatch::writeImmediateReplace, 1},
				{activeKext, NGVfTlbPatch::pollEcxFind,
				 NGVfTlbPatch::pollEcxReplace, 1},
			};
			LookupPatchPlus const immediateMemoryPatches[] = {
				{activeKext, NGVfTlbPatch::writeImmediateFind,
				 NGVfTlbPatch::writeImmediateReplace, 1},
				{activeKext, NGVfTlbPatch::pollMemoryFind,
				 NGVfTlbPatch::pollMemoryReplace, 1},
			};
			PANIC_COND(
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 workQueueInit, workQueueInitEnd - workQueueInit) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 workQueueFree, workQueueFreeEnd - workQueueFree) ||
				!LookupPatchPlus::applyAll(patcher, immediateMemoryPatches,
				 ctbFree, ctbFreeEnd - ctbFree) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 releaseUkContext, releaseUkContextEnd - releaseUkContext),
				"ngreen", "V237: failed to isolate a live VF physical-TLB caller");
			SYSLOG("ngreen", "V237: isolated all live native 0xCEE8 callers");

		}

		// Keep IGAccelDevice::deviceStart native, including failure propagation.

		// Admit exactly the 0x9a49 compatibility identity. The old patch removed
		// two branches and made every ID reach GT2 initialization; changing the
		// existing 0x9a40 compare immediate preserves Apple's sentinel and failure
		// paths while accepting the one device ID published by our PCI hook. This
		// comparison belongs to IntelAccelerator::probe(), not getGPUInfo(). Keep
		// its patch bounded to the owning symbol so a valid anchor elsewhere can
		// never conceal a wrong function-range assumption.
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15,
			0x81, 0xff, 0x86, 0x80, 0x49, 0x9a, 0x74, 0x2d,
		};
		mach_vm_address_t probeStart = 0, probeEnd = 0;
		KernelPatcher::SolveRequest probeBounds[] = {
			{"__ZN16IntelAccelerator5probeEP9IOServicePi", probeStart},
			{"__ZN16IntelAccelerator18encodeFailureStackE15IGFailureReason", probeEnd},
		};
		PANIC_COND(!patcher.solveMultiple(index, probeBounds, address, size) ||
		           probeEnd <= probeStart || probeEnd - probeStart > 0x800,
		           "ngreen", "Invalid IntelAccelerator::probe patch bounds");
		LookupPatchPlus const patchesAlways[] = {
			{activeKext, NGVfRuntimePatch::spoofedSkuFind, r3,
			 arrsize(NGVfRuntimePatch::spoofedSkuFind), 1},
		};
		PANIC_COND(!LookupPatchPlus::applyAll(
		               patcher, patchesAlways, probeStart, probeEnd - probeStart),
		           "ngreen", "Failed to patch pinned accelerator probe admission");

		// Apple reads these PF-owned runtime registers through raw BAR0 loads in
		// getGPUInfo(). They are not VF-visible. Replace only those five loads with
		// the values returned by Intel's early MMIO relay, then leave Apple's native
		// population-count and timestamp calculations intact.
		uint8_t vfSliceFuseReplace[] = {
			0x41, 0xbc, 0, 0, 0, 0, 0x90
		};
		uint8_t vfDssFuseReplace[] = {
			0xb9, 0, 0, 0, 0, 0x89, 0x4d, 0xd0, 0x90
		};
		uint8_t vfEuFuseReplace[] = {
			0xb9, 0, 0, 0, 0, 0x89, 0x4d, 0xcc, 0x90
		};
		uint8_t vfMediaFuseReplace[] = {
			0x41, 0xbe, 0, 0, 0, 0, 0x90
		};
		uint8_t vfRpmConfigReplace[] = {
			0xb8, 0, 0, 0, 0, 0x90
		};

		// L3BankCount is selected by branches tied to Apple's original SKU table.
		// Force the single store, but populate its immediate from MIRROR_FUSE3
		// instead of assuming the target machine always has eight banks.
		uint8_t r3b[] = {
			0x90, 0x90, 0x83, 0xf9, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x83, 0xfe, 0x01, 0x90, 0x90, 0x83, 0xfa, 0x0c, 0x90, 0x90, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		
		// V139: RPL-only mitigation for GP faults inside blit3d_submit_rectlist.
		// Some command-buffer pointers on spoofed paths are 8-byte aligned; Apple emits
		// aligned SSE stores (movaps [r9+...], xmmN), which faults on unaligned targets.
		// Convert the hot-path stores to movups to tolerate unaligned command pointers.
		{
			// V52: Split patches into always-apply and RPL-only groups.
			// TGL reads its native fuse layout correctly on either PF or VF;
			// the current later-generation compatibility path must hardcode
			// because fuse layout differs and BCS ring doesn't start.
			mach_vm_address_t gpuInfoStart = 0, gpuInfoEnd = 0;
			KernelPatcher::SolveRequest gpuInfoBounds[] = {
				{"__ZN16IntelAccelerator10getGPUInfoEv", gpuInfoStart},
				{"__ZN16IntelAccelerator14teardownDeviceEP11IOPCIDevice", gpuInfoEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, gpuInfoBounds,
			                                      address, size) ||
			           gpuInfoEnd <= gpuInfoStart ||
			           gpuInfoEnd - gpuInfoStart > 0x1000,
			           "ngreen", "Invalid getGPUInfo patch bounds");
			if (vfActive) {
				PANIC_COND(!gVfRuntimeReady, "ngreen",
					"VF runtime fuses were not published before getGPUInfo patching");
				NGVfRuntime::writeImmediate32(vfSliceFuseReplace + 2,
					gVfRuntimeRegisters.sliceEnable & 0xFFU);
				NGVfRuntime::writeImmediate32(vfDssFuseReplace + 1,
					gVfTopology.geometryDssMask);
				NGVfRuntime::writeImmediate32(vfEuFuseReplace + 1,
					gVfRuntimeRegisters.euDisable & 0xFFU);
				NGVfRuntime::writeImmediate32(vfMediaFuseReplace + 2,
					gVfRuntimeRegisters.veboxVdboxDisable);
				NGVfRuntime::writeImmediate32(vfRpmConfigReplace + 1,
					gVfRuntimeRegisters.rpmConfig0);
				NGVfRuntime::writeImmediate32(r3b + 28,
					gVfTopology.l3BankCount);

				LookupPatchPlus const vfRuntimePatches[] = {
					{activeKext, NGVfRuntimePatch::sliceFuseFind, vfSliceFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::dssFuseFind, vfDssFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::euFuseFind, vfEuFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::mediaFuseFind, vfMediaFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::rpmConfigFind, vfRpmConfigReplace, 1},
					{activeKext, NGVfRuntimePatch::l3BranchFind, r3b, 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(
					patcher, vfRuntimePatches, gpuInfoStart, gpuInfoEnd - gpuInfoStart),
					"ngreen", "Failed to inject PF runtime fuses into VF getGPUInfo");
				SYSLOG("ngreen", "V244: injected relayed VF runtime fuses into pinned getGPUInfo");
			}

			if (!tglGeneration) {
				mach_vm_address_t rectListStart = 0, rectListEnd = 0;
				KernelPatcher::SolveRequest rectListBounds[] = {
					{"__ZL22blit3d_submit_rectlistP23IGHardwareBlit3DContextP15blit3d_params_tPK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyE",
					 rectListStart},
					{"__ZL19IsSurfaceCompressedj", rectListEnd},
				};
				PANIC_COND(!patcher.solveMultiple(
				               index, rectListBounds, address, size) ||
				           rectListEnd <= rectListStart ||
				           rectListEnd - rectListStart > 0x3000,
				           "ngreen", "Invalid blit3d rect-list patch bounds");
				// Every matching store in the pinned blit3d_submit_rectlist body is
				// converted as one mandatory group. A single count patches all matches
				// against the original image; sequential skip-based patches changed the
				// match set after every write and therefore targeted the wrong sites.
				LookupPatchPlus const unalignedStorePatches[] = {
					{activeKext, NGUnalignedPatch::movaps10Find,
					 NGUnalignedPatch::movups10Replace, 1},
					{activeKext, NGUnalignedPatch::movaps30Find,
					 NGUnalignedPatch::movups30Replace, 1},
					{activeKext, NGUnalignedPatch::movaps50Find,
					 NGUnalignedPatch::movups50Replace, 1},
					{activeKext, NGUnalignedPatch::movaps00Find,
					 NGUnalignedPatch::movups00Replace, 6},
					{activeKext, NGUnalignedPatch::movaps20Find,
					 NGUnalignedPatch::movups20Replace, 4},
					{activeKext, NGUnalignedPatch::movaps40Find,
					 NGUnalignedPatch::movups40Replace, 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(
				               patcher, unalignedStorePatches, rectListStart,
				               rectListEnd - rectListStart),
				           "ngreen", "Failed to apply complete unaligned-store patch set");
				SYSLOG("ngreen", "V243: converted all 14 pinned blit3d aligned-store sites");

				SYSLOG("ngreen", "V52: applied later-gen unaligned-store compatibility patches");
			} else {
				SYSLOG("ngreen", "V52: TGL generation — skipping later-gen unaligned-store patches");
			}
		}

		SYSLOG("ngreen", "Loaded AppleIntelTGLGraphics! path=%s generation=%s",
			   vfActive ? "VF" : "physical",
			   tglGeneration ? "TGL native topology" :
			   "later-gen compatibility topology");
		// Adding a personality can start matching immediately. Publish only
		// after this payload's required routes and patches have been installed.
		const char *bundleId = activeKext == &kextG11HWTA ?
			"com.apple.driver.AppleIntelTGLGraphics" :
			"com.xxxxx.driver.AppleIntelTGLGraphics";
		gPendingAcceleratorBundle = bundleId;
		publishAcceleratorWhenReady();

		return true;
	}

    return false;
}

IOReturn TglAccel::vfGetHwCaps(void *that, void *input, void *output,
	uint64_t inputSize, uint64_t *outputSize)
{
	PANIC_COND(!callback || !callback->oVfGetHwCaps, "ngreen",
		"Missing native VF hardware-capability trampoline");
	const IOReturn result = FunctionCast(
		vfGetHwCaps, callback->oVfGetHwCaps)(
			that, input, output, inputSize, outputSize);
	if (result != kIOReturnSuccess)
		return result;

	constexpr uint64_t kHwCapsSize = 0x7C;
	constexpr uint32_t kTigerLakeVfIdentity = 0x9A498086U;
	constexpr uint32_t kCanonicalTigerLakeIdentity = 0x9A408086U;
	if (!output || !outputSize || *outputSize != kHwCapsSize) {
		SYSLOG("ngreen", "V335: rejected changed VF media hw-caps reply shape size=%llu",
		       outputSize ? *outputSize : 0ULL);
		return kIOReturnUnsupported;
	}

	auto *words = static_cast<uint32_t *>(output);
	if (words[2] != kTigerLakeVfIdentity) {
		SYSLOG("ngreen", "V335: rejected unexpected VF media hw-caps identity=%08x",
		       words[2]);
		return kIOReturnUnsupported;
	}
	if (checkKernelArgument("-ngiclva")) {
		words[2] = 0x8A528086U;
		SYSLOG("ngreen", "V374: exposed ICL media hw-caps identity 8a528086");
		return kIOReturnSuccess;
	}
	words[2] = kCanonicalTigerLakeIdentity;
	SYSLOG("ngreen", "V340: exposed canonical TGL media hw-caps identity 9a408086 for exact VF 9a498086");
	return kIOReturnSuccess;
}

IOReturn TglAccel::vfVideoGetIosurfaceAccelId(void *that, void *arguments,
	uint64_t *outputSize)
{
	PANIC_COND(!callback || !callback->oVfVideoGetIosurfaceAccelId, "ngreen",
		"Missing native VF video accelerator-id trampoline");
	SYSLOG("ngreen", "V337: media selector 0x100 enter");
	const IOReturn result = FunctionCast(
		vfVideoGetIosurfaceAccelId,
		callback->oVfVideoGetIosurfaceAccelId)(that, arguments, outputSize);
	const uint64_t observedOutputSize = outputSize ? *outputSize : 0;
	const bool readable = result == kIOReturnSuccess && arguments != nullptr;
	const uint32_t acceleratorId = readable ?
		*static_cast<const uint32_t *>(arguments) : 0;
	SYSLOG("ngreen", "V341: media selector 0x100 return result=0x%x output-size=%llu accel-id=%u readable=%u",
	       static_cast<unsigned int>(result), observedOutputSize, acceleratorId,
	       readable ? 1U : 0U);
	return result;
}

IOReturn TglAccel::vfVideoUpdatePerfCapability(void *that, void *arguments,
	uint64_t inputSize)
{
	PANIC_COND(!callback || !callback->oVfVideoUpdatePerfCapability, "ngreen",
		"Missing native VF video performance-capability trampoline");
	SYSLOG("ngreen", "V337: media selector 0x101 enter size=%llu", inputSize);
	const IOReturn result = FunctionCast(
		vfVideoUpdatePerfCapability,
		callback->oVfVideoUpdatePerfCapability)(that, arguments, inputSize);
	SYSLOG("ngreen", "V337: media selector 0x101 return result=0x%x",
	       static_cast<unsigned int>(result));
	return result;
}

IOReturn TglAccel::vfVideoSetContextPriority(void *that, void *arguments,
	uint64_t inputSize)
{
	PANIC_COND(!callback || !callback->oVfVideoSetContextPriority, "ngreen",
		"Missing native VF video context-priority trampoline");
	SYSLOG("ngreen", "V337: media selector 0x102 enter size=%llu", inputSize);
	const IOReturn result = FunctionCast(
		vfVideoSetContextPriority,
		callback->oVfVideoSetContextPriority)(that, arguments, inputSize);
	SYSLOG("ngreen", "V337: media selector 0x102 return result=0x%x",
	       static_cast<unsigned int>(result));
	return result;
}

bool TglAccel::IGAccelTaskIsKernelGPUTask(const void *that)
{
	const bool original = FunctionCast(IGAccelTaskIsKernelGPUTask,
	                                   callback->oIGAccelTaskIsKernelGPUTask)(that);
	const bool virtualDevice = vfIdentifyDevice() == VfIdentity::Virtual;
	if (original || !virtualDevice || that == nullptr)
		return original;

	void *task = const_cast<void *>(that);
	void *accelerator = getMember<void *>(task, 0x10);
	if (accelerator == nullptr)
		return original;

	void *kernelTask = getMember<void *>(accelerator, 0x150);
	if (NGVfSubmission::bootstrapKernelTask(original, virtualDevice, true, true,
	                                       kernelTask != nullptr)) {
		SYSLOG("ngreen", "V214: bootstrapping first VF task from Global GTT");
		return true;
	}

	return original;
}

IOReturn TglAccel::vfCommandQueueSubmit(void *that, const void *arguments)
{
	VfSlowSubmitLog slow("command queue submit");
	void *accelerator = that ? getMember<void *>(that, 0x5C0) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	slow.mark("guard");
	return FunctionCast(vfCommandQueueSubmit,
	                    callback->oVfCommandQueueSubmit)(that, arguments);
}

IOReturn TglAccel::vfContextSubmit(void *that, void *input, void *output,
	                            uint64_t options, uint64_t *stamp)
{
	VfSlowSubmitLog slow("context submit");
	void *accelerator = that ? getMember<void *>(that, 0x5A8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfContextSubmit, callback->oVfContextSubmit)(
		that, input, output, options, stamp);
}

IOReturn TglAccel::vf2DSetSurface(void *that, uint32_t surface, uint32_t mode)
{
	void *accelerator = that ? getMember<void *>(that, 0x5A8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vf2DSetSurface, callback->oVf2DSetSurface)(
		that, surface, mode);
}

IOReturn TglAccel::vf2DFinish(void *that, uint32_t options)
{
	void *accelerator = that ? getMember<void *>(that, 0x5A8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vf2DFinish, callback->oVf2DFinish)(that, options);
}

IOReturn TglAccel::vf2DBlit(void *that, void *command, uint64_t options)
{
	void *accelerator = that ? getMember<void *>(that, 0x5A8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vf2DBlit, callback->oVf2DBlit)(
		that, command, options);
}

IOReturn TglAccel::vfSurfaceExternalMethod(void *that, uint32_t selector,
	                                   void *arguments, void *dispatch,
	                                   void *target, void *reference)
{
	void *accelerator = that ? getMember<void *>(that, 0x12C8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfSurfaceExternalMethod,
	                    callback->oVfSurfaceExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
}

IOReturn TglAccel::vfSharedExternalMethod(void *that, uint32_t selector,
	                                  void *arguments, void *dispatch,
	                                  void *target, void *reference)
{
	void *accelerator = that ? getMember<void *>(that, 0xF8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	// Observe the resource-create boundary without reading user arguments or
	// changing native dispatch. Saturation bounds logging across all clients.
	static volatile UInt32 observedCreates = 0;
	UInt32 ticket = 0;
	if (selector == 0) {
		UInt32 used = observedCreates;
		while (used < 32) {
			if (OSCompareAndSwap(used, used + 1, &observedCreates)) {
				ticket = used + 1;
				break;
			}
			used = observedCreates;
		}
	}
	if (ticket)
		SYSLOG("ngreen", "V357: shared resource selector 0 enter ticket=%u", ticket);
	// Heap setup also invokes non-create resource methods. Observe only the
	// selector/result, with a separate bound; never inspect their arguments.
	static volatile UInt32 observedOtherMethods = 0;
	UInt32 otherTicket = 0;
	if (selector != 0) {
		UInt32 used = observedOtherMethods;
		while (used < 16) {
			if (OSCompareAndSwap(used, used + 1, &observedOtherMethods)) {
				otherTicket = used + 1;
				break;
			}
			used = observedOtherMethods;
		}
	}
	if (otherTicket)
		SYSLOG("ngreen", "V360: shared resource method enter ticket=%u selector=0x%x",
		       otherTicket, selector);
	const IOReturn result = FunctionCast(vfSharedExternalMethod,
	                    callback->oVfSharedExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
	if (ticket)
		SYSLOG("ngreen", "V357: shared resource selector 0 return ticket=%u result=0x%x",
		       ticket, static_cast<unsigned int>(result));
	if (otherTicket)
		SYSLOG("ngreen", "V360: shared resource method return ticket=%u selector=0x%x result=0x%x",
		       otherTicket, selector, static_cast<unsigned int>(result));
	if (selector != 0 && result != kIOReturnSuccess) {
		static volatile UInt32 observedOtherFailures = 0;
		UInt32 used = observedOtherFailures;
		while (used < 8) {
			if (OSCompareAndSwap(used, used + 1, &observedOtherFailures)) {
				SYSLOG("ngreen", "V360: shared resource method failed failure=%u selector=0x%x result=0x%x",
				       used + 1, selector, static_cast<unsigned int>(result));
				break;
			}
			used = observedOtherFailures;
		}
	}
	// Tahoe IOAccelResourceCreate copies native output +0x10/+0x18 into
	// its CF resource's client-shared RO/RW fields. Observe kernel-owned inline
	// output only after native success; never touch user input or descriptors.
	if (selector == 0 && result == kIOReturnSuccess && arguments) {
		static_assert(offsetof(IOExternalMethodArguments, structureOutput) == 0x58 &&
		              offsetof(IOExternalMethodArguments, structureOutputSize) == 0x60 &&
		              offsetof(IOExternalMethodArguments, structureOutputDescriptor) == 0x68,
		              "Tahoe external-method inline output ABI changed");
		const auto *args = static_cast<const IOExternalMethodArguments *>(arguments);
		const bool inlineShape = args->structureOutput && !args->structureOutputDescriptor &&
		                         args->structureOutputSize >= 0x58 && args->structureOutputSize <= 0x1000;
		uint64_t ro = 0, rw = 0;
		if (inlineShape) {
			memcpy(&ro, static_cast<const uint8_t *>(args->structureOutput) + 0x10, sizeof(ro));
			memcpy(&rw, static_cast<const uint8_t *>(args->structureOutput) + 0x18, sizeof(rw));
		}
		if (ticket)
			SYSLOG("ngreen", "V359: native resource output ticket=%u size=%u inline=%u ro-present=%u rw-present=%u",
			       ticket, args->structureOutputSize, inlineShape ? 1U : 0U, ro ? 1U : 0U, rw ? 1U : 0U);
		if (!inlineShape || !ro || !rw) {
			static volatile UInt32 observedOutputFailures = 0;
			UInt32 used = observedOutputFailures;
			while (used < 8) {
				if (OSCompareAndSwap(used, used + 1, &observedOutputFailures)) {
					SYSLOG("ngreen", "V359: native resource output anomaly failure=%u size=%u inline=%u ro-present=%u rw-present=%u",
					       used + 1, args->structureOutputSize, inlineShape ? 1U : 0U, ro ? 1U : 0U, rw ? 1U : 0U);
					break;
				}
				used = observedOutputFailures;
			}
		}
	}
	// Successful allocations may exhaust the ordinary trace before a later
	// allocation fails. Give failures an independent, saturating budget.
	if (selector == 0 && result != kIOReturnSuccess) {
		static volatile UInt32 observedFailures = 0;
		UInt32 used = observedFailures;
		while (used < 8) {
			if (OSCompareAndSwap(used, used + 1, &observedFailures)) {
				SYSLOG("ngreen", "V358: shared resource create failed failure=%u result=0x%x ordinary-budget-exhausted=%u",
				       used + 1, static_cast<unsigned int>(result), ticket ? 0U : 1U);
				break;
			}
			used = observedFailures;
		}
	}
	return result;
}

IOReturn TglAccel::vfGLContextExternalMethod(void *that, uint32_t selector,
	                                     void *arguments, void *dispatch,
	                                     void *target, void *reference)
{
	// Selectors below 0x100 use IOAccelContext2's inherited table.  Selector 2
	// is already leased at submit_data_buffers; leasing this forwarding wrapper
	// too would let close land between two gates in one transaction.
	const bool specialized = selector >= 0x100 && selector <= 0x105;
	void *accelerator = that ? getMember<void *>(that, 0x5A8) : nullptr;
	VfExternalProducerGuard guard(accelerator, specialized);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfGLContextExternalMethod,
	                    callback->oVfGLContextExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
}

IOReturn TglAccel::vfGLDrawableExternalMethod(void *that, uint32_t selector,
	                                      void *arguments, void *dispatch,
	                                      void *target, void *reference)
{
	void *accelerator = that ? getMember<void *>(that, 0xF8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfGLDrawableExternalMethod,
	                    callback->oVfGLDrawableExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
}

IOReturn TglAccel::vfSurfaceMtlExternalMethod(void *that, uint32_t selector,
	                                      void *arguments, void *dispatch,
	                                      void *target, void *reference)
{
	void *accelerator = that ? getMember<void *>(that, 0x2F8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfSurfaceMtlExternalMethod,
	                    callback->oVfSurfaceMtlExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
}

IOReturn TglAccel::vfMemoryInfoExternalMethod(void *that, uint32_t selector,
	                                      void *arguments, void *dispatch,
	                                      void *target, void *reference)
{
	void *accelerator = that ? getMember<void *>(that, 0xE0) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfMemoryInfoExternalMethod,
	                    callback->oVfMemoryInfoExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
}

IOReturn TglAccel::vfDisplayPipeExternalMethod(void *that, uint32_t selector,
	                                       void *arguments, void *dispatch,
	                                       void *target, void *reference)
{
	void *accelerator = that ? getMember<void *>(that, 0xD8) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfDisplayPipeExternalMethod,
	                    callback->oVfDisplayPipeExternalMethod)(
		that, selector, arguments, dispatch, target, reference);
}

IOReturn TglAccel::vfDisplayChangeHandler(void *that, void *reference,
	                                  void *framebuffer, int event,
	                                  void *argument)
{
	void *accelerator = that ? getMember<void *>(that, 0x88) : nullptr;
	VfExternalProducerGuard guard(accelerator);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfDisplayChangeHandler,
	                    callback->oVfDisplayChangeHandler)(
		that, reference, framebuffer, event, argument);
}

void TglAccel::vfGartCollector(void *that, IOInterruptEventSource *source,
	                        int count)
{
	VfExternalProducerGuard guard(that);
	if (!guard)
		return;
	FunctionCast(vfGartCollector, callback->oVfGartCollector)(
		that, source, count);
}

void TglAccel::vfFinalizeInterrupt(void *that,
	                            IOInterruptEventSource *source, int count)
{
	if (!vfTargetsPublishedAccelerator(that)) {
		FunctionCast(vfFinalizeInterrupt, callback->oVfFinalizeInterrupt)(
			that, source, count);
		return;
	}
	PANIC_COND(!callback || !callback->oVfFinalizeInterrupt, "ngreen",
		"Missing native accelerator finalize handler");
	auto phase = vfAcceleratorStopPhase();
	auto action = NGVfAcceleratorStop::finalizeAction(phase);
	SYSLOG("ngreen", "V317: VF finalizer entry phase=%u thread=%p",
	       NGVfAcceleratorStop::raw(phase),
	       reinterpret_cast<void *>(current_thread()));
	PANIC_COND(action == NGVfAcceleratorStop::FinalizeAction::Reject,
		"ngreen", "Invalid VF accelerator finalize entry phase %u",
		NGVfAcceleratorStop::raw(phase));
	if (action == NGVfAcceleratorStop::FinalizeAction::AlreadyOwned) {
		SYSLOG("ngreen", "V317: ignored duplicate VF finalizer phase=%u",
		       NGVfAcceleratorStop::raw(phase));
		return;
	}
	if (!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::Idle,
		    NGVfAcceleratorStop::Phase::Finalizing)) {
		phase = vfAcceleratorStopPhase();
		PANIC_COND(NGVfAcceleratorStop::finalizeAction(phase) ==
		           NGVfAcceleratorStop::FinalizeAction::Reject,
			"ngreen", "VF finalizer lost ownership to invalid phase %u",
			NGVfAcceleratorStop::raw(phase));
		return;
	}
	PANIC_COND(!vfCloseExternalProducerGateAndWait(), "ngreen",
		"Cannot drain VF external producers before accelerator finalize");
	{
		VfDeviceCacheRetirementScope retirement(that);
		SYSLOG("ngreen", "V317: entered sole VF cache-finalization owner");
		FunctionCast(vfFinalizeInterrupt, callback->oVfFinalizeInterrupt)(
			that, source, count);
	}
	phase = vfAcceleratorStopPhase();
	const auto returned = NGVfAcceleratorStop::ownerReturnAction(phase, false);
	if (returned == NGVfAcceleratorStop::OwnerReturnAction::PublishFinalized) {
		PANIC_COND(!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::Finalizing,
		    NGVfAcceleratorStop::Phase::Finalized), "ngreen",
			"Cannot publish completed VF cache finalization");
		SYSLOG("ngreen", "V317: cache finalization awaits native stop");
		return;
	}
	PANIC_COND(returned !=
	           NGVfAcceleratorStop::OwnerReturnAction::AlreadyComplete,
		"ngreen", "VF finalizer returned in invalid stop phase %u",
		NGVfAcceleratorStop::raw(phase));
	SYSLOG("ngreen", "V317: synchronous finalize re-entry completed native VF stop");
}

void TglAccel::vfDeviceCacheControl(void *that, void *cache,
	                             uint32_t selector, uint64_t argument0,
	                             uint64_t argument1)
{
	OSSynchronizeIO();
	const bool nestedRetirement = (selector == 3 || selector == 4) &&
	                              vfOwnsDeviceCacheRetirement();
	VfExternalProducerGuard guard(that, !nestedRetirement);
	if (!guard)
		return;
	FunctionCast(vfDeviceCacheControl, callback->oVfDeviceCacheControl)(
		that, cache, selector, argument0, argument1);
}

void TglAccel::vfEmitFirstFlushEvents(void *that)
{
	VfExternalProducerGuard guard(that);
	if (!guard)
		return;
	FunctionCast(vfEmitFirstFlushEvents,
	             callback->oVfEmitFirstFlushEvents)(that);
}

IOReturn TglAccel::vfDisplaySleepCallback(void *that, uint32_t command,
	                                  uint32_t argument0,
	                                  uint32_t argument1)
{
	VfExternalProducerGuard guard(that);
	if (!guard)
		return kIOReturnOffline;
	return FunctionCast(vfDisplaySleepCallback,
	                    callback->oVfDisplaySleepCallback)(
		that, command, argument0, argument1);
}

// -ngtglearly: keep the GPU's IOService busy from patcher setup until the
// accelerator has started. WindowServer waits for IOKit quiet (IOKitWaitQuiet,
// 60 s) before it enumerates Metal devices and picks its compositor.
static IOService *volatile gTglBusyHold;

void ngHoldTglBusy(IOService *service) {
	if (!service || !OSCompareAndSwapPtr(nullptr, service, reinterpret_cast<void *volatile *>(&gTglBusyHold)))
		return;
	service->retain();
	service->adjustBusy(1);
	SYSLOG("ngreen", "V289: holding IOKit busy until the accelerator starts");
	thread_t thread = nullptr;
	if (kernel_thread_start([](void *, wait_result_t) {
		    IOSleep(40000);
		    ngReleaseTglBusyHold();
		    thread_terminate(current_thread());
	    }, nullptr, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
}

void ngReleaseTglBusyHold() {
	IOService *service = gTglBusyHold;
	if (!service || !OSCompareAndSwapPtr(service, nullptr, reinterpret_cast<void *volatile *>(&gTglBusyHold)))
		return;
	service->adjustBusy(-1);
	service->release();
	SYSLOG("ngreen", "V289: released IOKit busy hold");
}

// Boot-arg cache for the per-submission RCS prologue switch.

bool TglAccel::start(void *that, void *provider)
{
	if (!that || !provider || !callback || !callback->ostart) {
		SYSLOG("ngreen", "V239: refusing accelerator start with incomplete IOService ABI");
		return false;
	}
	// An SR-IOV VF owns neither force-wake nor legacy execlist MMIO. Establish
	// the GuC VF ABI and its assigned GGTT range before native scheduler setup.
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Invalid) {
		SYSLOG("ngreen", "V239: refusing accelerator start with invalid PCI identity");
		return false;
	}
	const bool vfActive = identity == VfIdentity::Virtual;
	auto *pciDevice = vfActive ? OSDynamicCast(
		IOPCIDevice, reinterpret_cast<OSObject *>(provider)) : nullptr;
	if (vfActive) {
		if (!pciDevice) {
			vfMarkProtocolFault("accelerator provider is not an IOPCIDevice");
			return false;
		}
		NGreen::callback->closeVfBusMasterAdmission();
		PANIC_COND(NGreen::callback->vfBusMasterAdmissionOpen(), "ngreen",
			"Cannot close stale VF PCI Bus Master admission before native start");
		// NootedGreen used to enable Bus Master unconditionally during its
		// processPatcher() setup.  That made a VF DMA-capable before the initial
		// PPGTT, HWS mappings, interrupt consumer or GuC transport existed.  Close
		// the command bit again at the stable accelerator/provider boundary and
		// fail before any native allocation if VFIO did not reflect the write.
		pciDevice->setBusMasterEnable(false);
		OSSynchronizeIO();
		if ((pciDevice->configRead16(kIOPCIConfigCommand) &
		     kIOPCICommandBusMaster) != 0) {
			vfMarkProtocolFault(
				"VF PCI bus mastering remained enabled before native start");
			return false;
		}
		SYSLOG("ngreen", "V314: held VF PCI Bus Master off before native accelerator start");
	}
	if (vfActive) {
		OSSynchronizeIO();
		if (gVfExternalProducerGate != 0 ||
		    vfAcceleratorStopPhase() != NGVfAcceleratorStop::Phase::Idle) {
			vfMarkProtocolFault(
				"VF accelerator start found reused teardown state");
			return false;
		}
		// start() is the first stable point where the exact IntelAccelerator
		// instance is available. Publish it before native context/pool creation;
		// the System-KC wrapper compares this owner and leaves every other pool
		// on the native PF path.
		gVfAccelerator = that;
		OSSynchronizeIO();
	}
	if (vfActive) {
		// utilGetProperty<unsigned> lets this OSData override the mandatory
		// accelerator property after publication. Do not invoke its string
		// parser or mutate global options to resolve a VF conflict.
		auto *options = IORegistryEntry::fromPath("IODeviceTree:/options");
		auto *overrideProperty = options ?
			options->copyProperty("GraphicsSchedulerSelect") : nullptr;
		const bool hasSchedulerOverride = OSDynamicCast(OSData, overrideProperty);
		OSSafeReleaseNULL(overrideProperty);
		OSSafeReleaseNULL(options);
		if (hasSchedulerOverride) {
			vfMarkProtocolFault("VF rejects late device-tree scheduler override");
			return false;
		}
	}
	// Native start overrides GraphicsSchedulerSelect with scheduler 5 when
	// this boot argument is present. A VF cannot enter that physical path.
	int firmwareDisableArgument = 0;
	if (vfActive && PE_parse_boot_argn("-disablegfxfirmware",
	                                  &firmwareDisableArgument,
	                                  sizeof(firmwareDisableArgument))) {
		vfMarkProtocolFault("VF cannot disable mandatory GuC firmware scheduling");
		return false;
	}
	if (vfActive && (getMember<uint8_t>(that, 0x1190) &
	                 kVfLegacyPageOwnershipFlag)) {
		vfMarkProtocolFault(
			"VF exposes unsupported legacy page-ownership/async-slice mode");
		return false;
	}
	if (vfActive && !vfBootstrapDirectGgtt()) {
		vfMarkProtocolFault("VF bootstrap failed before native accelerator start");
		return false;
	}
	if (vfActive) {
		// The UUID-pinned TGL driver predates IOPCIDevice::configureInterrupts
		// and asks getInterruptType() while creating its local filter source.
		// Quiesce Gen11 master/source state before native object construction, but
		// do not allocate MSI here: Tahoe's configureInterrupts itself enables PCI
		// Bus Master.  The routed engine-start boundary performs that allocation
		// only after every native HWS mapping has been validated.
		if (!vfQuiesceVirtualInterruptsBeforeMsi())
			return false;
		SYSLOG("ngreen", "V320: deferred VF PCI MSI allocation until native HWS validation");
	}

	auto *service = static_cast<IOService *>(that);
	int scheduler = vfActive ? 4 : (NGreen::callback->isRealTGL ? 3 : 5);
	int requestedScheduler = 0;
	if (PE_parse_boot_argn("ngreenSched", &requestedScheduler,
	                       sizeof(requestedScheduler)) &&
	    requestedScheduler >= 3 && requestedScheduler <= 5) {
		scheduler = requestedScheduler;
	} else {
		auto *providerService =
			OSDynamicCast(IOService, reinterpret_cast<OSObject *>(provider));
		auto *configured = providerService ?
			OSDynamicCast(OSNumber, providerService->getProperty("SchedulerType")) :
			nullptr;
		if (configured) {
			const int value = static_cast<int>(configured->unsigned32BitValue());
			if (value >= 3 && value <= 5)
				scheduler = value;
		}
	}

	// A VF supports only the translated reference GuC scheduler. Host fallback
	// would enter PF-owned execlist and power-management register paths.
	if (vfActive)
		scheduler = 4;
	auto *schedulerNumber =
		OSNumber::withNumber(static_cast<unsigned long long>(scheduler), 32);
	const bool schedulerPublished =
		schedulerNumber && service->setProperty("GraphicsSchedulerSelect",
		                                       schedulerNumber);
	OSSafeReleaseNULL(schedulerNumber);
	if (vfActive && !schedulerPublished) {
		vfMarkProtocolFault("failed to publish mandatory VF scheduler selection");
		return false;
	}
	SYSLOG("ngreen", "Accelerator scheduler=%d path=%s", scheduler,
	       vfActive ? "VF GuC" : "physical");

	if (vfActive) {
		auto *zero = OSNumber::withNumber(0ULL, 32);
		auto *one = OSNumber::withNumber(1ULL, 32);
		const bool pmDisabled =
			zero && service->setProperty("SchedPmNotifyEnable", zero);
		const bool fallbackDisabled =
			zero && service->setProperty("SchedulerFallbackOnFirmwareFail", zero);
		const bool telemetryDisabled =
			one && service->setProperty("TelemetryDisable", one);
		// V372: the TGL personality sets EnableMultiContext=1, which puts every
		// compute context on CCS0. Official i915-sriov-dkms (>= 2025.12.10.1)
		// does not expose CCS on Xe_LP by default, and GuC then resets or
		// rejects those contexts. CCS shares RCS's slice anyway; keep compute
		// on RCS so the VF works with or without host xelp_enable_ccs.
		const bool multiContextDisabled =
			zero && service->setProperty("EnableMultiContext", zero);
		OSSafeReleaseNULL(zero);
		OSSafeReleaseNULL(one);
		if (!pmDisabled || !fallbackDisabled || !telemetryDisabled || !multiContextDisabled) {
			vfMarkProtocolFault(
				"failed to disable PF-owned VF scheduler/telemetry paths");
			return false;
		}
	}

	// V368: kernelmanagerd may have loaded and matched TGL itself, so its start
	// can come before IOPCIFamily and IOAcceleratorFamily2 are processed (by
	// Lilu or, if Lilu missed them, by recoverMissedKexts a few seconds later).
	if (vfActive) {
		uint32_t waited = 0;
		while ((!callback->ioPciConfigureInterrupts || !gIoAccelLifecycleResolved) && waited < 20000)
			IOSleep(10), waited += 10;
		if (waited)
			SYSLOG("ngreen", "V368: start waited %u ms for IOPCIFamily/IOAcceleratorFamily2 processing", waited);
	}
	if (vfActive)
		SYSLOG("ngreen", "V312: entering exact native IntelAccelerator start");
	const auto result = FunctionCast(start, callback->ostart)(that, provider);
	ngReleaseTglBusyHold();
	if (vfActive)
		SYSLOG("ngreen", "V312: exact native IntelAccelerator start returned %d",
		       result);
	if (!result) {
		if (vfActive) {
			// The pinned Tahoe start body normally routes failures after
			// startGraphicsEngine() through its virtual stop entry.  Its 0x215
			// DPSM-timer allocation failure is the exception: that edge returns
			// false after the VF GuC/CTB and accelerator lifecycle are live, but
			// never calls stop.  Close that exact transactional hole here while
			// retirement transport is still trusted.  acceleratorStop() sets the
			// device-stopping boundary before native finishAllStamps and reaches
			// our stopGraphicsEngine() quiescence route; use a null provider just
			// like Tahoe's other start-failure cleanup edge.
			OSSynchronizeIO();
			if (gVfSchedulerFirmwareReady && !gVfDeviceStopping) {
				SYSLOG("ngreen", "V252: rolling back live VF engine after native accelerator start failure");
				acceleratorStop(that, nullptr);
				OSSynchronizeIO();
				PANIC_COND(!gVfDeviceStopping || !gVfDmaQuiesced, "ngreen",
					"Native VF start failure escaped without a DMA-quiesced stop");
			}
			vfMarkProtocolFault("native accelerator start failed after VF bootstrap");
		}
		return result;
	}
	if (vfActive) {
		PANIC_COND(!vfNativeCallbackBindingsReady(that), "ngreen",
			"Native VF start published incomplete timer/event-source ownership");
		SYSLOG("ngreen", "V301: verified VF DPSM/periodic/fallback source bindings before publication");
	}

	// The accelerator personality is injected before this wrapper runs.  V323
	// removes the UUID-pinned native synchronous publication, so this is the
	// only VF publication and matching cannot observe partial callback state.
	service->registerService(kIOServiceAsynchronous);
	return result;
}

void TglAccel::acceleratorStop(void *that, void *provider)
{
	if (!vfTargetsPublishedAccelerator(that)) {
		FunctionCast(acceleratorStop, callback->oAcceleratorStop)(that, provider);
		return;
	}
	PANIC_COND(!callback || !callback->oAcceleratorStop ||
	           !callback->oVfFinalizeInterrupt, "ngreen",
		"Missing native VF accelerator lifecycle trampoline");

	// One helper owns the only transition through native stop. It runs inside
	// the thread-affine retirement extent established by the finalizer owner (or
	// creates that extent when resuming a completed asynchronous finalizer).
	const auto runNativeStop = [&]() {
		PANIC_COND(vfAcceleratorStopPhase() !=
		           NGVfAcceleratorStop::Phase::NativeStopActive,
			"ngreen", "Native VF stop entered outside its active phase");
		PANIC_COND(!NGVfIrqGate::closed(gVfExternalProducerGate) ||
		           !NGVfIrqGate::drained(gVfExternalProducerGate),
			"ngreen", "Native VF stop reached before producer closure/drain");
		PANIC_COND(!OSCompareAndSwap(0, 1, &gVfDeviceStopping), "ngreen",
			"Duplicate VF device-stopping publication");
		PANIC_COND(gVfStopMemoryManager ||
		           gVfStopMemoryManagerAccelerator ||
		           gVfStopMemoryManagerThread,
			"ngreen", "Native VF stop found a stale memory-manager hold");
		auto *stopMemoryManager =
			getMember<OSObject *>(that, 0x1260);
		if (stopMemoryManager) {
			stopMemoryManager->retain();
			gVfStopMemoryManager = stopMemoryManager;
			gVfStopMemoryManagerAccelerator = that;
			gVfStopMemoryManagerThread = IOThreadSelf();
		}
		OSSynchronizeIO();
		SYSLOG("ngreen", "V319: entering exactly-once native VF stop phase thread=%p manager-held=%u",
		       reinterpret_cast<void *>(current_thread()),
		       static_cast<unsigned int>(stopMemoryManager != nullptr));
		{
			VfDeviceCacheRetirementScope retirement(that);
			FunctionCast(acceleratorStop,
			             callback->oAcceleratorStop)(that, provider);
		}
		OSSynchronizeIO();
		if (stopMemoryManager) {
			if (gVfStopMemoryManager) {
				// A null-provider native rollback deliberately skips inherited
				// base stop.  Native Intel stop has nevertheless dropped and
				// cleared its field owner, so release our sole retained hold now.
				PANIC_COND(gVfStopMemoryManager != stopMemoryManager ||
				           gVfStopMemoryManagerAccelerator != that ||
				           gVfStopMemoryManagerThread != IOThreadSelf() ||
				           getMember<void *>(that, 0x1260) != nullptr,
					"ngreen", "Native VF stop returned with inconsistent memory-manager hold");
				PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
					"Cannot close skipped VF base-stop memory-manager hold");
				IORecursiveLockLock(gVfPageTableUpdateLock);
				// Restore the native release-before-clear invariant for the
				// deferred final reference as well.  The original release could
				// not call free while our extra retain was outstanding.
				getMember<OSObject *>(that, 0x1260) = stopMemoryManager;
				OSSynchronizeIO();
				stopMemoryManager->release();
				getMember<OSObject *>(that, 0x1260) = nullptr;
				gVfStopMemoryManager = nullptr;
				gVfStopMemoryManagerAccelerator = nullptr;
				gVfStopMemoryManagerThread = nullptr;
				OSSynchronizeIO();
				IORecursiveLockUnlock(gVfPageTableUpdateLock);
				SYSLOG("ngreen", "V319: released memory-manager hold after null-provider native stop");
			} else {
				PANIC_COND(gVfStopMemoryManagerAccelerator ||
				           gVfStopMemoryManagerThread,
					"ngreen", "Inherited VF base stop left partial memory-manager hold state");
			}
		}
		PANIC_COND(getMember<IOTimerEventSource *>(that, 0x1460) != nullptr,
			"ngreen", "Native VF stop returned with a live DPSM timer owner");
		if (!provider) {
			PANIC_COND(!NGVfIrqGate::closed(gVfExternalProducerGate) ||
			           !NGVfIrqGate::drained(gVfExternalProducerGate) ||
			           !vfDetachPartialNativeCallbackSources(that),
				"ngreen", "Cannot drain partial VF callback owners after null-provider stop");
			SYSLOG("ngreen", "V301: detached partial VF callback bindings after null-provider stop");
		}
		PANIC_COND(!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::NativeStopActive,
		    NGVfAcceleratorStop::Phase::NativeStopComplete), "ngreen",
			"Cannot publish completed native VF stop");
		SYSLOG("ngreen", "V319: completed exactly-once native VF stop");
	};

	auto phase = vfAcceleratorStopPhase();
	auto action = NGVfAcceleratorStop::stopAction(
		phase, vfOwnsDeviceCacheRetirement());
	SYSLOG("ngreen", "V317: VF stop entry phase=%u owner=%u thread=%p",
	       NGVfAcceleratorStop::raw(phase),
	       static_cast<unsigned int>(vfOwnsDeviceCacheRetirement()),
	       reinterpret_cast<void *>(current_thread()));
	PANIC_COND(action == NGVfAcceleratorStop::StopAction::Reject,
		"ngreen", "Concurrent or recursive VF accelerator stop phase=%u owner=%u",
		NGVfAcceleratorStop::raw(phase), vfOwnsDeviceCacheRetirement());
	if (action == NGVfAcceleratorStop::StopAction::AlreadyComplete) {
		SYSLOG("ngreen", "V317: ignored duplicate completed VF stop");
		return;
	}

	if (action == NGVfAcceleratorStop::StopAction::BeginFinalization) {
		PANIC_COND(!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::Idle,
		    NGVfAcceleratorStop::Phase::Finalizing), "ngreen",
			"Cannot claim VF accelerator stop lifecycle");
		PANIC_COND(!vfCloseExternalProducerGateAndWait(), "ngreen",
			"Cannot drain VF external producers before accelerator stop");
		// The one-shot Tahoe finalizer retires IOSurface caches, then may
		// synchronously tail-call IOService::finalize and re-enter this wrapper.
		// Keep this retirement owner live across that complete nested native stop.
		VfDeviceCacheRetirementScope retirement(that);
		SYSLOG("ngreen", "V317: stop owner entered sole VF cache finalizer");
		FunctionCast(vfFinalizeInterrupt,
		             callback->oVfFinalizeInterrupt)(that, nullptr, 0);
		phase = vfAcceleratorStopPhase();
		const auto returned = NGVfAcceleratorStop::ownerReturnAction(phase, true);
		if (returned ==
		    NGVfAcceleratorStop::OwnerReturnAction::AlreadyComplete) {
			SYSLOG("ngreen", "V317: nested IOService finalize completed VF stop");
			return;
		}
		PANIC_COND(returned !=
		           NGVfAcceleratorStop::OwnerReturnAction::RunNativeStop,
			"ngreen", "VF stop finalizer returned in invalid phase %u",
			NGVfAcceleratorStop::raw(phase));
		PANIC_COND(!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::Finalizing,
		    NGVfAcceleratorStop::Phase::NativeStopActive), "ngreen",
			"Cannot claim fallback native VF stop");
		runNativeStop();
		return;
	}

	if (phase == NGVfAcceleratorStop::Phase::Finalizing) {
		PANIC_COND(!vfOwnsDeviceCacheRetirement(), "ngreen",
			"Cross-thread VF stop collided with active cache finalizer");
		PANIC_COND(!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::Finalizing,
		    NGVfAcceleratorStop::Phase::NativeStopActive), "ngreen",
			"Cannot claim nested native VF stop");
	} else {
		PANIC_COND(phase != NGVfAcceleratorStop::Phase::Finalized, "ngreen",
			"Unexpected resumable VF stop phase %u",
			NGVfAcceleratorStop::raw(phase));
		PANIC_COND(!NGVfIrqGate::closed(gVfExternalProducerGate) ||
		           !NGVfIrqGate::drained(gVfExternalProducerGate),
			"ngreen", "Finalized VF cache owner left producers undrained");
		PANIC_COND(!vfTransitionAcceleratorStop(
		    NGVfAcceleratorStop::Phase::Finalized,
		    NGVfAcceleratorStop::Phase::NativeStopActive), "ngreen",
			"Cannot resume native VF stop after cache finalization");
	}
	runNativeStop();
}

void TglAccel::vfBaseAcceleratorStop(void *that, void *provider)
{
	PANIC_COND(!callback || !callback->oVfBaseAcceleratorStop,
		"ngreen", "Missing native IOAccelerator stop trampoline");
	OSObject *stopMemoryManager = nullptr;
	bool stopMemoryManagerPublished = false;
	if (gVfIdentity == VfIdentity::Virtual && that == gVfAccelerator) {
		OSSynchronizeIO();
		if (gVfDeviceStopping) {
			PANIC_COND(!NGVfIrqGate::closed(gVfExternalProducerGate) ||
			           !NGVfIrqGate::drained(gVfExternalProducerGate),
				"ngreen", "Base VF stop reached before producer closure/drain");
			PANIC_COND(!vfDetachNativeCallbackSourcesBeforeBaseStop(that),
				"ngreen", "Cannot drain VF periodic/fallback callback owners before workloop teardown");
			SYSLOG("ngreen", "V301: synchronously detached VF periodic/fallback sources before base workloop teardown");
			stopMemoryManager = gVfStopMemoryManager;
			PANIC_COND((stopMemoryManager == nullptr) !=
			           (gVfStopMemoryManagerAccelerator == nullptr) ||
			           (stopMemoryManager == nullptr) !=
			           (gVfStopMemoryManagerThread == nullptr),
				"ngreen", "VF base stop observed partial memory-manager hold state");
			PANIC_COND(gVfCtbEverEnabled && !stopMemoryManager,
				"ngreen", "Live VF base stop has no retained memory manager");
			if (stopMemoryManager) {
				PANIC_COND(gVfStopMemoryManagerAccelerator != that ||
				           gVfStopMemoryManagerThread != IOThreadSelf() ||
				           getMember<void *>(that, 0x1260) != nullptr,
					"ngreen", "VF base stop cannot republish retained memory manager");
				PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
					"VF base stop cannot serialize memory-manager lifetime");
				IORecursiveLockLock(gVfPageTableUpdateLock);
				PANIC_COND(gVfStopMemoryManager != stopMemoryManager ||
				           gVfStopMemoryManagerAccelerator != that ||
				           gVfStopMemoryManagerThread != IOThreadSelf() ||
				           getMember<void *>(that, 0x1260) != nullptr,
					"ngreen", "VF base stop lost memory-manager ownership before publication");
				getMember<OSObject *>(that, 0x1260) = stopMemoryManager;
				stopMemoryManagerPublished = true;
				OSSynchronizeIO();
				// Do not carry the page-table lock into inherited base stop: its
				// accelerator-mutex acquisition is the opposite order used by
				// ordinary mapping callers.  The retained object and closed producer
				// gate keep the publication valid; each nested mapping transaction
				// takes this lock independently.
				IORecursiveLockUnlock(gVfPageTableUpdateLock);
				SYSLOG("ngreen", "V319: republished retained memory manager for inherited VF base stop");
			}
		} else {
			PANIC_COND(gVfSchedulerFirmwareReady || gVfCtbEverEnabled ||
			           gVfExternalProducerGate != 0 ||
			           !vfDetachPartialNativeCallbackSources(that),
				"ngreen", "Unsafe unpublished VF start rollback reached base workloop teardown");
			SYSLOG("ngreen", "V301: detached zero-use partial VF callback bindings during native start rollback");
		}
	}
	FunctionCast(vfBaseAcceleratorStop,
	             callback->oVfBaseAcceleratorStop)(that, provider);
	if (stopMemoryManagerPublished) {
		// Re-enter after Apple has dropped its accelerator locks.  This waits
		// for every synchronous mapping/PTE transaction started by base stop
		// before the retained manager is finally destroyed.
		IORecursiveLockLock(gVfPageTableUpdateLock);
		OSSynchronizeIO();
		PANIC_COND(getMember<OSObject *>(that, 0x1260) != stopMemoryManager ||
		           gVfStopMemoryManager != stopMemoryManager ||
		           gVfStopMemoryManagerAccelerator != that ||
		           gVfStopMemoryManagerThread != IOThreadSelf(),
			"ngreen", "Inherited VF base stop changed retained memory-manager ownership");
		// Preserve Intel's native manager release-before-field-clear ABI.  Base
		// task/mapping destruction has completed synchronously while the retained
		// manager was published, so this final release now owns manager free.
		stopMemoryManager->release();
		getMember<OSObject *>(that, 0x1260) = nullptr;
		gVfStopMemoryManager = nullptr;
		gVfStopMemoryManagerAccelerator = nullptr;
		gVfStopMemoryManagerThread = nullptr;
		OSSynchronizeIO();
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		SYSLOG("ngreen", "V319: completed inherited VF mapping release before memory-manager free");
	}
}

int TglAccel::vfEventMachineFinishAllStamps(void *that)
{
	PANIC_COND(!callback || !callback->oVfEventMachineFinishAllStamps,
		"ngreen", "Missing native event-machine finish trampoline");
	const bool vfOwner = gVfIdentity == VfIdentity::Virtual &&
		gVfAccelerator && that == getMember<void *>(gVfAccelerator, 0x380);
	if (vfOwner)
		SYSLOG("ngreen", "V318: inherited VF finishAllStamps enter");
	const int result = FunctionCast(vfEventMachineFinishAllStamps,
		callback->oVfEventMachineFinishAllStamps)(that);
	if (vfOwner)
		SYSLOG("ngreen", "V318: inherited VF finishAllStamps return=%d", result);
	return result;
}

void TglAccel::vfEventMachineStop(void *that)
{
	PANIC_COND(!callback || !callback->oVfEventMachineStop,
		"ngreen", "Missing native event-machine stop trampoline");
	const bool vfOwner = gVfIdentity == VfIdentity::Virtual &&
		gVfAccelerator && that == getMember<void *>(gVfAccelerator, 0x380);
	if (vfOwner)
		SYSLOG("ngreen", "V318: inherited VF event-machine stop enter");
	FunctionCast(vfEventMachineStop, callback->oVfEventMachineStop)(that);
	if (vfOwner)
		SYSLOG("ngreen", "V318: inherited VF event-machine stop return");
}

uint32_t TglAccel::vfTelemetryPrintDashboard(void *that, uint64_t options)
{
	(void)that;
	(void)options;
	return 0;
}

int TglAccel::vfTelemetryInitWithAccelerator(void *that, void *accelerator,
	                                      uint32_t options)
{
	if (!that || !accelerator)
		return static_cast<int>(kIOReturnUnsupported);

	// Exact Tahoe IGTelemetryManager layout. telemetryCreateManager has already
	// constructed the hash table and zeroed the embedded IGSupportMDAPI object.
	// Publish only the software ownership fields consumed by teardown/statistics;
	// leave both OA reference counters and all mapped-buffer pointers zero. The
	// native destructor's finalizeOaBuffer() zero-counter fast path then performs
	// no register access and no release of fabricated storage.
	getMember<void *>(that, 0x258) = accelerator;
	getMember<void *>(that, 0x2D8) = accelerator;
	getMember<void *>(that, 0x228) = getMember<void *>(accelerator, 0xE00);
	getMember<uint32_t>(that, 0x2A0) = 0;
	getMember<uint32_t>(that, 0x10) = 1;
	getMember<uint32_t>(that, 0x0C) = options;
	getMember<uint64_t>(that, 0x230) = 1;
	getMember<uint32_t>(that, 0x30C) = 0;
	getMember<uint32_t>(that, 0x310) = 0;
	return 0;
}

void TglAccel::vfTelemetryRetain(void *that)
{
	(void)that;
}

void TglAccel::vfTelemetryRelease(void *that)
{
	(void)that;
}

uint64_t TglAccel::vfTelemetryCalcGlobalUsage(void *that, uint64_t timestamp)
{
	(void)that;
	(void)timestamp;
	return 0;
}

int64_t TglAccel::vfTelemetryOperation(void *that, uint64_t selector,
	                                int64_t value, void *operation,
	                                void *connection, void *task)
{
	(void)that;
	(void)selector;
	(void)value;
	(void)operation;
	(void)connection;
	(void)task;
	return static_cast<int64_t>(static_cast<int32_t>(kIOReturnUnsupported));
}

void TglAccel::vfTelemetryPatchContextImage(void *that, void *contextImage)
{
	(void)that;
	(void)contextImage;
}

void TglAccel::vfTelemetryOnConnectionStop(void *that, void *connection,
	                                     void *task)
{
	(void)that;
	(void)task;
	if (!connection)
		return;
	auto *flags = static_cast<uint8_t *>(connection);
	flags[0x08] = 0;
	flags[0x09] = 0;
	flags[0x0A] = 0;
}

IOReturn TglAccel::vfTelemetryInitOaBuffer(void *that, void *connection,
	                                    void *input, void *output,
	                                    uint64_t inputSize,
	                                    uint64_t *outputSize)
{
	(void)that;
	(void)connection;
	(void)input;
	(void)output;
	(void)inputSize;
	(void)outputSize;
	return kIOReturnUnsupported;
}

IOReturn TglAccel::vfTelemetryReadOaBuffer(void *that, void *input,
	                                    void *output, uint64_t *outputSize,
	                                    void *connection)
{
	(void)that;
	(void)input;
	(void)output;
	(void)outputSize;
	(void)connection;
	return kIOReturnUnsupported;
}

IOReturn TglAccel::vfTelemetryMapOaBufferMemory(void *that, void *input,
	                                         void *output, uint64_t inputSize,
	                                         uint64_t *outputSize, void *task)
{
	(void)that;
	(void)input;
	(void)output;
	(void)inputSize;
	(void)outputSize;
	(void)task;
	return kIOReturnUnsupported;
}

void TglAccel::vfTelemetryUsageAlloc(void *that)
{
	if (!that)
		return;
	// Preserve initWithAcceleratorAndStampIndex()'s supported allocation-failure
	// state without paying for 64 per-stamp GPU and metadata buffers on a VF.
	getMember<void *>(that, 0x68) = nullptr;
	getMember<void *>(that, 0x70) = nullptr;
	getMember<uint32_t>(that, 0x78) = 0;
	getMember<uint64_t>(that, 0x80) = 0;
	getMember<void *>(that, 0x88) = nullptr;
	getMember<uint32_t>(that, 0x90) = 0;
}

void TglAccel::vfTelemetryUsageLog(void *that, void *context, uint64_t timestamp)
{
	(void)that;
	(void)context;
	(void)timestamp;
}

void TglAccel::vfTelemetryUsageReportGlobal(void *that)
{
	(void)that;
}

bool TglAccel::vfTelemetryUsageStartSample(void *that, void *ring,
	                                     uint32_t stamp, uint32_t engine,
	                                     uint64_t commandId,
	                                     uint64_t submitTime,
	                                     uint64_t startTime,
	                                     uint64_t endTime)
{
	(void)that;
	(void)ring;
	(void)stamp;
	(void)engine;
	(void)commandId;
	(void)submitTime;
	(void)startTime;
	(void)endTime;
	return false;
}

void TglAccel::vfTelemetryUsageStopSample(void *that, void *ring,
	                                   uint32_t stamp, uint32_t engine)
{
	(void)that;
	(void)ring;
	(void)stamp;
	(void)engine;
}

void TglAccel::vfTelemetryUsageFrameCalc(void *that, void *accelerator,
	                                   uint32_t frame)
{
	(void)that;
	(void)accelerator;
	(void)frame;
}

void TglAccel::vfDisableDebugSysctl(void *that)
{
	(void)that;
}

IOReturn TglAccel::vfRejectPavpCommandCallback(void *that, uint32_t command,
	                                         uint32_t session, uint32_t *data,
	                                         bool recovery)
{
	(void)that;
	(void)command;
	(void)session;
	(void)data;
	(void)recovery;
	return kIOReturnUnsupported;
}

void TglAccel::vfIgnoreTraceRecognizeFlip(void *that)
{
	(void)that;
}

void *TglAccel::vfRejectPhysicalFence(void *that,
	                               const NGIGAddressRange &range,
	                               uint64_t pitch, uint32_t tileMode)
{
	(void)that;
	(void)range;
	(void)pitch;
	(void)tileMode;
	return nullptr;
}

void TglAccel::vfDisableEdramProbe(void *that)
{
	if (!that)
		return;
	getMember<uint8_t>(that, 0x20) = 0;
	getMember<uint8_t>(that, 0x21) = 0;
	SYSLOG("ngreen", "V321: suppressed independent TGL eDRAM probe");
}

static UInt32 vfTakeTaskConstructionDiagnosticStage()
{
	UInt32 stage = gVfTaskConstructionDiagnosticStages;
	while (stage < 9) {
		const UInt32 next = stage + 1;
		if (OSCompareAndSwap(
		        stage, next, &gVfTaskConstructionDiagnosticStages)) {
			stage = next;
			break;
		}
		stage = gVfTaskConstructionDiagnosticStages;
	}
	return stage > 8 ? 0 : stage;
}

bool TglAccel::vfInitStampAndScratchPages(void *that)
{
	PANIC_COND(!that || !callback || !callback->oVfInitStampAndScratchPages,
		"ngreen", "Missing native VF stamp/scratch construction boundary");
	const UInt32 stage = gVfIdentity == VfIdentity::Virtual ?
		vfTakeTaskConstructionDiagnosticStage() : 0;
	if (stage != 0)
		SYSLOG("ngreen", "V316: task stage=%d stamp-scratch enter task=%p",
		       static_cast<int>(stage), that);
	const bool initialized = FunctionCast(
		vfInitStampAndScratchPages,
		callback->oVfInitStampAndScratchPages)(that);
	if (stage != 0)
		SYSLOG("ngreen", "V316: task stage=%d stamp-scratch return=%d scratch=%p stamp=%p",
		       static_cast<int>(stage), initialized,
		       getMember<void *>(that, 0x280),
		       getMember<void *>(that, 0x288));
	return initialized;
}

void *TglAccel::vfAuxPageTableWithOptions(void *accelerator, void *task)
{
	PANIC_COND(!accelerator || !task || !callback ||
	           !callback->oVfAuxPageTableWithOptions,
		"ngreen", "Missing native VF Aux page-table construction boundary");
	const UInt32 stage = gVfIdentity == VfIdentity::Virtual ?
		vfTakeTaskConstructionDiagnosticStage() : 0;
	if (stage != 0)
		SYSLOG("ngreen", "V316: task stage=%d Aux page-table enter accelerator=%p task=%p",
		       static_cast<int>(stage), accelerator, task);
	auto *table = FunctionCast(
		vfAuxPageTableWithOptions,
		callback->oVfAuxPageTableWithOptions)(accelerator, task);
	if (stage != 0)
		SYSLOG("ngreen", "V316: task stage=%d Aux page-table return=%p",
		       static_cast<int>(stage), table);
	return table;
}

void TglAccel::populateResetRegisterList(void *that)
{
	(void)that;
	// This replacement is installed only for a classified VF. It has no
	// guest-owned legacy reset registers; GuC restores direct-LRCA contexts
	// from PF-managed state.
}

void *TglAccel::igAccelTaskWithOptions(void *that)
{
	// V216: IOGraphicsAccelerator2 stores IntelAccelerator+0x150 only after
	// IGAccelTask::withOptions returns.  The first task therefore has to receive
	// counter value 0; that value controls its address-mode flags, managed page
	// table list, Global-GTT synchronization, and stamp/scratch allocation.
	//
	// Earlier failed start candidates increment Apple's process-wide counter but
	// IGAccelTask::free never decrements it (only IGAccelTask::stop resets it).
	// Reset the stale value before the next unassigned accelerator constructs its
	// kernel task.  Changing the per-object identity later in initialization is
	// unsafe because address mode and page-table state have already been chosen.
	const bool vfTask = vfIdentifyDevice() == VfIdentity::Virtual && that != nullptr;
	if (vfTask && !vfEnsurePageTableUpdateLock()) {
		SYSLOG("ngreen", "V283: cannot serialize VF task construction");
		return nullptr;
	}
	if (vfTask)
		IORecursiveLockLock(gVfPageTableUpdateLock);
	if (vfTask && getMember<void *>(that, 0x150) == nullptr) {
		if (callback->igAccelTaskCounter == 0) {
			SYSLOG("ngreen", "V216: bootstrap task counter symbol is unavailable; refusing unsafe allocation");
			IORecursiveLockUnlock(gVfPageTableUpdateLock);
			return nullptr;
		}

		auto *counter = reinterpret_cast<volatile UInt64 *>(callback->igAccelTaskCounter);
		UInt64 stale = *counter;
		if (stale != 0) {
			bool repaired = false;
			for (unsigned attempt = 0; attempt < 8 && stale != 0; attempt++) {
				if (OSCompareAndSwap64(stale, 0, counter)) {
					repaired = true;
					break;
				}
				stale = *counter;
			}

			if (repaired) {
				SYSLOG("ngreen",
				       "V216: reset stale VF bootstrap task counter from %llu before allocation",
				       static_cast<unsigned long long>(stale));
			} else if (*counter != 0) {
				SYSLOG("ngreen",
				       "V216: could not stabilize VF bootstrap task counter (current=%llu); refusing unsafe allocation",
				       static_cast<unsigned long long>(*counter));
				IORecursiveLockUnlock(gVfPageTableUpdateLock);
				return nullptr;
			}
		}
	}

	// Preserve native ownership: a failed factory returns null. Never write an
	// unrelated IOAccelTask base-class field or substitute a borrowed task. The
	// recursive domain spans inherited early list publication, private-table
	// construction and failed-init destruction/list unlink.
	if (vfTask)
		SYSLOG("ngreen", "V312: entering native IGAccelTask factory accelerator=%p",
		       that);
	auto *task = FunctionCast(igAccelTaskWithOptions,
	                         callback->oigAccelTaskWithOptions)(that);
	if (vfTask)
		SYSLOG("ngreen", "V312: native IGAccelTask factory returned task=%p",
		       task);
	if (vfTask)
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return task;
}

void *TglAccel::getBlit3DContext(void *that, bool create)
{
	if (!that || !callback->ogetBlit3DContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getBlit3DContext,
	                    callback->ogetBlit3DContext)(that, create);
}

bool TglAccel::vfInitScheduler(void *scheduler, uint32_t options,
	                        uint64_t privateSize, void *accelerator)
{
	if (!scheduler || !accelerator || !callback || !callback->originalSchedulerInit)
		return false;
	if (!FunctionCast(vfInitScheduler, callback->originalSchedulerInit)(
			scheduler, options, privateSize, accelerator))
		return false;
	// Native base init ignores addEventSource's result. The pinned default
	// timer constructor and effective getter let us verify actual attachment.
	auto *timer = getMember<IOTimerEventSource *>(scheduler, 0x448);
	auto *expected = getMember<IOWorkLoop *>(accelerator, 0xf0);
	if (!timer || !expected) {
		vfMarkProtocolFault("VF native scheduler success has incomplete timer ownership");
		PANIC("ngreen", "Cannot unwind incomplete VF scheduler timer ownership");
	}
	auto *attached = timer->getWorkLoop();
	if (attached && attached != expected) {
		vfMarkProtocolFault("VF scheduler timer bound to unexpected workloop");
		PANIC("ngreen", "Cannot release VF scheduler with foreign timer binding");
	}
	if (attached != expected) {
		vfMarkProtocolFault("VF scheduler timer failed workloop attachment");
		return false;
	}
	// Do not manually free: the failed outer factory owns final cleanup.
	return true;
}

void *TglAccel::vfCreateScheduler(void *accelerator)
{
	// Installed only on the VF route table. Validate the final native bits
	// before create can tail-dispatch into any physical scheduler factory.
	if (!accelerator || !callback || !callback->originalSchedulerCreate)
		return nullptr;
	const uint32_t schedulerType =
		(getMember<uint32_t>(accelerator, 0x1190) >> 23) & 7U;
	if (schedulerType != 4U) {
		vfMarkProtocolFault("VF final native scheduler selection is not GuC type 4");
		return nullptr;
	}
	SYSLOG("ngreen", "V310: entering native GuC scheduler factory after IRQ quiescence");
	auto *scheduler =
		FunctionCast(vfCreateScheduler, callback->originalSchedulerCreate)(accelerator);
	SYSLOG("ngreen", "V310: native GuC scheduler factory returned %s",
	       scheduler ? "success" : "failure");
	return scheduler;
}

void *TglAccel::vfRejectLegacySchedulerFactory(void *accelerator)
{
	(void)accelerator;
	vfMarkProtocolFault("legacy scheduler factory reached a VF");
	return nullptr;
}

bool TglAccel::vfRejectLegacySchedulerInit(void *scheduler, void *accelerator)
{
	(void)scheduler;
	(void)accelerator;
	vfMarkProtocolFault("legacy scheduler initialization reached a VF");
	return false;
}

void *TglAccel::vfRejectLegacyCommandStreamerFactory(void *accelerator,
	                                               void *workLoop,
	                                               void *scheduler,
	                                               uint32_t engine)
{
	(void)accelerator;
	(void)workLoop;
	(void)scheduler;
	(void)engine;
	vfMarkProtocolFault("Scheduler5 command-streamer factory reached a VF");
	return nullptr;
}

bool TglAccel::vfRejectLegacyCommandStreamerInit(void *streamer,
	                                          void *accelerator,
	                                          void *workLoop,
	                                          void *scheduler,
	                                          uint32_t engine)
{
	(void)streamer;
	(void)accelerator;
	(void)workLoop;
	(void)scheduler;
	(void)engine;
	vfMarkProtocolFault("Scheduler5 command-streamer initialization reached a VF");
	return false;
}

uint32_t TglAccel::vfRejectSafeReadRegister32(void *accelerator,
	                                       uint64_t registerOffset)
{
	(void)accelerator;
	(void)registerOffset;
	vfMarkProtocolFault("physical SafeReadRegister32 reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned 32-bit register read on a VF");
	return 0;
}

uint64_t TglAccel::vfRejectSafeReadRegister64(void *accelerator,
	                                       uint64_t registerOffset)
{
	(void)accelerator;
	(void)registerOffset;
	vfMarkProtocolFault("physical SafeReadRegister64 reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned 64-bit register read on a VF");
	return 0;
}

void TglAccel::vfRejectSafeWriteRegister32(void *accelerator,
	                                    uint64_t registerOffset,
	                                    uint32_t value)
{
	(void)accelerator;
	(void)registerOffset;
	(void)value;
	vfMarkProtocolFault("physical SafeWriteRegister32 reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned 32-bit register write on a VF");
}

IOReturn TglAccel::vfRejectSchedulerPmAttributes(void *scheduler,
	                                          const void *attributes)
{
	(void)scheduler;
	(void)attributes;
	vfMarkProtocolFault("physical scheduler PM attributes reached a VF");
	return kIOReturnUnsupported;
}

void TglAccel::vfDisablePhysicalCacheInit(void *memoryManager)
{
	(void)memoryManager;
	vfMarkProtocolFault("physical cache/PAT/MOCS initialization reached a VF");
}

void TglAccel::vfRejectDynamicInterruptClear(void *bridge, uint32_t identity,
	                                       uint32_t identityClear,
	                                       uint32_t iir,
	                                       uint32_t iirClearMask)
{
	(void)bridge;
	(void)identity;
	(void)identityClear;
	(void)iir;
	(void)iirClearMask;
	vfMarkProtocolFault("dynamic interrupt-register clear reached a VF");
}

uint32_t TglAccel::vfRejectDynamicInterruptRead(void *bridge, uint32_t identity,
	                                         uint32_t iir,
	                                         uint32_t iirClearMask)
{
	(void)bridge;
	(void)identity;
	(void)iir;
	(void)iirClearMask;
	vfMarkProtocolFault("dynamic interrupt-register read reached a VF");
	return 0;
}

bool TglAccel::vfDeferFilterInterruptEventSource(void *that)
{
	if (!callback || !callback->oVfCreateFilterInterruptEventSource || !that) {
		vfMarkProtocolFault("missing VF local interrupt-source factory");
		return false;
	}

	// MTL/ARL use a separately provisioned memory-IRQ transport. Preserve their
	// existing source-registration point; V311 is restricted to the Gen11
	// virtual-MMIO path observed on TGL/ADL/RPL media-12 VFs.
	if (gVfUsesMemoryIrq)
		return FunctionCast(vfDeferFilterInterruptEventSource,
		                    callback->oVfCreateFilterInterruptEventSource)(that);

	// initInterruptBridge has not published the bridge globally yet. Its native
	// free() explicitly accepts a null +0x20 source, so returning success here
	// preserves rollback ownership without opening MSI or PCI bus mastering.
	if (getMember<uint8_t>(that, 0x8AA) != 0 ||
	    getMember<void *>(that, 0x20) != nullptr) {
		vfMarkProtocolFault("VF local interrupt source was already published before HWS");
		return false;
	}
	OSSynchronizeIO();
	SYSLOG("ngreen", "V311: deferred Gen11 VF MSI source and bus-master transition until HWS readiness");
	return true;
}

bool TglAccel::startGraphicsEngine(void *that)
{
	// This replacement is installed only for a classified VF. Ring power,
	// reset and legacy execlist state belong to the PF, but the native tail of
	// IntelAccelerator::startGraphicsEngine first enters the scheduler firmware
	// boundary and then performs two software lifecycle transitions:
	// IGInterruptBridge::enable() and
	// IOGraphicsAccelerator2::enableAccelerator().  Omitting those transitions
	// leaves IOAccel resource creation asleep in acceleratorWaitEnabled().
	if (!that || !callback || !callback->vfSchedulerInitFirmware ||
	    !callback->vfInterruptBridgeEnable ||
	    !callback->vfInterruptBridgeDisable ||
	    !callback->oVfCreateFilterInterruptEventSource ||
	    !callback->ioPciConfigureInterrupts ||
	    !callback->ioGraphicsEnableAccelerator ||
	    !callback->ioAccelEventMachineInitEvent) {
		if (callback)
			SYSLOG("ngreen", "V368: lifecycle fw=%d bridge=%d/%d filter=%d msi=%d enable=%d event=%d",
			       callback->vfSchedulerInitFirmware != 0, callback->vfInterruptBridgeEnable != 0,
			       callback->vfInterruptBridgeDisable != 0, callback->oVfCreateFilterInterruptEventSource != 0,
			       callback->ioPciConfigureInterrupts != 0, callback->ioGraphicsEnableAccelerator != 0,
			       callback->ioAccelEventMachineInitEvent != 0);
		vfMarkProtocolFault("VF engine start before accelerator lifecycle is ready");
		return false;
	}

	// In the pinned native routine this call follows IGMemoryManager::initCache.
	// That cache method directly programs force-wake, MOCS, L3 and other
	// PF-owned MMIO, so a VF must omit it. IGScheduler::initFirmware is the next
	// independent named boundary: for scheduler 4 it constructs GuC and
	// registers the translated modern CTB. Requiring complete transport readiness
	// before this call created an impossible circular precondition and made every
	// otherwise-valid VF start return failure 0x214.
	auto *scheduler = getMember<void *>(that, 0x1250);
	if (!scheduler) {
		vfMarkProtocolFault("missing VF scheduler before firmware initialization");
		return false;
	}
	auto *interruptBridge = getMember<void *>(that, 0x1248);
	if (!interruptBridge) {
		vfMarkProtocolFault("missing VF interrupt bridge during engine start");
		return false;
	}

	if (!gVfUsesMemoryIrq) {
		// Reaching this routed call proves native start successfully constructed
		// the scheduler, stats/telemetry objects and every HWS mapped buffer. Do
		// not rely on the call order alone: validate the native active subset of
		// six possible engine HWS mappings and the global HWS mapping before the
		// IOPCIFamily factory can turn on PCI bus mastering. Tahoe allocates only
		// bits present in accelerator+0x1300; requiring all six fabricates engines
		// that are absent from the runtime topology.
		if (getMember<uint8_t>(interruptBridge, 0x8AA) != 0 ||
		    getMember<void *>(interruptBridge, 0x20) != nullptr ||
		    !callback->vfSharedMappedBufferGetVirtualAddress ||
		    !callback->vfMappedBufferGetGPUVirtualAddress) {
			vfMarkProtocolFault("VF deferred interrupt source preconditions are incomplete");
			return false;
		}
		const uint64_t engineMask = getMember<uint64_t>(that, 0x1300);
		if (engineMask == 0 || (engineMask & ~0x3FULL) != 0) {
			SYSLOG("ngreen", "V318: invalid native HWS engine mask=0x%llx",
			       static_cast<unsigned long long>(engineMask));
			vfMarkProtocolFault("VF native HWS engine mask is invalid");
			return false;
		}
		SYSLOG("ngreen", "V318: validating native active HWS engine mask=0x%llx",
		       static_cast<unsigned long long>(engineMask));
		using CpuAddress = uint8_t *(*)(void *);
		using GpuAddress = uint64_t (*)(void *);
		auto getCpu = reinterpret_cast<CpuAddress>(
			callback->vfSharedMappedBufferGetVirtualAddress);
		auto getGpu = reinterpret_cast<GpuAddress>(
			callback->vfMappedBufferGetGPUVirtualAddress);
		auto mappedHws = [&](OSObject *backing) {
			if (!backing || !getMember<void *>(backing, 0x30))
				return false;
			return NGGgtt::mappedBacking(
				reinterpret_cast<uintptr_t>(getCpu(backing)),
				getMember<uint64_t>(backing, kVfMappedBufferLengthOffset),
				PAGE_SIZE, getGpu(backing), gVfGGTTBase, gVfGGTTSize,
				kGucGgttTop);
		};
		for (size_t index = 0; index < 6; index++) {
			auto *backing = getMember<OSObject *>(that, 0x1408 + index * 8);
			if ((engineMask & (1ULL << index)) == 0) {
				if (backing) {
					SYSLOG("ngreen", "V318: inactive native HWS slot=%lu is populated",
					       index);
					vfMarkProtocolFault("VF inactive engine has an HWS mapping");
					return false;
				}
				continue;
			}
			if (!mappedHws(backing)) {
				SYSLOG("ngreen", "V318: active native HWS mapping failed slot=%lu mask=0x%llx",
				       index, static_cast<unsigned long long>(engineMask));
				vfMarkProtocolFault("VF engine HWS mapping incomplete before bus mastering");
				return false;
			}
		}
		if (!mappedHws(getMember<OSObject *>(that, 0x1438))) {
			vfMarkProtocolFault("VF global HWS mapping incomplete before bus mastering");
			return false;
		}

		auto *provider = OSDynamicCast(
			IOPCIDevice, static_cast<IOService *>(that)->getProvider());
		if (!provider ||
		    (provider->configRead16(kIOPCIConfigCommand) &
		     kIOPCICommandBusMaster) != 0) {
			vfMarkProtocolFault("VF PCI bus mastering escaped the deferred HWS boundary");
			return false;
		}
		PANIC_COND(!NGreen::callback->openVfBusMasterAdmission(), "ngreen",
			"Cannot open the validated VF PCI Bus Master admission");
		OSSynchronizeIO();
		SYSLOG("ngreen", "V322: opened VF PCI Bus Master admission after native HWS validation");
		// Tahoe exposes one 64-bit MSI vector and no legacy INTx route for this
		// VF. configureInterrupts is the first operation which turns on Bus Master,
		// so it belongs after the complete HWS proof and immediately before the
		// original filter-source factory which consumes the configured vector.
		using ConfigureInterrupts = IOReturn (*)(IOPCIDevice *, UInt32,
			UInt32, UInt32, IOOptionBits);
		const IOReturn interruptResult =
			reinterpret_cast<ConfigureInterrupts>(
				callback->ioPciConfigureInterrupts)(
					provider, kIOInterruptTypePCIMessaged, 1, 1, 0);
		if (interruptResult != kIOReturnSuccess) {
			SYSLOG("ngreen", "V320: late VF MSI allocation failed ret=0x%x",
			       interruptResult);
			NGreen::callback->closeVfBusMasterAdmission();
			provider->setBusMasterEnable(false);
			OSSynchronizeIO();
			PANIC_COND((provider->configRead16(kIOPCIConfigCommand) &
			            kIOPCICommandBusMaster) != 0,
				"ngreen", "Cannot revoke failed VF MSI configuration Bus Master admission");
			vfMarkProtocolFault("VF MSI allocation failed after HWS validation");
			return false;
		}
		OSSynchronizeIO();
		SYSLOG("ngreen", "V320: allocated VF PCI MSI after validating all HWS mappings");
		using CreateFilterSource = bool (*)(void *);
		const bool sourceCreated =
			reinterpret_cast<CreateFilterSource>(
				callback->oVfCreateFilterInterruptEventSource)(interruptBridge);
		OSSynchronizeIO();
		if (!sourceCreated ||
		    getMember<void *>(interruptBridge, 0x20) == nullptr ||
		    (provider->configRead16(kIOPCIConfigCommand) &
		     kIOPCICommandBusMaster) == 0) {
			NGreen::callback->closeVfBusMasterAdmission();
			provider->setBusMasterEnable(false);
			OSSynchronizeIO();
			PANIC_COND((provider->configRead16(kIOPCIConfigCommand) &
			            kIOPCICommandBusMaster) != 0,
				"ngreen", "Cannot revoke failed late VF MSI Bus Master admission");
			vfMarkProtocolFault("VF deferred MSI source did not establish its bus-master boundary");
			return false;
		}
		SYSLOG("ngreen", "V320: created Gen11 VF MSI source after late allocation and HWS validation");
	}
	using LifecycleMethod = void (*)(void *);
	// A VF has to admit its MSI consumer before CTB enable. Unlike the physical
	// Tahoe sequence, scheduler initialization allocates GGTT-backed proxy state
	// after enabling CTB, and every post-CTB map requires a synchronous GuC TLB
	// completion. Linux follows the same dependency: CT is enabled, interrupt
	// delivery is enabled immediately, then messages crossing that boundary are
	// consumed. The UUID-pinned bridge body retains the event-source lifecycle;
	// force-wake is isolated. TGL/ADL/RPL retain its native Gen11 virtual-MMIO
	// programming while MTL/ARL route the bridge to their memory-IRQ page.
	reinterpret_cast<LifecycleMethod>(callback->vfInterruptBridgeEnable)(
		interruptBridge);
	if (getMember<uint8_t>(interruptBridge, 0x8A8) == 0) {
		vfMarkProtocolFault("VF interrupt bridge did not enter enabled state");
		return false;
	}
	if (!gVfUsesMemoryIrq) {
		OSCompareAndSwap(0, 1, &gVfMmioIrqReady);
		OSSynchronizeIO();
	}
	SYSLOG("ngreen", "V251: enabled VF %s MSI consumer before scheduler firmware initialization",
	       gVfUsesMemoryIrq ? "memory-IRQ" : "virtual-MMIO");

	using InitFirmware = IOReturn (*)(void *);
	const IOReturn firmwareResult =
		reinterpret_cast<InitFirmware>(callback->vfSchedulerInitFirmware)(scheduler);
	if (firmwareResult != kIOReturnSuccess) {
		SYSLOG("ngreen", "V247: VF scheduler firmware initialization failed ret=0x%x",
		       firmwareResult);
		reinterpret_cast<LifecycleMethod>(callback->vfInterruptBridgeDisable)(
			interruptBridge);
		OSCompareAndSwap(1, 0, &gVfMmioIrqReady);
		OSSynchronizeIO();
		vfMarkProtocolFault("VF scheduler firmware initialization failure");
		return false;
	}
	OSCompareAndSwap(0, 1, &gVfSchedulerFirmwareReady);
	OSSynchronizeIO();
	if (!vfNativeGpuWorkReady()) {
		SYSLOG("ngreen",
		       "V251: incomplete post-firmware state ggtt=%d irq=%d mode=%s ctbCpu=%d ctbGpu=0x%x enabled=%d stopped=%d submissionStopped=%d fault=%d",
		       gVfGGTTReady, vfInterruptTransportReady(),
		       gVfUsesMemoryIrq ? "memory" : "MMIO", gVfCtbCpuBase != nullptr,
		       gVfCtbGpuBase, gVfCtbEnabled != 0, gVfCtbStopped != 0,
		       gVfSubmissionStopped != 0, gVfProtocolFault != 0);
		vfMarkProtocolFault("VF transport incomplete after scheduler firmware initialization");
		return false;
	}
	SYSLOG("ngreen", "V247: VF scheduler firmware and GuC transport initialized before accelerator enable");

	reinterpret_cast<LifecycleMethod>(callback->ioGraphicsEnableAccelerator)(that);

	// The next two calls in the UUID-pinned native tail attach the accelerator's
	// embedded completion events to IOAccelEventMachineFast2.  They are software
	// lifecycle, not engine programming, and omitting them leaves later stamp and
	// teardown paths operating on events that were never admitted by the event
	// machine.
	auto *eventMachine = getMember<void *>(that, 0x380);
	if (!eventMachine) {
		vfMarkProtocolFault("missing VF event machine during engine start");
		return false;
	}
	using InitEventMethod = void (*)(void *, void *);
	auto initEvent = reinterpret_cast<InitEventMethod>(
		callback->ioAccelEventMachineInitEvent);
	initEvent(eventMachine, reinterpret_cast<uint8_t *>(that) + 0x11A8);
	initEvent(eventMachine, reinterpret_cast<uint8_t *>(that) + 0x11E8);
	SYSLOG("ngreen", "V245: enabled VF bridge/IOAccelerator and initialized completion events");
	return true;
}

bool TglAccel::stopGraphicsEngine(void *that)
{
	if (!that || !callback || !callback->vfInterruptBridgeDisable ||
	    !callback->ioGraphicsDisableAccelerator) {
		vfMarkProtocolFault("VF engine stop without accelerator lifecycle API");
		return false;
	}
	// Preserve the native software timer cancellation before shutdown. The
	// pinned constructor stores a base IOTimerEventSource at +0x1460.
	// This invalidates scheduled work; it does not drain callbacks or prove
	// DMA quiescence. Do not copy native +0x1458=1 (a reported idle state).
	auto *dpsmTimer = getMember<IOTimerEventSource *>(that, 0x1460);
	if (dpsmTimer)
		dpsmTimer->cancelTimeout();
	if (gVfDeviceStopping && gVfGGTTReady) {
		PANIC_COND(!vfQuiesceDeviceForShutdown(gVfHardwareGuc), "ngreen",
			"Cannot stop VF accelerator before every GuC context and DMA mapping is quiesced");
		SYSLOG("ngreen", "V242: VF final engine stop completed through GuC context retirement");
	}

	auto *interruptBridge = getMember<void *>(that, 0x1248);
	if (!interruptBridge) {
		vfMarkProtocolFault("missing VF interrupt bridge during engine stop");
		return false;
	}
	using LifecycleMethod = void (*)(void *);
	// Preserve the native order after any final VF DMA quiescence.  On
	// TGL/ADL/RPL this also disables the VF-owned virtual-MMIO interrupt block;
	// on MTL/ARL the routed bridge masks the memory-IRQ page.
	reinterpret_cast<LifecycleMethod>(callback->vfInterruptBridgeDisable)(
		interruptBridge);
	OSCompareAndSwap(1, 0, &gVfMmioIrqReady);
	OSSynchronizeIO();
	// IOPCIFamily's MSI unregister/disable path does not revoke the PCI
	// Command Bus Master bit that source registration enabled.  Clear it only
	// after the final device-wide quiescence proof; an ordinary sleep/wake
	// engine stop must retain its configured source and restart capability.
	if (gVfDmaQuiesced) {
		auto *provider = OSDynamicCast(
			IOPCIDevice, static_cast<IOService *>(that)->getProvider());
		PANIC_COND(!provider, "ngreen",
			"Cannot revoke VF PCI bus mastering without its provider");
		NGreen::callback->closeVfBusMasterAdmission();
		provider->setBusMasterEnable(false);
		OSSynchronizeIO();
		PANIC_COND((provider->configRead16(kIOPCIConfigCommand) &
		            kIOPCICommandBusMaster) != 0,
			"ngreen", "VF PCI bus mastering remained enabled after DMA quiescence");
		SYSLOG("ngreen", "V314: revoked VF PCI Bus Master after DMA quiescence");
	}
	reinterpret_cast<LifecycleMethod>(callback->ioGraphicsDisableAccelerator)(that);
	SYSLOG("ngreen", "V244: disabled VF interrupt bridge and IOAccelerator lifecycle");
	return true;
}

static bool vfRequireSuccessfulNativeSubmission(bool submitted,
	const char *failure)
{
	if (submitted)
		return true;
	vfMarkProtocolFault(failure);
	PANIC_COND(true, "ngreen", "%s", failure);
	return false;
}

bool TglAccel::submitBlit(void *that, void *param_1, void *param_2, void *param_3, bool param_4) {
	// Native returns a boolean in AL, not an IOReturn. Every rejected/no-op
	// path below returns false. A classified VF must fail-stop those paths for
	// non-empty work because the complete caller inventory includes consumers
	// which ignore AL and publish successful CPU-side progress.
	PANIC_COND(!callback || !callback->osubmitBlit, "ngreen",
		"Missing native blit submission trampoline");
	if (gVfIdentity != VfIdentity::Virtual)
		return FunctionCast(submitBlit, callback->osubmitBlit)(
			that, param_1, param_2, param_3, param_4);
	if (!that || !param_2)
		return vfRequireSuccessfulNativeSubmission(false,
			"VF blit submission received invalid arguments");

	// An empty vector is a genuine native no-op. Preserve its return without
	// requiring transport or task/context ownership.
	const uint64_t count = *reinterpret_cast<const uint64_t *>(param_2);
	if (count == 0)
		return FunctionCast(submitBlit, callback->osubmitBlit)(
			that, param_1, param_2, param_3, param_4);
	if (!param_1 || !param_3 || !getMember<void *>(param_3, 0x0))
		return vfRequireSuccessfulNativeSubmission(false,
			"VF blit submission lacks parameters or task ownership");
	if (!vfNativeGpuWorkReady())
		return vfRequireSuccessfulNativeSubmission(false,
			"VF blit submission reached a stopped transport");

	// Tahoe may dereference either native blit-context FIFO depending on its
	// format/route selection. Preallocate both through the original factories,
	// but do not write private task slots or substitute unrelated objects.
	void *blit2D = getBlit2DContext(param_3, true);
	void *blit3D = getBlit3DContext(param_3, true);
	if (!blit2D || !blit3D || !getMember<void *>(blit2D, 0xb8) ||
	    !getMember<void *>(blit3D, 0xb8)) {
		SYSLOG("ngreen", "V300: rejected blit with incomplete native contexts task=%p 2D=%p 3D=%p",
		       param_3, blit2D, blit3D);
		return vfRequireSuccessfulNativeSubmission(false,
			"VF blit submission has incomplete native contexts");
	}

	const bool submitted = FunctionCast(submitBlit,
		callback->osubmitBlit)(that, param_1, param_2, param_3, param_4);
	return vfRequireSuccessfulNativeSubmission(submitted,
		"Native VF blit returned without publishing non-empty work");
}

bool TglAccel::vfSubmitCCSResolve(void *that, void *entry, void *accelerator,
	void *task, uint32_t resolveType, uint8_t plane, uint8_t level)
{
	PANIC_COND(!callback || !callback->oVfSubmitCCSResolve, "ngreen",
		"Missing native CCS resolve trampoline");
	using Submit = bool (*)(void *, void *, void *, void *, uint32_t,
	                       uint8_t, uint8_t);
	auto native = reinterpret_cast<Submit>(callback->oVfSubmitCCSResolve);
	if (gVfIdentity != VfIdentity::Virtual)
		return native(that, entry, accelerator, task, resolveType, plane, level);
	if (!that || !entry || !task || accelerator != gVfAccelerator ||
	    !vfNativeGpuWorkReady())
		return vfRequireSuccessfulNativeSubmission(false,
			"VF CCS resolve lacks stable owners or transport");
	const bool submitted = native(
		that, entry, accelerator, task, resolveType, plane, level);
	return vfRequireSuccessfulNativeSubmission(submitted,
		"Native VF CCS resolve returned false after admission");
}

bool TglAccel::vfSubmitDepthResolve(void *that, void *entry, void *accelerator,
	void *task, uint32_t resolveType, uint8_t plane, uint8_t level,
	uint16_t width, uint16_t height)
{
	PANIC_COND(!callback || !callback->oVfSubmitDepthResolve, "ngreen",
		"Missing native depth resolve trampoline");
	using Submit = bool (*)(void *, void *, void *, void *, uint32_t,
	                       uint8_t, uint8_t, uint16_t, uint16_t);
	auto native = reinterpret_cast<Submit>(callback->oVfSubmitDepthResolve);
	if (gVfIdentity != VfIdentity::Virtual)
		return native(that, entry, accelerator, task, resolveType, plane, level,
		              width, height);
	if (!that || !entry || !task || accelerator != gVfAccelerator ||
	    !vfNativeGpuWorkReady())
		return vfRequireSuccessfulNativeSubmission(false,
			"VF depth resolve lacks stable owners or transport");
	const bool submitted = native(that, entry, accelerator, task, resolveType,
		plane, level, width, height);
	return vfRequireSuccessfulNativeSubmission(submitted,
		"Native VF depth resolve returned false after admission");
}

void TglAccel::barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
								void *event, uint16_t count, const uint16_t *list) {
	// Both direct callers in the pinned Tahoe payload ignore RAX, and the native
	// body has return paths that do not establish a value: this is a void ABI.
	// Therefore no wrapper rejection can be reported to the caller. Fail-stop
	// before it continues without the mandatory barrier side effects.
	PANIC_COND(!queue || !accelerator || !cmdDesc || !event ||
		(count && !list) || !callback || !callback->obarrierSubmission,
		"ngreen", "Invalid Tahoe barrier submission arguments or trampoline");
	if (gVfIdentity == VfIdentity::Virtual) {
		PANIC_COND(!vfNativeGpuWorkReady(), "ngreen",
			"VF barrier reached without an admitted GPU transport");

		// The Tahoe body unconditionally dereferences 2D, 3D and depth FIFO
		// pointers and may dereference color as well. Prime every native context;
		// if any allocation is missing, the void caller cannot recover or observe
		// an error, so continuing would fabricate successful barrier progress.
		void *task = getMember<void *>(cmdDesc, 0x8);
		void *blit2D = task ? getBlit2DContext(task, true) : nullptr;
		void *blit3D = task ? getBlit3DContext(task, true) : nullptr;
		void *depth = task ? getDepthResolveContext(task, true) : nullptr;
		void *color = task ? getColorResolveContext(task, true) : nullptr;
		const bool incomplete = !blit2D || !blit3D || !depth || !color ||
		    !getMember<void *>(blit2D, 0xb8) ||
		    !getMember<void *>(blit3D, 0xb8) ||
		    !getMember<void *>(depth, 0xb8) ||
		    !getMember<void *>(color, 0xb8);
		if (incomplete) {
			SYSLOG("ngreen", "V243: rejected VF barrier with incomplete contexts task=%p 2D=%p 3D=%p depth=%p color=%p",
			       task, blit2D, blit3D, depth, color);
		}
		PANIC_COND(incomplete, "ngreen",
			"VF barrier cannot continue with incomplete native contexts");
	}
	FunctionCast(barrierSubmission, callback->obarrierSubmission)(
		queue, accelerator, cmdDesc, event, count, list);
}

void *TglAccel::getBlit2DContext(void *that, bool create)
{
	if (!that || !callback->ogetBlit2DContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getBlit2DContext,
	                    callback->ogetBlit2DContext)(that, create);
}

void *TglAccel::getDepthResolveContext(void *that, bool create)
{
	if (!that || !callback->ogetDepthResolveContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getDepthResolveContext,
	                    callback->ogetDepthResolveContext)(that, create);
}

void *TglAccel::getColorResolveContext(void *that, bool create)
{
	if (!that || !callback->ogetColorResolveContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getColorResolveContext,
	                    callback->ogetColorResolveContext)(that, create);
}

bool TglAccel::loadGuCBinary(void *that) {
	// The PF already owns and runs GuC for an SR-IOV VF. Report firmware as
	// available so Apple's GuC scheduler initializes its submission transport,
	// but never try to replace the PF-owned image or WOPCM configuration.
	// This route is installed only for the UUID-pinned VF payload.
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady || gVfProtocolFault)
		return false;
	// The stock load routine initializes all scheduler-side allocations before
	// touching WOPCM and uploading firmware. Skipping it outright left
	// contextCount at zero, so the first createUkContext returned the 0x400
	// invalid-context sentinel and tore the GuC object down.
	if (!that || !callback->orgInitSchedControl || !vfCanUseSleepingLock() ||
	    !getMember<void *>(that, 0x38) || !getMember<IOLock *>(that, 0x40) ||
	    !getMember<IOLock *>(that, 0xA08)) {
		vfAbortSchedulerBootstrap("missing VF scheduler initializer, owner or locks");
		return false;
	}
	using InitSchedControl = bool (*)(void *);
	const bool initialized = reinterpret_cast<InitSchedControl>(
		callback->orgInitSchedControl)(that);
	// Native initSchedControl checks setupContextPool but discards failures from
	// setupLogBuffers and setupAdditionalDataStructs before returning true. Do
	// not admit partially constructed scheduler storage.
	const bool storageReady = initialized && getMember<void *>(that, 0x50) &&
		getMember<void *>(that, 0x68) && getMember<void *>(that, 0x60) &&
		getMember<void *>(that, 0x70) && getMember<void *>(that, 0x78) &&
		getMember<void *>(that, 0x9E8);
	if (!storageReady) {
		vfAbortSchedulerBootstrap("incomplete native VF scheduler storage allocation");
		return false;
	}
	SYSLOG("ngreen", "V224: VF GuC firmware is PF-owned; scheduler data init=%d",
	       initialized);
	return initialized && !gVfProtocolFault;
}

void TglAccel::wrapSafeForceWake(void *that, bool set, uint32_t dom)
{
	forceWake(that, set, dom, 0);
}

void TglAccel::forceWake(void *that, bool set, uint32_t dom, uint32_t ctx)
{
	(void)that;
	(void)set;
	(void)dom;
	(void)ctx;
	// i915 creates no force-wake domains for a VF. Power ownership remains
	// with the PF/GuC, so every routed VF entry point is an intentional no-op.
	static bool logged = false;
	if (!logged) {
		logged = true;
		SYSLOG("ngreen", "V240: guest force-wake suppressed for SR-IOV VF");
	}
}
bool TglAccel::injectAcceleratorPersonality(const char *bundleId)
{
	if (this->acceleratorPersonalityInjected) {
		DBGLOG("ngreen", "injectAcceleratorPersonality: already injected, skipping");
		return true;
	}

	if (!bundleId || !gIOCatalogue)
		return false;

	// Clone the complete personality already admitted with this exact payload.
	// Reconstructing it by hand had silently dropped Development, Debug and both
	// HEVC capability dictionaries, making display and media behaviour diverge
	// from the bundled Tahoe driver.
	auto *matching = OSDictionary::withCapacity(1);
	auto *bundle = OSString::withCString(bundleId);
	if (!matching || !bundle) {
		OSSafeReleaseNULL(bundle);
		OSSafeReleaseNULL(matching);
		return false;
	}
	const bool matchingReady = matching->setObject("CFBundleIdentifier", bundle);
	bundle->release();
	if (!matchingReady) {
		matching->release();
		return false;
	}

	SInt32 generation = 0;
	auto *drivers = gIOCatalogue->findDrivers(matching, &generation);
	matching->release();
	if (!drivers || drivers->getCount() != 1) {
		SYSLOG("ngreen", "injectAcceleratorPersonality: expected one %s personality, found %u",
			bundleId, drivers ? drivers->getCount() : 0);
		OSSafeReleaseNULL(drivers);
		return false;
	}

	auto *source = OSDynamicCast(OSDictionary, drivers->getObject(0));
	const bool complete = source &&
		OSDynamicCast(OSDictionary, source->getObject("Development")) &&
		OSDynamicCast(OSDictionary, source->getObject("Debug")) &&
		OSDynamicCast(OSDictionary, source->getObject("IOAccelDisplayPipeCapabilities")) &&
		OSDynamicCast(OSDictionary, source->getObject("IOGVAHEVCDecodeCapabilities")) &&
		OSDynamicCast(OSDictionary, source->getObject("IOGVAHEVCEncodeCapabilities"));
	auto *dict = complete ? OSDictionary::withDictionary(source) : nullptr;
	drivers->release();
	if (!dict) {
		SYSLOG("ngreen", "injectAcceleratorPersonality: native personality is incomplete");
		return false;
	}

	// OSDictionary::withDictionary is shallow. Clone Development separately so
	// forcing the VF-only telemetry policy cannot mutate the admitted native PF
	// personality retained by the catalogue.
	auto *sourceDevelopment =
		OSDynamicCast(OSDictionary, dict->getObject("Development"));
	auto *development = sourceDevelopment ?
		OSDictionary::withDictionary(sourceDevelopment) : nullptr;
	auto *telemetryDisabled = OSNumber::withNumber(1ULL, 32);
	const bool telemetryReady = development && telemetryDisabled &&
		development->setObject("TelemetryDisable", telemetryDisabled) &&
		dict->setObject("Development", development);
	OSSafeReleaseNULL(telemetryDisabled);
	OSSafeReleaseNULL(development);
	if (!telemetryReady) {
		dict->release();
		return false;
	}

	// V327 added this exact Tahoe capability to the reviewed source payloads,
	// but the release/deployment product contains only NootedGreen.kext and
	// therefore cannot replace the Apple TGL Info.plist already installed in
	// the Guest.  Build the two-number dictionary on the private catalogue
	// clone instead of requiring an unpackaged source mutation.  Replacing the
	// shallow-cloned key is isolated from the native PF personality, while each
	// allocation and catalogue write remains part of the fail-closed publish
	// transaction.
	auto *h264Encode = OSDictionary::withCapacity(2);
	auto *qualityRating = OSNumber::withNumber(50ULL, 32);
	auto *videoToolboxRating = OSNumber::withNumber(400ULL, 32);
	const bool h264EncodeReady = h264Encode && qualityRating &&
		videoToolboxRating &&
		h264Encode->setObject("VTQualityRating", qualityRating) &&
		h264Encode->setObject("VTRating", videoToolboxRating) &&
		dict->setObject("IOGVAH264EncodeCapabilities", h264Encode);
	OSSafeReleaseNULL(videoToolboxRating);
	OSSafeReleaseNULL(qualityRating);
	OSSafeReleaseNULL(h264Encode);
	if (!h264EncodeReady) {
		dict->release();
		return false;
	}

	// Tahoe 25G229 AppleGVA removed the former TGL renderer slot 0x1080040
	// from its hard admission set, while retaining the ICL/Gen11 slot
	// 0x1080080.  Keep that admission alias, but preserve the cloned native TGL
	// IODVDBundleName so the signed TGL VADriver emits commands for TGL hardware.
	// The ICL userspace experiment reached the same selector ABI but left the
	// first accepted BCS batch permanently incomplete, proving that selector
	// topology alone is not a command-stream compatibility contract.
	auto *rendererId = OSNumber::withNumber(0x1080080ULL, 32);
	const bool rendererReady = rendererId &&
		dict->setObject("IOVARendererID", rendererId);
	OSSafeReleaseNULL(rendererId);
	if (!rendererReady) {
		dict->release();
		return false;
	}
	SYSLOG("ngreen",
		"V340: preserved native TGL media userspace with Tahoe renderer slot 0x1080080");
	// Opt-in changes only the data producer exposed to AppleGVA. The adapter
	// delegates every execution callback to the hash-pinned signed TGL driver.
	// This clone is already restricted to the admitted VF; PF is untouched.
	if (checkKernelArgument("-ngiclva")) {
		auto *iclVa = OSString::withCString("AppleIntelICLGraphicsVADriver");
		const bool iclReady = iclVa && dict->setObject("IODVDBundleName", iclVa);
		OSSafeReleaseNULL(iclVa);
		if (!iclReady) {
			dict->release();
			return false;
		}
		SYSLOG("ngreen", "V374: selected Tahoe ICL VADriver");
	} else if (checkKernelArgument("-ngreenmediaadapter")) {
		auto *mediaAdapter = OSString::withCString("NootedGreenTGLMediaAdapter");
		const bool adapterReady = mediaAdapter &&
			dict->setObject("IODVDBundleName", mediaAdapter);
		OSSafeReleaseNULL(mediaAdapter);
		if (!adapterReady) {
			dict->release();
			return false;
		}
		SYSLOG("ngreen", "V356: selected owned-data TGL media adapter without AppleGVA text patches");
	}

	auto *primaryMatch = OSString::withCString("0x9a498086");
	const bool matchReady = primaryMatch &&
		dict->setObject("IOPCIPrimaryMatch", primaryMatch);
	OSSafeReleaseNULL(primaryMatch);
	if (!matchReady) {
		dict->release();
		return false;
	}

	auto *array = OSArray::withCapacity(1);
	const bool arrayReady = array && array->setObject(dict);
	dict->release();
	if (!arrayReady) {
		OSSafeReleaseNULL(array);
		return false;
	}

	SYSLOG("ngreen",
		"V329: publishing complete %s personality with runtime Tahoe H.264 capability and renderer admission",
		bundleId);
	const bool ok = gIOCatalogue->addDrivers(array, true);
	array->release();
	this->acceleratorPersonalityInjected = ok;
	return ok;
}

// The VF has no guest-owned INSTDONE register. Its watchdog query is true only
// after every tracked direct-LRCA context has reached an idle lifecycle state.
bool TglAccel::wrapIGScheduler5IsGpuIdle(const void *that) {
	(void)that;
	return vfKnownIdleSnapshot(callback ?
		callback->vfSharedMappedBufferGetVirtualAddress : 0);
}

bool TglAccel::wrapIGScheduler4IsGpuIdle(const void *that) {
	(void)that;
	return vfKnownIdleSnapshot(callback ?
		callback->vfSharedMappedBufferGetVirtualAddress : 0);
}
