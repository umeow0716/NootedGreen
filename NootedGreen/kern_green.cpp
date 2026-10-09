//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include "kern_gen11.hpp"
#include "kern_pci_identity.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_telemetry.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <i386/machine_routines.h>
#include <kern/sched_prim.h>
#include <sys/kauth.h>

NGreen *NGreen::callback = nullptr;

static Gen11 gen11;

namespace {
// Tahoe 25G229 AppleGVA consumes the native ICL AVD_DriverCapability layout
// (0x438-byte records), while the signed TGL VADriver that must generate TGL
// commands returns its native 0x430-byte layout.  The common 0x18-byte header
// is unchanged; only the two format arrays and the linked-record pointer are
// eight bytes earlier in TGL.  Patch the exact H.264 scaler/encoder consumers
// in the two VideoToolbox services, never the signed file on disk.  Each
// LocalOnly anchor is unique in the pinned Tahoe AppleGVA __text and changes
// only one displacement byte.
constexpr uint32_t V342AppleGvaSection = 1;

static const uint8_t v342ScalerCountFind[] = {
	0xE8, 0x1F, 0xBD, 0xFB, 0xFF, 0x41, 0x8B, 0x4F, 0x28, 0x31, 0xD2
};
static const uint8_t v342ScalerCountReplace[] = {
	0xE8, 0x1F, 0xBD, 0xFB, 0xFF, 0x41, 0x8B, 0x4F, 0x20, 0x31, 0xD2
};
static const uint8_t v342ScalerFirstMatchFind[] = {
	0x41, 0x39, 0x44, 0xD7, 0x2C
};
static const uint8_t v342ScalerFirstMatchReplace[] = {
	0x41, 0x39, 0x44, 0xD7, 0x24
};
static const uint8_t v342ScalerFirstLoadFind[] = {
	0x4B, 0x8B, 0x44, 0xF7, 0x2C
};
static const uint8_t v342ScalerFirstLoadReplace[] = {
	0x4B, 0x8B, 0x44, 0xF7, 0x24
};
static const uint8_t v342ScalerSecondLoadFind[] = {
	0x49, 0x89, 0x45, 0x28,
	0x49, 0x8B, 0x87, 0x30, 0x02, 0x00, 0x00,
	0x49, 0x89, 0x45, 0x30, 0x49, 0xC7, 0x45, 0x38, 0x00, 0x00, 0x00, 0x00,
	0x49, 0x83, 0xBF, 0x30, 0x04, 0x00, 0x00, 0x00
};
static const uint8_t v342ScalerSecondLoadReplace[] = {
	0x49, 0x89, 0x45, 0x28,
	0x49, 0x8B, 0x87, 0x28, 0x02, 0x00, 0x00,
	0x49, 0x89, 0x45, 0x30, 0x49, 0xC7, 0x45, 0x38, 0x00, 0x00, 0x00, 0x00,
	0x49, 0x83, 0xBF, 0x30, 0x04, 0x00, 0x00, 0x00
};
static const uint8_t v342ScalerLinkCheckFind[] = {
	0x49, 0x83, 0xBF, 0x30, 0x04, 0x00, 0x00, 0x00
};
static const uint8_t v342ScalerLinkCheckReplace[] = {
	0x49, 0x83, 0xBF, 0x28, 0x04, 0x00, 0x00, 0x00
};
static const uint8_t v342ScalerLinkLoadFind[] = {
	0x49, 0x8B, 0x8F, 0x30, 0x04, 0x00, 0x00
};
static const uint8_t v342ScalerLinkLoadReplace[] = {
	0x49, 0x8B, 0x8F, 0x28, 0x04, 0x00, 0x00
};
static const uint8_t v342ScalerLinkedFirstFind[] = {
	0xF2, 0x43, 0x0F, 0x10, 0x44, 0xF7, 0x2C
};
static const uint8_t v342ScalerLinkedFirstReplace[] = {
	0xF2, 0x43, 0x0F, 0x10, 0x44, 0xF7, 0x24
};
static const uint8_t v342ScalerLinkedSecondFind[] = {
	0x49, 0x8B, 0x8F, 0x30, 0x02, 0x00, 0x00
};
static const uint8_t v342ScalerLinkedSecondReplace[] = {
	0x49, 0x8B, 0x8F, 0x28, 0x02, 0x00, 0x00
};
static const uint8_t v342EncoderFirstLoadFind[] = {
	0xF2, 0x0F, 0x10, 0x48, 0x2C
};
static const uint8_t v342EncoderFirstLoadReplace[] = {
	0xF2, 0x0F, 0x10, 0x48, 0x24
};
static const uint8_t v342EncoderSecondLoadFind[] = {
	0x48, 0x8B, 0x80, 0x30, 0x02, 0x00, 0x00,
	0x48, 0x89, 0x83, 0x20, 0xF1, 0x1E, 0x00
};
static const uint8_t v342EncoderSecondLoadReplace[] = {
	0x48, 0x8B, 0x80, 0x28, 0x02, 0x00, 0x00,
	0x48, 0x89, 0x83, 0x20, 0xF1, 0x1E, 0x00
};
static const uint8_t v342CapabilitySearchLinkFind[] = {
	0x4C, 0x8B, 0x89, 0x30, 0x04, 0x00, 0x00,
	0x4D, 0x85, 0xC9, 0x74, 0x06, 0x41, 0x83, 0x39, 0x10
};
static const uint8_t v342CapabilitySearchLinkReplace[] = {
	0x4C, 0x8B, 0x89, 0x28, 0x04, 0x00, 0x00,
	0x4D, 0x85, 0xC9, 0x74, 0x06, 0x41, 0x83, 0x39, 0x10
};
static const uint8_t v342FrameStatLinkCheckFind[] = {
	0xC7, 0x83, 0xB4, 0x9A, 0x23, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x44, 0x89, 0xA3, 0xA8, 0x9A, 0x23, 0x00,
	0x45, 0x31, 0xFF,
	0x48, 0x83, 0xB8, 0x30, 0x04, 0x00, 0x00, 0x00,
	0x0F, 0x84, 0x18, 0xFF, 0xFF, 0xFF
};
static const uint8_t v342FrameStatLinkCheckReplace[] = {
	0xC7, 0x83, 0xB4, 0x9A, 0x23, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x44, 0x89, 0xA3, 0xA8, 0x9A, 0x23, 0x00,
	0x45, 0x31, 0xFF,
	0x48, 0x83, 0xB8, 0x28, 0x04, 0x00, 0x00, 0x00,
	0x0F, 0x84, 0x18, 0xFF, 0xFF, 0xFF
};
static const uint8_t v342FrameStatLinkLoadFind[] = {
	0x48, 0x8B, 0x8B, 0xF8, 0x9B, 0x23, 0x00,
	0x48, 0x8B, 0x89, 0x30, 0x04, 0x00, 0x00,
	0x8B, 0x11, 0x89, 0x10, 0x48, 0x8B, 0x51, 0x08
};
static const uint8_t v342FrameStatLinkLoadReplace[] = {
	0x48, 0x8B, 0x8B, 0xF8, 0x9B, 0x23, 0x00,
	0x48, 0x8B, 0x89, 0x28, 0x04, 0x00, 0x00,
	0x8B, 0x11, 0x89, 0x10, 0x48, 0x8B, 0x51, 0x08
};

#define V342_LOCAL_PATCH(name) \
	{CPU_TYPE_X86_64, UserPatcher::LocalOnly, name##Find, name##Replace, \
	 arrsize(name##Find), 0, 1, UserPatcher::SegmentTextText, V342AppleGvaSection}

static UserPatcher::BinaryModPatch v342AppleGvaPatches[] = {
	V342_LOCAL_PATCH(v342ScalerCount),
	V342_LOCAL_PATCH(v342ScalerFirstMatch),
	V342_LOCAL_PATCH(v342ScalerFirstLoad),
	V342_LOCAL_PATCH(v342ScalerSecondLoad),
	V342_LOCAL_PATCH(v342ScalerLinkCheck),
	V342_LOCAL_PATCH(v342ScalerLinkLoad),
	V342_LOCAL_PATCH(v342ScalerLinkedFirst),
	V342_LOCAL_PATCH(v342ScalerLinkedSecond),
	V342_LOCAL_PATCH(v342EncoderFirstLoad),
	V342_LOCAL_PATCH(v342EncoderSecondLoad),
	V342_LOCAL_PATCH(v342CapabilitySearchLink),
	V342_LOCAL_PATCH(v342FrameStatLinkCheck),
	V342_LOCAL_PATCH(v342FrameStatLinkLoad),
};
#undef V342_LOCAL_PATCH

static UserPatcher::BinaryModInfo v342AppleGvaBinary {
	"/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA",
	v342AppleGvaPatches, arrsize(v342AppleGvaPatches)
};

// V349 captured this exact KAUTH exec path three times in the bounded runtime
// trace.  Patch only that proven encoder executable.  Decoder remains under
// the observation-only KAUTH listener until its exact path is observed after
// encoding crosses the current admission boundary.
static UserPatcher::ProcInfo v350EncoderProcess[] = {
    {"/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/VTEncoderXPCService.xpc/Contents/MacOS/VTEncoderXPCService",
     sizeof("/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/VTEncoderXPCService.xpc/Contents/MacOS/VTEncoderXPCService") - 1,
     V342AppleGvaSection, UserPatcher::ProcInfo::MatchExact},
};

void registerV350AppleGvaEncoderBridge() {
	lilu.onProcLoadForce(
        v350EncoderProcess, arrsize(v350EncoderProcess),
		[](void *, UserPatcher &, vm_map_t, const char *path, size_t pathLength) {
            SYSLOG("ngreen", "V350: dispatched proven encoder-only AppleGVA TGL capability-layout bridge path-len=%lu path=%s",
				pathLength, path);
		}, nullptr, &v342AppleGvaBinary, 1);
    SYSLOG("ngreen", "V350: armed proven encoder-only Tahoe AppleGVA TGL capability-layout bridge with 13 local-only sites");
}

// V347/V348 proved that neither the canonical XPC bundle tail nor either
// service basename occurs in KAUTH_FILEOP_EXEC's actual path.  Register a
// separate observation-only listener so unmatched paths (notably decoder) can
// be seen independently of any Lilu process patch.  The exact TGL payload arms
// capture only after its routes and personality are complete; a hard cap bounds
// log volume until the one media trigger starts its services.
static kauth_listener_t v349MediaExecListener {nullptr};
static UInt8 v349MediaExecCookie {0};
static volatile UInt32 v349MediaExecArmed {0};
static volatile SInt32 v349MediaExecCount {0};
constexpr SInt32 V349MediaExecLimit = 64;

int observeV349MediaExecPath(kauth_cred_t, void *idata, kauth_action_t action,
	uintptr_t, uintptr_t arg1, uintptr_t, uintptr_t) {
	if (idata == &v349MediaExecCookie && action == KAUTH_FILEOP_EXEC && arg1 &&
	    v349MediaExecArmed != 0) {
		const auto slot = OSIncrementAtomic(&v349MediaExecCount);
		if (slot < V349MediaExecLimit) {
			const auto *path = reinterpret_cast<const char *>(arg1);
			SYSLOG("ngreen", "V349: observed post-TGL exec vnode path slot=%d path-len=%lu path=%s",
				slot, strlen(path), path);
		}
	}
	return 0;
}

void registerV349MediaExecObservation() {
	PANIC_COND(v349MediaExecListener, "ngreen",
		"V349 media exec observer registered more than once");
	v349MediaExecListener = kauth_listen_scope(
		KAUTH_SCOPE_FILEOP, observeV349MediaExecPath, &v349MediaExecCookie);
	PANIC_COND(!v349MediaExecListener, "ngreen",
		"Cannot register V349 media exec observer");
	SYSLOG("ngreen", "V349: registered bounded KAUTH exec observer independently of binary modifications");
}
} // namespace

void ngArmV349MediaExecObservation() {
	PANIC_COND(!v349MediaExecListener, "ngreen",
		"Cannot arm missing V349 media exec observer");
	PANIC_COND(!OSCompareAndSwap(0, 1, &v349MediaExecArmed), "ngreen",
		"V349 media exec observer armed more than once");
	OSSynchronizeIO();
	SYSLOG("ngreen", "V349: enabled bounded KAUTH exec capture after exact TGL payload publication");
}

void NGreen::init() {
    callback = this;

	gen11.init();
    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<NGreen *>(user)->processPatcher(patcher); }, this);
    lilu.onKextLoadForce(
        nullptr, 0,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            static_cast<NGreen *>(user)->processKext(patcher, index, address, size);
        },
        this);
	
}


void NGreen::processPatcher(KernelPatcher &patcher) {
	auto *devInfo = DeviceInfo::create();
	PANIC_COND(!devInfo, "ngreen", "Failed to create DeviceInfo");
	devInfo->processSwitchOff();

	this->iGPU = OSDynamicCast(IOPCIDevice, devInfo->videoBuiltin);
	if (!this->iGPU) {
		DeviceInfo::deleter(devInfo);
		SYSLOG("ngreen", "No built-in PCI GPU; NootedGreen remains inactive");
		return;
	}

	PANIC_COND(!ngResolveKernelTelemetry(patcher), "ngreen",
		"Cannot resolve Tahoe kernel GPU telemetry ABI");
	// DeviceInfo keeps borrowed registry pointers and its deleter does not retain
	// them.  The PCI object is needed later by lazy BAR0 mapping and config-read
	// hooks, so make its plugin-lifetime ownership explicit before deleting the
	// temporary inventory.  NootedGreen is not unloadable while these routes are
	// installed; the matching release therefore belongs to whole-kext teardown.
	this->iGPU->retain();

	// PCI configuration space does not require either BAR decoding or bus
	// mastering.  Capture the unspoofed identity first, then prevent every
	// SR-IOV-capable or unknown function from initiating DMA while BAR0 is used
	// to distinguish a PF from a VF.  A confirmed PF regains the historical
	// bus-master setting below; a VF must remain stopped until the routed engine
	// start has validated all HWS mappings and registers its MSI consumer.
	this->iGPU->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
	this->deviceId = WIOKit::readPCIConfigValue(
		this->iGPU, WIOKit::kIOPCIConfigDeviceID);
	const auto sriovCapability = NGGpuCapabilities::sriov(this->deviceId);
	if (sriovCapability != NGGpuCapabilities::Sriov::Absent)
		this->iGPU->setBusMasterEnable(false);
	this->iGPU->setMemoryEnable(true);

	WIOKit::renameDevice(this->iGPU, "IGPU");
	WIOKit::awaitPublishing(this->iGPU);

	const bool physicalAccess = ngPhysicalGpuAccessAllowed();
	const bool virtualAccess = ngVirtualGpuAccessAllowed();
	this->iGPU->setBusMasterEnable(physicalAccess);
	OSSynchronizeIO();
	const bool busMasterEnabled =
		(this->iGPU->configRead16(kIOPCIConfigCommand) &
		 kIOPCICommandBusMaster) != 0;
	PANIC_COND(busMasterEnabled != physicalAccess, "ngreen",
		"Cannot establish identity-scoped PCI Bus Master state");
	SYSLOG("ngreen", "V314: PCI Bus Master %s after PF/VF identity classification",
	       physicalAccess ? "enabled for physical GPU" : "held off for VF/unknown GPU");

	const bool physicalTgl =
		NGGpuCapabilities::isTigerLake(this->deviceId) && physicalAccess;
	this->isRealTGL = NGGpuCapabilities::useNativeTigerLakePath(this->deviceId, physicalTgl);
	SYSLOG("ngreen", "V243: GPU=%04x nativeTglPf=%d", this->deviceId, this->isRealTGL);

	// V342 compared deviceId with 0x9a49 here, but deviceId is deliberately the
	// unspoofed PCI configuration identity captured above.  This host's VF is
	// therefore 0xa7a8 at this boundary; 0x9a49 exists only in the exact Guest
	// registry compatibility identity consumed by our later config-read route.
	// Require all three facts independently so an unclassified/PF function or a
	// different compatibility personality can never receive the userspace patch.
	uint32_t compatibilityDeviceId = 0;
	const bool exactTigerLakeVf = virtualAccess && this->deviceId == 0xA7A8 &&
		WIOKit::getOSDataValue(this->iGPU, "device-id", compatibilityDeviceId) &&
		compatibilityDeviceId == 0x9A49;
	if (exactTigerLakeVf) {
		SYSLOG("ngreen", "V343: classified AppleGVA bridge VF physical=a7a8 compatibility=9a49");
		registerV350AppleGvaEncoderBridge();
		registerV349MediaExecObservation();
	}

	const bool routedRead16 = KernelPatcher::routeVirtual(this->iGPU,
		WIOKit::PCIConfigOffset::ConfigRead16, configRead16, &orgConfigRead16);
	const bool routedRead32 = KernelPatcher::routeVirtual(this->iGPU,
		WIOKit::PCIConfigOffset::ConfigRead32, configRead32, &orgConfigRead32);
	const bool routedBusMaster = KernelPatcher::routeVirtual(this->iGPU,
		kSetBusMasterEnableVirtualOffset, setBusMasterEnable,
		&orgSetBusMasterEnable);
	PANIC_COND(!routedRead16 || !routedRead32 || !routedBusMaster ||
		!orgConfigRead16 || !orgConfigRead32 || !orgSetBusMasterEnable,
		"ngreen", "Failed to route PCI configuration/Bus Master boundary");

	DeviceInfo::deleter(devInfo);
	this->driverReady = true;
}


bool NGreen::setRMMIOIfNecessary() {
	auto *mapping = this->rmmio;
	if (!mapping) {
		if (!this->iGPU || ml_at_interrupt_context() || !preemption_enabled())
			return false;
		mapping = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
		if (!mapping)
			return false;
		const auto address = mapping->getVirtualAddress();
		if (!address || (address & 3U) || mapping->getLength() < sizeof(uint32_t)) {
			mapping->release();
			return false;
		}
		// Publish one immutable lifetime-long mapping. A losing initializer
		// drops only its own unused mapping and uses the published winner.
		if (!OSCompareAndSwapPtr(nullptr, mapping, &this->rmmio))
			mapping->release();
		mapping = this->rmmio;
	}
	OSCompareAndSwapPtr(nullptr, reinterpret_cast<void *>(mapping->getVirtualAddress()), &this->rmmioPtr);
	return true;
}

bool NGreen::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (!this->driverReady)
		return true;
	if (gen11.processKext(patcher, index, address, size)) {
		DBGLOG("ngreen", "Processed Generation 11 configuration");
	}
	return true;
}



uint16_t NGreen::configRead16(IORegistryEntry *service, uint32_t space, uint8_t offset) {
	if (callback && callback->orgConfigRead16) {
		auto result = callback->orgConfigRead16(service, space, offset);
		if (service && service == callback->iGPU &&
		    NGPciIdentity::readsDeviceId16(space, offset)) {
			uint32_t device;
			if (WIOKit::getOSDataValue(service, "device-id", device))
				return NGPciIdentity::replaceDeviceId16(result, space, offset, device);
		}

		return result;
	}

	return UINT16_MAX;
}

uint32_t NGreen::configRead32(IORegistryEntry *service, uint32_t space, uint8_t offset) {
	if (callback && callback->orgConfigRead32) {
		auto result = callback->orgConfigRead32(service, space, offset);
		if (service && service == callback->iGPU &&
		    NGPciIdentity::readsVendorDevice32(space, offset)) {
			uint32_t device;
			if (WIOKit::getOSDataValue(service, "device-id", device))
				return NGPciIdentity::replaceDeviceId32(result, space, offset, device);
		}

		return result;
	}

	return UINT32_MAX;
}

bool NGreen::setBusMasterEnable(IOPCIDevice *device, bool enable) {
	auto *owner = callback;
	if (!owner || !owner->orgSetBusMasterEnable)
		return false;
	if (device != owner->iGPU || ngPhysicalGpuAccessAllowed() || !enable ||
	    owner->vfBusMasterAdmissionOpen())
		return owner->orgSetBusMasterEnable(device, enable);

	// Tahoe 25G229 IOGraphicsAccelerator2::start calls the IOPCIDevice virtual
	// at vtable +0x8d8 after setMemoryEnable(true), before IntelAccelerator has
	// created any HWS mapping.  Preserve setBusMasterEnable's previous-state
	// result while suppressing only this identity-scoped premature VF enable.
	const bool previous =
		(device->configRead16(kIOPCIConfigCommand) & kIOPCICommandBusMaster) != 0;
	if (previous) {
		owner->orgSetBusMasterEnable(device, false);
		OSSynchronizeIO();
		PANIC_COND((device->configRead16(kIOPCIConfigCommand) &
		            kIOPCICommandBusMaster) != 0,
			"ngreen", "Cannot close premature VF PCI Bus Master admission");
	}
	OSIncrementAtomic(&owner->vfEarlyBusMasterSuppressions);
	SYSLOG("ngreen", "V322: suppressed premature VF PCI Bus Master enable at the IOPCIDevice sink");
	return previous;
}
