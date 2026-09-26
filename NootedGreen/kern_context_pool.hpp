#pragma once
#include <stddef.h>
#include <stdint.h>

// TGL legacy proxy bookkeeping only, NOT a modern GuC context descriptor.
// The caller owns the native GuC context-pool lock throughout this operation.
namespace NGContextPool {
constexpr uint32_t invalidId = 0x400;
constexpr size_t stride = 0x5B00;
constexpr size_t flagsOffset = 0x5A6C;
enum class Result { Allocated, Full, Invalid };

// Validate the complete CPU interval before any record pointer arithmetic.
// This checks representability, not whether the caller's mapping is resident.
inline bool validStorage(const uint8_t *pool, uint64_t bytes, uint32_t count) {
    if (!pool || !count || count > invalidId)
        return false;
    const uint64_t required = static_cast<uint64_t>(count) * stride;
    return bytes >= required &&
        required <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(pool);
}

// Caller owns the same native pool lock as allocate(). This retires ONLY the
// legacy proxy reservation, not its DMA backing or modern GuC context ID.
inline bool release(uint8_t *pool, uint64_t bytes, uint32_t count,
                    uint32_t &used, uint32_t id) {
    if (!validStorage(pool, bytes, count) || !used || used > count || id >= count)
        return false;
    auto &flags = pool[static_cast<size_t>(id) * stride + flagsOffset];
    if (!(flags & 1U))
        return false;
    flags &= ~1U;
    --used;
    return true;
}

inline Result allocate(uint8_t *pool, uint64_t bytes, uint32_t count,
                       uint32_t &used, uint32_t &next, bool clear,
                       uint32_t &result) {
    if (!validStorage(pool, bytes, count) || used > count ||
        (next >= count && next != invalidId))
        return Result::Invalid;
    if (used == count)
        return Result::Full;
    uint32_t id = next == invalidId ? 0 : next;
    for (uint32_t scanned = 0; scanned < count; ++scanned) {
        auto *record = pool + static_cast<size_t>(id) * stride;
        if (!(record[flagsOffset] & 1U)) {
            if (clear)
                for (size_t byte = 0; byte < stride; ++byte)
                    record[byte] = 0;
            record[flagsOffset] |= 1U;
            ++used;
            next = id + 1 == count ? 0 : id + 1;
            result = id;
            return Result::Allocated;
        }
        id = id + 1 == count ? 0 : id + 1;
    }
    // Occupancy disagrees with the caller's allocated count. No writes made.
    return Result::Invalid;
}
}
