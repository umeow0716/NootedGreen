//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include "kern_gen11.hpp"
#include "kern_genx.hpp"
#include "kern_model.hpp"
#include "DYLDPatches.hpp"
#include "kern_patcherplus.hpp"
#include "kern_pci_identity.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_dvmt_patch.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <i386/machine_routines.h>
#include <kern/sched_prim.h>


static const char *pathIOAcceleratorFamily2= "/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2";
static const char *pathAGDP = "/System/Library/Extensions/AppleGraphicsControl.kext/Contents/PlugIns/"
							  "AppleGraphicsDevicePolicy.kext/Contents/MacOS/AppleGraphicsDevicePolicy";

static KernelPatcher::KextInfo kextAGDP {"com.apple.driver.AppleGraphicsDevicePolicy", &pathAGDP, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextIOAcceleratorFamily2 { "com.apple.iokit.IOAcceleratorFamily2", &pathIOAcceleratorFamily2, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded };

NGreen *NGreen::callback = nullptr;

static Genx genx;
static Gen11 gen11;
static DYLDPatches dyldpatches;

static uint8_t builtin2[] = {0x00, 0x00, 0x49, 0x9A};
static uint8_t builtin3[] = {0x49, 0x9A, 0x00, 0x00};

static bool isLegacyIGPUPropSeedingEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenforceprops", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenforceprops");
}

static bool seedIGPUPropertiesOnEntry(IORegistryEntry *entry, bool &failed) {
	failed = false;
	if (!entry) {
		failed = true;
		return false;
	}

	bool changed = false;
	auto record = [&](bool result) {
		changed |= result;
		failed |= !result;
	};

	// Default fallback platform-id for RPL/TGL spoof bring-up. Only inject when missing.
	if (!entry->getProperty("AAPL,ig-platform-id")) {
		record(entry->setProperty("AAPL,ig-platform-id", builtin2, arrsize(builtin2)));
	}

	if (!entry->getProperty("device-id")) {
		record(entry->setProperty("device-id", builtin3, arrsize(builtin3)));
	}

	if (!entry->getProperty("built-in")) {
		static uint8_t builtin[] = {0x00};
		record(entry->setProperty("built-in", builtin, arrsize(builtin)));
	}

	if (!entry->getProperty("AAPL,slot-name")) {
		record(entry->setProperty("AAPL,slot-name", const_cast<char *>("built-in"), 9));
	}

	if (!entry->getProperty("hda-gfx")) {
		record(entry->setProperty("hda-gfx", const_cast<char *>("onboard-1"), 10));
	}

	if (!entry->getProperty("model")) {
		record(entry->setProperty("model", const_cast<char *>("Intel Iris Xe Graphics"), 23));
	}

	if (!entry->getProperty("framebuffer-unifiedmem")) {
		static uint8_t unifiedMem[] = {0x00, 0x00, 0x00, 0x60}; // 1536 MB
		record(entry->setProperty("framebuffer-unifiedmem", unifiedMem, arrsize(unifiedMem)));
	}

	if (!entry->getProperty("saved-config")) {
		static uint8_t sconf[0xEA] = {};
		record(entry->setProperty("saved-config", sconf, sizeof(sconf)));
	}

	return changed;
}

static void seedIGPUPropertiesEarly() {
	// Try common ACPI namespace variants used by laptop firmware before DeviceInfo scans.
	const char *paths[] = {
		"IOService:/AppleACPIPlatformExpert/PC00@0/IGPU@2",
		"IOService:/AppleACPIPlatformExpert/PC00@0/GFX0@2",
		"IOService:/AppleACPIPlatformExpert/PCI0@0/IGPU@2",
		"IOService:/AppleACPIPlatformExpert/PCI0@0/GFX0@2"
	};

	bool found = false;
	bool changed = false;
	bool failed = false;
	for (auto path : paths) {
		auto *entry = IORegistryEntry::fromPath(path, gIOServicePlane);
		if (!entry) {
			continue;
		}
		found = true;
		bool entryFailed = false;
		if (seedIGPUPropertiesOnEntry(entry, entryFailed)) {
			changed = true;
		}
		failed |= entryFailed;
		entry->release();
	}

	if (found) {
		SYSLOG("ngreen", "Early IGPU pre-seed via IOService path: changed=%d failed=%d", changed, failed);
	} else {
		SYSLOG("ngreen", "Early IGPU pre-seed skipped: no IGPU path resolved before DeviceInfo");
	}
}

void NGreen::init() {
    callback = this;
	
	lilu.onKextLoadForce(&kextAGDP);
	lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
	
	genx.init();
	gen11.init();
	dyldpatches.init();
	
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
	// Hook _cs_validate_page FIRST — before DeviceInfo which blocks
	// for >60s polling PEGP (NVIDIA dGPU) disable via processSwitchOff.
	// Without this, WindowServer starts before the hook is established
	// and CoreDisplay loads unpatched from the shared cache.
	if (!checkKernelArgument("-nbdyldoff")) {
		dyldpatches.processPatcher(patcher);
	} else {
		DBGLOG("ngreen", "DYLD patches disabled by boot argument -nbdyldoff");
	}

	// Compatibility-first default: do not force-inject IGPU properties unless explicitly requested.
	if (isLegacyIGPUPropSeedingEnabled()) {
		seedIGPUPropertiesEarly();
	} else {
		SYSLOG("ngreen", "compat mode: legacy IGPU property seeding disabled (use -ngreenforceprops to enable)");
	}

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
	if (isLegacyIGPUPropSeedingEnabled()) {
		bool failed = false;
		const bool changed = seedIGPUPropertiesOnEntry(this->iGPU, failed);
		SYSLOG("ngreen", "IGPU compatibility properties: changed=%d failed=%d", changed, failed);
	}

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

bool NGreen::getAperture(volatile UInt32 *&address, uint64_t &length) {
	address = nullptr;
	length = 0;
	if (!this->iGPU || !ngPhysicalGpuAccessAllowed())
		return false;
	auto *mapping = this->aperture;
	if (!mapping) {
		if (ml_at_interrupt_context() || !preemption_enabled())
			return false;
		mapping = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
		if (!mapping) {
			SYSLOG("ngreen", "V201: BAR2 aperture map FAILED");
			return false;
		}
		const auto candidateAddress = mapping->getVirtualAddress();
		if (!candidateAddress || (candidateAddress & 3U) || mapping->getLength() < sizeof(uint32_t)) {
			mapping->release();
			SYSLOG("ngreen", "V201: BAR2 aperture map invalid");
			return false;
		}
		if (!OSCompareAndSwapPtr(nullptr, mapping, &this->aperture)) {
			mapping->release();
		} else {
			SYSLOG("ngreen", "V201: BAR2 aperture mapped, len=0x%llx", mapping->getLength());
		}
		mapping = this->aperture;
	}
	const auto mappingLength = mapping->getLength();
	const auto mappingAddress = mapping->getVirtualAddress();
	if (!mappingAddress || (mappingAddress & 3U) || mappingLength < sizeof(uint32_t))
		return false;
	address = reinterpret_cast<volatile UInt32 *>(mappingAddress);
	length = mappingLength;
	return true;
}


bool NGreen::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (kextIOAcceleratorFamily2.loadIndex == index) {
		// Preserve native surface-mode validation and capability checks. These
		// are global user-client interfaces, not per-VF GuC scheduling bits.
		SYSLOG("ngreen", "IOAccelFamily2: preserving native capability and surface-mode validation");
	} else if (kextAGDP.loadIndex == index) {
		const LookupPatchPlus patch {&kextAGDP, kAGDPBoardIDKeyOriginal, kAGDPBoardIDKeyPatched, 1};
		SYSLOG_COND(!patch.apply(patcher, address, size), "NGreen", "Failed to apply AGDP board-id patch");
	} else if (genx.processKext(patcher, index, address, size)) {
		DBGLOG("ngreen", "Processed Generation x configuration");
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
