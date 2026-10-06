#pragma once

#include <stdint.h>

namespace NGVfIrqGate {

constexpr uint32_t closedBit = 0x80000000U;
constexpr uint32_t activeMask = closedBit - 1U;

// Linux gen11_irq_reset() disables the GT master first, then clears every
// interrupt-enable bank and masks every source implemented by this RPL-P
// media-12 engine inventory. Tahoe registers its IOFilterInterruptEventSource
// before the native bridge enable boundary, and IOPCIFamily enables PCI MSI as
// a side effect of that registration. Keep this pre-MSI plan explicit so an
// inherited/stale VF source cannot target an uninitialized message address.
// All offsets are in i915's fixed VF-accessible BAR0 allowlist.
struct RegisterWrite {
    uint32_t offset;
    uint32_t value;
};

constexpr uint32_t masterRegister = 0x190010U;
constexpr uint32_t masterEnableBit = 0x80000000U;

constexpr RegisterWrite preMsiQuiescePlan[] = {
    {masterRegister, 0U},

    // GEN11_*_INTR_ENABLE plus the Gen12 CCS bank present on RPL-P.
    {0x190030U, 0U},
    {0x190034U, 0U},
    {0x190038U, 0U},
    {0x19003CU, 0U},
    {0x190040U, 0U},
    {0x190048U, 0U},

    // RCS0, BCS0, VCS0..3, VECS0/1, GuC, GPM, crypto and CCS0/1.
    {0x190090U, 0xFFFFFFFFU},
    {0x1900A0U, 0xFFFFFFFFU},
    {0x1900A8U, 0xFFFFFFFFU},
    {0x1900ACU, 0xFFFFFFFFU},
    {0x1900D0U, 0xFFFFFFFFU},
    {0x1900E8U, 0xFFFFFFFFU},
    {0x1900ECU, 0xFFFFFFFFU},
    {0x1900F0U, 0xFFFFFFFFU},
    {0x190100U, 0xFFFFFFFFU},
};

constexpr uint32_t preMsiQuiesceCount =
    sizeof(preMsiQuiescePlan) / sizeof(preMsiQuiescePlan[0]);

static inline bool masterDisabled(uint32_t value) {
    return value != 0xFFFFFFFFU && (value & masterEnableBit) == 0;
}

inline bool closed(uint32_t state) {
    return (state & closedBit) != 0;
}

inline uint32_t active(uint32_t state) {
    return state & activeMask;
}

inline bool enter(uint32_t state, uint32_t &next) {
    if (closed(state) || active(state) == activeMask)
        return false;
    next = state + 1U;
    return true;
}

inline bool leave(uint32_t state, uint32_t &next) {
    if (active(state) == 0)
        return false;
    next = state - 1U;
    return true;
}

inline uint32_t close(uint32_t state) {
    return state | closedBit;
}

inline bool drained(uint32_t state) {
    return closed(state) && active(state) == 0;
}

} // namespace NGVfIrqGate
