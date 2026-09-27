#pragma once
#include <stdint.h>

namespace NGPciIdentity {
constexpr uint32_t kExtendedRegisterMask = 0x0F000000U;

inline bool isPrimaryConfigPage(uint32_t space) {
    return (space & kExtendedRegisterMask) == 0;
}

inline bool readsDeviceId16(uint32_t space, uint8_t offset) {
    // PCI 16-bit accesses ignore address bit 0.
    return isPrimaryConfigPage(space) && (offset & 0xFEU) == 2U;
}

inline bool readsVendorDevice32(uint32_t space, uint8_t offset) {
    // PCI 32-bit accesses ignore address bits 1:0. In particular, Apple's
    // TGL probe calls extendedConfigRead32(2), which still returns the aligned
    // vendor/device DWORD rather than a sliding device/command window.
    return isPrimaryConfigPage(space) && (offset & 0xFCU) == 0;
}

inline uint16_t replaceDeviceId16(uint16_t original, uint32_t space,
                                  uint8_t offset, uint32_t device) {
    if (device > 0xFFFFU || !readsDeviceId16(space, offset))
        return original;
    return static_cast<uint16_t>(device);
}

inline uint32_t replaceDeviceId32(uint32_t original, uint32_t space,
                                  uint8_t offset, uint32_t device) {
    if (device > 0xFFFFU || !readsVendorDevice32(space, offset))
        return original;
    return (original & 0xFFFFU) | (device << 16);
}
}
