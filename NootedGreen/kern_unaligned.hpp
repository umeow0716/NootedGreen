#pragma once
#include <stdint.h>

namespace NGUnaligned {
// Little-endian byte assembly avoids alignment and strict-aliasing assumptions.
// Caller still owns readability and lifetime of all four bytes.
inline uint32_t readLe32(const void *address) {
    const auto *bytes = static_cast<const uint8_t *>(address);
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(bytes[i]) << (8 * i);
    return value;
}
}
