#include "../NootedGreen/kern_context_pool.hpp"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

using namespace NGContextPool;
int main() {
    size_t cases = 0;
    for (uint32_t count = 1; count <= 8; ++count) {
        const size_t bytes = count * stride;
        std::vector<uint8_t> storage(bytes + 2, 0xA4);
        auto *pool = storage.data() + 1;
        for (uint32_t mask = 0; mask < (1U << count); ++mask) {
            for (uint32_t start = 0; start <= count; ++start) {
                for (bool clear : {false, true}) {
                    std::fill(storage.begin(), storage.end(), 0xA4);
                    uint32_t used = 0;
                    for (uint32_t id = 0; id < count; ++id) {
                        pool[id * stride + flagsOffset] |= (mask >> id) & 1;
                        used += (mask >> id) & 1;
                    }
                    const auto before = storage;
                    uint32_t next = start == count ? invalidId : start;
                    const uint32_t oldUsed = used, oldNext = next;
                    uint32_t expected = invalidId;
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t candidate = ((start == count ? 0 : start) + i) % count;
                        if (!(mask & (1U << candidate))) { expected = candidate; break; }
                    }
                    uint32_t id = invalidId;
                    auto result = allocate(pool, bytes, count, used, next, clear, id);
                    assert(id == expected);
                    if (expected == invalidId) {
                        assert(result == Result::Full && used == oldUsed && next == oldNext);
                        assert(storage == before);
                    } else {
                        assert(result == Result::Allocated && used == oldUsed + 1);
                        assert(next == (id + 1) % count);
                        for (size_t i = 0; i < bytes; ++i) {
                            uint8_t value = before[i + 1];
                            if (i / stride == id && clear) value = 0;
                            if (i == id * stride + flagsOffset) value |= 1;
                            assert(pool[i] == value);
                        }
                    }
                    assert(storage.front() == 0xA4 && storage.back() == 0xA4);
                    ++cases;
                }
            }
        }
    }
    // Entire 1024-entry namespace: exhaust, release, reuse after old sentinel.
    std::vector<uint8_t> full(invalidId * stride, 0);
    uint32_t used = 0, next = 0, id = invalidId;
    for (uint32_t i = 0; i < invalidId; ++i) {
        assert(allocate(full.data(), full.size(), invalidId, used, next, true, id) == Result::Allocated);
        assert(id == i);
    }
    assert(allocate(full.data(), full.size(), invalidId, used, next, false, id) == Result::Full);
    full[17 * stride + flagsOffset] &= ~1U;
    --used;
    next = invalidId;
    assert(allocate(full.data(), full.size(), invalidId, used, next, false, id) == Result::Allocated);
    assert(id == 17 && used == invalidId);

    // Invalid inputs/short backing/inconsistent occupancy make no writes.
    std::vector<uint8_t> small(stride, 0);
    for (unsigned variant = 0; variant < 7; ++variant) {
        used = variant == 3 ? 2 : 0;
        next = variant == 4 ? 1 : 0;
        id = 0x12345678;
        small[flagsOffset] = variant == 6 ? 1 : 0;
        const auto before = small;
        const uint32_t oldUsed = used, oldNext = next;
        assert(allocate(variant == 0 ? nullptr : small.data(),
                        variant == 5 ? stride - 1 : stride,
                        variant == 1 ? 0 : variant == 2 ? invalidId + 1 : 1,
                        used, next, true, id) == Result::Invalid);
        assert(small == before && used == oldUsed && next == oldNext && id == 0x12345678);
    }
    printf("PASS %zu exhaustive proxy allocations; 1024-entry reuse and invalid-input checks\n", cases);
}
