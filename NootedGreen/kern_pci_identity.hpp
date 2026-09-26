#pragma once
#include <stdint.h>

namespace NGPciIdentity {
inline uint32_t replaceDeviceId32(uint32_t original, uint8_t offset, uint32_t device) {
    if (device > 0xFFFFU)
        return original;
    if (offset == 0) // vendor ID low, device ID high
        return (original & 0xFFFFU) | (device << 16);
    if (offset == 2) // unaligned: device ID low, command register high
        return (original & 0xFFFF0000U) | device;
    return original;
}
}
