//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include "kern_gen11.hpp"
#include "kern_pci_identity.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_dvmt_patch.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <i386/machine_routines.h>
#include <kern/sched_prim.h>


static const char *pathIOAcceleratorFamily2= "/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2";
static KernelPatcher::KextInfo kextIOAcceleratorFamily2 { "com.apple.iokit.IOAcceleratorFamily2", &pathIOAcceleratorFamily2, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded };

NGreen *NGreen::callback = nullptr;

static Gen11 gen11;

void NGreen::init() {
    callback = this;
	
	lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
	
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

	// The GPU must be in D0 with memory and DMA decoding enabled before its
	// BARs or VF capability register are accessed.
	this->iGPU->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
	this->iGPU->setBusMasterEnable(true);
	this->iGPU->setMemoryEnable(true);

	WIOKit::renameDevice(this->iGPU, "IGPU");
	WIOKit::awaitPublishing(this->iGPU);
	this->deviceId = WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigDeviceID);

	// CPUID is diagnostic only. A hypervisor may expose any CPU model; it
	// cannot identify the passed-through GPU or distinguish its PF/VF role.
	{
		uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
		asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
		uint32_t family = (eax >> 8) & 0xF;
		uint32_t model = (eax >> 4) & 0xF;
		const uint32_t extModel = (eax >> 16) & 0xF;
		const uint32_t stepping = eax & 0xF;
		if (family == 0x6)
			model |= extModel << 4;
		const bool physicalTgl = NGGpuCapabilities::isTigerLake(this->deviceId) &&
			ngPhysicalGpuAccessAllowed();
		this->isRealTGL = NGGpuCapabilities::useNativeTigerLakePath(this->deviceId, physicalTgl);
		SYSLOG("ngreen", "V242: CPU family=0x%x model=0x%x stepping=%u GPU=%04x nativeTglPf=%d",
			family, model, stepping, this->deviceId, this->isRealTGL);
	}

	const auto gms = WIOKit::readPCIConfigValue(this->iGPU,
		WIOKit::kIOPCIConfigGraphicsControl, 0, 16) >> 8;
	uint32_t decodedStolen = 0;
	if (!NGDvmt::decodeGen9Gms(static_cast<uint8_t>(gms), decodedStolen)) {
		SYSLOG("ngreen", "GMS 0x%x is reserved or exceeds the 32-bit framebuffer ABI; using 128 MiB floor",
			gms);
		decodedStolen = 128U * 1024 * 1024;
	}
	stolen_size = decodedStolen < 128U * 1024 * 1024 ? 128U * 1024 * 1024 : decodedStolen;
	SYSLOG("ngreen", "stolen_size 0x%x (GMS=0x%x)", stolen_size, gms);

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
	if (kextIOAcceleratorFamily2.loadIndex == index) {
		// Preserve native surface-mode validation and capability checks. These
		// are global user-client interfaces, not per-VF GuC scheduling bits.
		SYSLOG("ngreen", "IOAccelFamily2: preserving native capability and surface-mode validation");
	} else if (gen11.processKext(patcher, index, address, size)) {
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
