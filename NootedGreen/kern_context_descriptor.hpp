#pragma once
#include <stdint.h>
#include "kern_unaligned.hpp"

namespace NGContextDescriptor {
struct Value {
    uint32_t low;
    uint32_t high;
};

// Gen8-Gen12 hardware context-descriptor fields.  Tahoe's
// IGHardwareContext::initWithOptions builds the persistent low dword from
// 0x309, replaces address-mode bits 4:3, and conditionally toggles coherent
// bit 5 and privilege bit 8.  It never stores either restore bit in the
// object.  IGHardwareCommandStreamer5::submitExecList adds FORCE_RESTORE only
// to a transient stack copy on first submission.
constexpr uint32_t addressMask = 0xFFFFF000U;
constexpr uint32_t valid = 1U << 0;
constexpr uint32_t forceRestore = 1U << 2;
constexpr uint32_t addressModeMask = 3U << 3;
constexpr uint32_t coherent = 1U << 5;
constexpr uint32_t privilege = 1U << 8;
constexpr uint32_t priorityMask = 3U << 9;
constexpr uint32_t normalPriority = 1U << 9;
constexpr uint32_t persistentFlagMask = valid | addressModeMask | coherent |
    privilege | priorityMask;

// Gen11 upper descriptor: SW ID 47:37, engine instance 53:48, MBZ 54,
// SW counter 60:55, engine class 63:61.  Tahoe creates no SW counter and its
// bit-54 preserve operation starts from a zero-initialized descriptor.
constexpr uint32_t swContextIdMaskHigh = 0x0000FFE0U;
constexpr uint32_t engineInstanceMaskHigh = 0x003F0000U;
constexpr uint32_t engineClassMaskHigh = 0xE0000000U;
constexpr uint32_t persistentHighMask = swContextIdMaskHigh |
    engineInstanceMaskHigh | engineClassMaskHigh;

struct Attributes {
    bool valid;
    uint32_t lrcaPage;
    uint32_t rawClass;
    uint32_t engineInstance;
    uint8_t gucClass;
};

// The pinned native descriptor is a packed member at context+0x89.
// Caller guarantees eight live readable bytes and stable descriptor ownership.
// Byte loads avoid imposing uint32_t alignment on that native representation.
// This is not an atomic snapshot or an object-provenance check.
inline Value read(const void *descriptor) {
    const auto *bytes = static_cast<const uint8_t *>(descriptor);
    return {NGUnaligned::readLe32(bytes), NGUnaligned::readLe32(bytes + 4)};
}

inline bool validPersistentLow(uint32_t low)
{
    const uint32_t flags = low & ~addressMask;
    return (low & addressMask) != 0 &&
        (flags & ~persistentFlagMask) == 0 &&
        (flags & valid) != 0 &&
        (flags & priorityMask) == normalPriority;
}

inline uint8_t mapEngineClass(uint32_t rawClass)
{
    // Intel engine classes {RCS,VCS,VECS,BCS,OTHER,CCS} map to the GuC
    // classes {0,1,2,3,5,4}; Tahoe's native AttachContext uses this table too.
    constexpr uint8_t map[] = {0, 1, 2, 3, 5, 4};
    return rawClass < sizeof(map) ? map[rawClass] : 0xFFU;
}

inline Attributes inspect(Value descriptor)
{
    Attributes result = {};
    result.lrcaPage = descriptor.low & addressMask;
    result.rawClass = descriptor.high >> 29;
    result.engineInstance = (descriptor.high >> 16) & 0x3FU;
    result.gucClass = mapEngineClass(result.rawClass);
    result.valid = validPersistentLow(descriptor.low) &&
        (descriptor.high & ~persistentHighMask) == 0 &&
        result.gucClass != 0xFFU && result.engineInstance < 32;
    return result;
}

inline uint32_t gucHwlrca(Value descriptor)
{
    // GuC v70 keeps the LRCA from registration and later schedule actions do
    // not carry a descriptor.  Preserve Apple's proven fields and materialize
    // the first-submit FORCE_RESTORE that its legacy path adds transiently.
    return descriptor.low | forceRestore;
}

inline bool validGucHwlrca(uint32_t low, uint32_t high)
{
    if (high != 0 || (low & forceRestore) == 0)
        return false;
    return validPersistentLow(low & ~forceRestore);
}
}
