//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_iokit.hpp>
#include <IOKit/pci/IOPCIDevice.h>

// Fail closed for VF or unclassified identity before physical display access.
bool ngPhysicalGpuAccessAllowed();
// Positive VF ownership only; unlike the physical-access gate, Unknown is not
// admitted.  Userspace compatibility patches must never run on an unclassified
// physical function.
bool ngVirtualGpuAccessAllowed();
// PFs may use their complete BAR; VFs may use only i915's fixed MMIO allowlist.
bool ngGpuRegisterAccessAllowed(unsigned long reg);
// Observation-only: enable a bounded KAUTH exec-path capture after the exact
// TGL accelerator payload is fully patched and published on the classified VF.
void ngArmV349MediaExecObservation();

class NGreen {
    friend class Gen11;

    public:
    static NGreen *callback;
    void init();
    void processPatcher(KernelPatcher &patcher);
	bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
	bool setRMMIOIfNecessary();
	
	static uint16_t configRead16(IORegistryEntry *service, uint32_t space, uint8_t offset);
	static uint32_t configRead32(IORegistryEntry *service, uint32_t space, uint8_t offset);
	static bool setBusMasterEnable(IOPCIDevice *device, bool enable);
	WIOKit::t_PCIConfigRead16 orgConfigRead16 {nullptr};
	WIOKit::t_PCIConfigRead32 orgConfigRead32 {nullptr};
	using t_SetBusMasterEnable = bool (*)(IOPCIDevice *, bool);
	t_SetBusMasterEnable orgSetBusMasterEnable {nullptr};
	
	// Checked BAR0 register access used by the VF GuC mailbox/doorbell path.
	UInt32 readReg32(unsigned long reg) {
		if (!rmmio || !rmmioPtr || (reg & 3U)) return 0xFFFFFFFFU;
		if (!ngGpuRegisterAccessAllowed(reg)) return 0xFFFFFFFFU;
		const auto bytes = this->rmmio->getLength();
		if (bytes >= sizeof(uint32_t) && reg <= bytes - sizeof(uint32_t)) {
			return this->rmmioPtr[reg >> 2];
		}
		// Intel BAR0 has no generic INDEX2/DATA2 fallback. Never turn an
		// out-of-range read into writes to unrelated offsets 0x38/0x3c.
		return 0xFFFFFFFFU;
	}

	// reg = byte offset (i915 convention). rmmioPtr is uint32_t* so divide by 4.
	void writeReg32(unsigned long reg, UInt32 val) {
		if (!rmmio || !rmmioPtr || (reg & 3U)) return;
		if (!ngGpuRegisterAccessAllowed(reg)) return;
		const auto bytes = this->rmmio->getLength();
		if (bytes < sizeof(uint32_t) || reg > bytes - sizeof(uint32_t)) return;
		this->rmmioPtr[reg >> 2] = val;
	}

	public:
	uint64_t getRMMIOLength() const { return rmmio ? rmmio->getLength() : 0; }
	volatile UInt32 *getRMMIOAddress() const { return rmmioPtr; }
	bool nativeTigerLakePath() const { return isRealTGL; }

	private:
    bool isRealTGL = false;  // compatibility name: true only for a physical TGL GPU
public:
    // Captured from PCI configuration before installing our ID spoof hooks.
    uint32_t getOriginalDeviceId() const { return deviceId; }
private:
	uint32_t deviceId {0};
	IOPCIDevice *iGPU {nullptr};
	bool driverReady {false};
	// The Tahoe IOGraphicsAccelerator2 base start enables PCI Bus Master before
	// Intel's HWS construction.  Keep the exact VF sink closed until Gen11 has
	// validated every native HWS mapping and is ready to allocate its sole MSI.
	volatile UInt32 vfBusMasterAdmission {0};
	volatile UInt32 vfEarlyBusMasterSuppressions {0};
	static constexpr size_t kSetBusMasterEnableVirtualOffset = 0x11B;
	bool openVfBusMasterAdmission() {
		OSSynchronizeIO();
		return OSCompareAndSwap(0, 1, &vfBusMasterAdmission);
	}
	void closeVfBusMasterAdmission() {
		OSCompareAndSwap(1, 0, &vfBusMasterAdmission);
		OSSynchronizeIO();
	}
	bool vfBusMasterAdmissionOpen() const {
		return vfBusMasterAdmission != 0;
	}
	
	IOMemoryMap *rmmio {nullptr};
	volatile UInt32 *rmmioPtr {nullptr};

};
