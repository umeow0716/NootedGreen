#ifndef NGREEN_GGTT_BOUNDS_HPP
#define NGREEN_GGTT_BOUNDS_HPP
#include <stdint.h>

namespace NGGgtt {
enum class TlbInvalidation : uint8_t {
    NotRequired,
    Required,
    Unsafe,
};

// Before the VF command transport has ever run, no GPU request can retain a
// translation and initialization rollback needs no TLB rendezvous. Once the
// transport has run, every unmap must complete a heavy GuC invalidation while
// the transport is still active. A stopped/faulted transport cannot prove DMA
// quiescence and must not permit the caller to release backing pages.
inline TlbInvalidation unmapInvalidation(bool everEnabled, bool enabled,
                                         bool stopped, bool faulted) {
    if (!everEnabled)
        return TlbInvalidation::NotRequired;
    if (enabled && !stopped && !faulted)
        return TlbInvalidation::Required;
    return TlbInvalidation::Unsafe;
}

// The inspected Apple mapper masks DMA addresses to bits 38:12. Until that
// encoder is replaced/validated, reject inputs it would silently truncate.
inline bool nativePhysicalRange(uint64_t physical, uint64_t length) {
    constexpr uint64_t limit = UINT64_C(1) << 39;
    return ((physical | length) & 0xFFF) == 0 && physical <= limit &&
           length <= limit - physical;
}
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

// Validate BOTH address spaces before touching an unpublished mapped buffer.
// gpuTop is exclusive. No narrowing to native 32-bit descriptor fields yet.
inline bool mappedBacking(uint64_t cpu, uint64_t allocated, uint64_t required,
                          uint64_t gpu, uint64_t base, uint64_t size,
                          uint64_t gpuTop) {
    return cpu && !(cpu & 0xFFF) && required && allocated >= required &&
           required <= UINT64_MAX - cpu && gpu && gpu < gpuTop &&
           required <= gpuTop - gpu && contains(base, size, gpu, required);
}
}
#endif
