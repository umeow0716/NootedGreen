#ifndef NGREEN_GGTT_BOUNDS_HPP
#define NGREEN_GGTT_BOUNDS_HPP
#include <stdint.h>

namespace NGGgtt {
// Absolute GPU addresses index the full 8 MiB / 8-byte PTE aperture.
// Subtraction avoids overflow even for malformed native caller ranges.
inline bool contains(uint64_t base, uint64_t size, uint64_t start, uint64_t length) {
    constexpr uint64_t aperture = UINT64_C(0x100000000);
    if (((base | size | start | length) & 0xFFF) != 0 || !size ||
        base >= aperture || size > aperture - base || start < base ||
        length > size)
        return false;
    return start - base <= size - length;
}
}
#endif
