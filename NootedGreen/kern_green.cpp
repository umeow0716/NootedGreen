//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include "kern_gen11.hpp"
#include "kern_pci_identity.hpp"
#include "kern_gpu_capabilities.hpp"
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
	(void)patcher;

	auto *devInfo = DeviceInfo::create();
	PANIC_COND(!devInfo, "ngreen", "Failed to create DeviceInfo");
	devInfo->processSwitchOff();

	this->iGPU = OSDynamicCast(IOPCIDevice, devInfo->videoBuiltin);
	PANIC_COND(!this->iGPU, "ngreen", "videoBuiltin is not IOPCIDevice");
	// DeviceInfo keeps borrowed registry pointers and its deleter does not retain
	// them.  The PCI object is needed later by lazy BAR0 mapping and config-read
	// hooks, so make its plugin-lifetime ownership explicit before deleting the
	// temporary inventory.  NootedGreen is not unloadable while these routes are
	// installed; the matching release therefore belongs to whole-kext teardown.
	this->iGPU->retain();

	// The GPU must be in D0 with memory and DMA decoding enabled before its
	// BARs or VF capability register are accessed.
	this->iGPU->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
	this->iGPU->setBusMasterEnable(true);
	this->iGPU->setMemoryEnable(true);

	WIOKit::renameDevice(this->iGPU, "IGPU");
	WIOKit::awaitPublishing(this->iGPU);
	this->deviceId = WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigDeviceID);

	const bool physicalTgl = NGGpuCapabilities::isTigerLake(this->deviceId) &&
		ngPhysicalGpuAccessAllowed();
	this->isRealTGL = NGGpuCapabilities::useNativeTigerLakePath(this->deviceId, physicalTgl);
	SYSLOG("ngreen", "V243: GPU=%04x nativeTglPf=%d", this->deviceId, this->isRealTGL);

	const bool routedRead16 = KernelPatcher::routeVirtual(this->iGPU,
		WIOKit::PCIConfigOffset::ConfigRead16, configRead16, &orgConfigRead16);
	const bool routedRead32 = KernelPatcher::routeVirtual(this->iGPU,
		WIOKit::PCIConfigOffset::ConfigRead32, configRead32, &orgConfigRead32);
	PANIC_COND(!routedRead16 || !routedRead32 || !orgConfigRead16 || !orgConfigRead32,
		"ngreen", "Failed to route PCI configuration readers");

	DeviceInfo::deleter(devInfo);
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
	if (gen11.processKext(patcher, index, address, size)) {
		DBGLOG("ngreen", "Processed Generation 11 configuration");
	}
	return true;
}



uint16_t NGreen::configRead16(IORegistryEntry *service, uint32_t space, uint8_t offset) {
	if (callback && callback->orgConfigRead16) {
		auto result = callback->orgConfigRead16(service, space, offset);
		if (service && service == callback->iGPU && offset == WIOKit::kIOPCIConfigDeviceID) {
			uint32_t device;
			if (WIOKit::getOSDataValue(service, "device-id", device) && device <= 0xFFFFU)
				return static_cast<uint16_t>(device);
		}

		return result;
	}

	return UINT16_MAX;
}

uint32_t NGreen::configRead32(IORegistryEntry *service, uint32_t space, uint8_t offset) {
	if (callback && callback->orgConfigRead32) {
		auto result = callback->orgConfigRead32(service, space, offset);
		// According to lvs unaligned reads may happen
		if (service && service == callback->iGPU &&
		    (offset == WIOKit::kIOPCIConfigDeviceID || offset == WIOKit::kIOPCIConfigVendorID)) {
			uint32_t device;
			if (WIOKit::getOSDataValue(service, "device-id", device))
				return NGPciIdentity::replaceDeviceId32(result, offset, device);
		}

		return result;
	}

	return UINT32_MAX;
}
