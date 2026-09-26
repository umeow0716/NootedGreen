#pragma once
#include <stdint.h>

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
    Value value {0, 0};
    for (unsigned i = 0; i < 4; ++i) {
        value.low |= static_cast<uint32_t>(bytes[i]) << (8 * i);
        value.high |= static_cast<uint32_t>(bytes[i + 4]) << (8 * i);
    }
    return value;
}
}
