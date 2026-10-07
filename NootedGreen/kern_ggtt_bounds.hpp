#ifndef NGREEN_GGTT_BOUNDS_HPP
#define NGREEN_GGTT_BOUNDS_HPP
#include <stdint.h>

namespace NGGgtt {
enum class TlbInvalidation : uint8_t {
    NotRequired,
    Required,
    Unsafe,
};

struct VfSegmentPlan {
    uint64_t globalStart;
    uint64_t globalLength;
    uint64_t unified32Start;
    uint64_t unified32Length;
    bool valid;
};

struct SynchronizationEntry {
    bool valid;
    bool map;
    uint64_t flags;
};

// Before the VF command transport has ever run, no GPU request can retain a
// translation and initialization rollback needs no TLB rendezvous. Once the
// transport has run, every unmap must complete a heavy GuC invalidation while
// the transport is still active. A stopped/faulted transport cannot prove DMA
// quiescence and must not permit the caller to release backing pages.
inline TlbInvalidation unmapInvalidation(bool everEnabled, bool enabled,
                                         bool stopped, bool faulted,
                                         bool deviceQuiesced = false) {
    // A completed shutdown invalidation is a stronger lifetime boundary than
    // the later CTB stopped/fault bookkeeping: no context remains registered
    // and no new GPU request can consume a translation.
    if (deviceQuiesced)
        return TlbInvalidation::NotRequired;
    if (!everEnabled)
        return TlbInvalidation::NotRequired;
    if (enabled && !stopped && !faulted)
        return TlbInvalidation::Required;
    return TlbInvalidation::Unsafe;
}

// The pinned Tahoe mapping contract supplies bits 38:12. The direct VF encoder
// is now explicit, but wider DMA/IOMMU addresses remain fail-closed until that
// private caller contract is independently established.
inline bool nativePhysicalRange(uint64_t physical, uint64_t length) {
    constexpr uint64_t limit = UINT64_C(1) << 39;
    return physical != 0 && ((physical | length) & 0xFFF) == 0 &&
           physical <= limit &&
           length <= limit - physical;
}

// A PF provisions an unused VF GGTT slot as Present|VFID with address zero.
// Tahoe's native global-table read decodes that owner tombstone as a present
// mapping at physical page zero, and its generic synchronizer then copies it
// into a PPGTT.  Treat only that global-source form as a sparse entry.  A
// present address-zero entry from a private table is corruption.  Direct VF
// GGTT stores deliberately discard Apple's physical-driver attributes because
// their low bits overlap the PF-owned VFID/LM fields, so the reverse copy must
// not reinterpret those owner bits as PPGTT cache attributes either.
static inline SynchronizationEntry classifySynchronizationEntry(bool present,
                                                                  bool globalSource,
                                                                  uint64_t physical,
                                                                  uint64_t flags) {
    if (!present)
        return {true, false, 0};
    if (globalSource && physical == 0)
        return {true, false, 0};
    if (!nativePhysicalRange(physical, UINT64_C(0x1000)))
        return {false, false, 0};
    return {true, true, globalSource ? 0 : flags};
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

// Tahoe's native initSegments reads physical stolen/BAR2 state, which a VF
// does not own. Build the three VF-visible ranges only from the PF-provisioned
// GGTT assignment. The 48-bit canonical PPGTT range is independent and stays
// at Apple's fixed [1 GiB, wrap-to-zero) representation.
inline VfSegmentPlan vfSegmentPlan(uint64_t base, uint64_t size) {
    VfSegmentPlan plan {0, 0, 0, 0, false};
    if (!contains(base, size, base, size))
        return plan;
    const uint64_t end = base + size;
    const uint64_t unifiedStart = base > UINT64_C(0x40000000) ?
                                  base : UINT64_C(0x40000000);
    const uint64_t unifiedEnd = end < UINT64_C(0xFE000000) ?
                                end : UINT64_C(0xFE000000);
    if (unifiedEnd <= unifiedStart)
        return plan;
    plan.globalStart = base;
    plan.globalLength = size;
    plan.unified32Start = unifiedStart;
    plan.unified32Length = unifiedEnd - unifiedStart;
    plan.valid = true;
    return plan;
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
