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

NGreen *NGreen::callback = nullptr;

static Gen11 gen11;

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
