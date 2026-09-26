#pragma once
#include <stdint.h>
#include "kern_unaligned.hpp"

namespace NGContextDescriptor {
struct Value {
    uint32_t low;
    uint32_t high;
};

// The pinned native descriptor is a packed member at context+0x89.
// Caller guarantees eight live readable bytes and stable descriptor ownership.
// Byte loads avoid imposing uint32_t alignment on that native representation.
// This is not an atomic snapshot or an object-provenance check.
inline Value read(const void *descriptor) {
    const auto *bytes = static_cast<const uint8_t *>(descriptor);
    return {NGUnaligned::readLe32(bytes), NGUnaligned::readLe32(bytes + 4)};
}
}
