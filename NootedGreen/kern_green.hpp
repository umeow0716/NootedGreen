//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_iokit.hpp>
#include <IOKit/pci/IOPCIDevice.h>

// Fail closed for VF or unclassified identity before physical display access.
bool ngPhysicalGpuAccessAllowed();
// PFs may use their complete BAR; VFs may use only i915's fixed MMIO allowlist.
bool ngGpuRegisterAccessAllowed(unsigned long reg);

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
	WIOKit::t_PCIConfigRead16 orgConfigRead16 {nullptr};
	WIOKit::t_PCIConfigRead32 orgConfigRead32 {nullptr};
	
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

	private:
    bool isRealTGL = false;  // compatibility name: true only for a physical TGL GPU
public:
    // Captured from PCI configuration before installing our ID spoof hooks.
    uint32_t getOriginalDeviceId() const { return deviceId; }
private:
    uint32_t deviceId {0};
    IOPCIDevice *iGPU {nullptr};
	
	IOMemoryMap *rmmio {nullptr};
	volatile UInt32 *rmmioPtr {nullptr};

};
