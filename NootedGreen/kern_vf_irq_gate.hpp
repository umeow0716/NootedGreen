#pragma once

#include <stdint.h>

namespace NGVfIrqGate {

constexpr uint32_t closedBit = 0x80000000U;
constexpr uint32_t activeMask = closedBit - 1U;

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
