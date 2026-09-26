#include "../NootedGreen/kern_context_pool.hpp"
#include "../NootedGreen/kern_context_descriptor.hpp"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

using namespace NGContextPool;
int main() {
    for (unsigned offset = 0; offset < 16; ++offset) {
        std::vector<uint8_t> packed(offset + 8, 0xA5);
        for (unsigned bit = 0; bit < 64; ++bit) {
            std::fill(packed.begin() + offset, packed.end(), 0);
            packed[offset + bit / 8] = 1U << (bit % 8);
            const auto before = packed;
            const auto value = NGContextDescriptor::read(packed.data() + offset);
            assert(value.low == (bit < 32 ? uint32_t(1) << bit : 0));
            assert(value.high == (bit >= 32 ? uint32_t(1) << (bit - 32) : 0));
            assert(packed == before);
        }
    }
    puts("PASS 1024 packed descriptor alignment/bit cases");
    // Synthetic addresses are inspected only as integers, never dereferenced.
    // Include a pool whose last byte fits but exclusive end wraps to zero.
    for (uint32_t count : {1U, 2U, invalidId}) {
        const uintptr_t required = static_cast<uintptr_t>(count) * stride;
        auto *boundary = reinterpret_cast<uint8_t *>(UINTPTR_MAX - required);
        assert(validStorage(boundary, required, count));
        assert(!validStorage(boundary, required - 1, count));
        for (uintptr_t excess : {uintptr_t(1), required / 2, required}) {
            auto *wrapped = reinterpret_cast<uint8_t *>(UINTPTR_MAX - required + excess);
            uint32_t used = 0, next = 0, id = 0x12345678;
            assert(allocate(wrapped, UINT64_MAX, count, used, next, true, id) == Result::Invalid);
            assert(used == 0 && next == 0 && id == 0x12345678);
            used = 1;
            assert(!NGContextPool::release(wrapped, UINT64_MAX, count, used, 0));
            assert(used == 1);
        }
    }
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
    size_t releaseCases = 0;
    for (uint32_t count = 1; count <= 8; ++count) {
        std::vector<uint8_t> storage(count * stride + 2, 0xA4);
        auto *pool = storage.data() + 1;
        for (uint32_t mask = 0; mask < (1U << count); ++mask)
        for (uint32_t candidate = 0; candidate <= count; ++candidate) {
            std::fill(storage.begin(), storage.end(), 0xA4);
            uint32_t used = 0;
            for (uint32_t index = 0; index < count; ++index) {
                pool[index * stride + flagsOffset] |= (mask >> index) & 1;
                used += (mask >> index) & 1;
            }
            auto expected = storage;
            const uint32_t oldUsed = used;
            const bool allocated = candidate < count && ((mask >> candidate) & 1U);
            if (allocated) expected[1 + candidate * stride + flagsOffset] &= ~1U;
            assert(NGContextPool::release(pool, count * stride, count, used, candidate) == allocated);
            assert(storage == expected && used == oldUsed - (allocated ? 1 : 0));
            if (allocated) {
                assert(!NGContextPool::release(pool, count * stride, count, used, candidate));
                assert(storage == expected && used == oldUsed - 1);
            }
            ++releaseCases;
        }
    }
    for (unsigned variant = 0; variant < 8; ++variant) {
        std::fill(small.begin(), small.end(), 0xA5);
        const auto before = small;
        uint32_t used = variant == 2 ? 0 : variant == 3 ? 2 : 1;
        const auto oldUsed = used;
        assert(!NGContextPool::release(variant == 0 ? nullptr : small.data(),
            variant == 1 ? stride - 1 : stride,
            variant == 4 ? 0 : variant == 5 ? invalidId + 1 : 1, used,
            variant == 6 ? invalidId : variant == 7 ? UINT32_MAX : 0));
        assert(small == before && used == oldUsed);
    }
    printf("PASS %zu proxy allocations and %zu releases; exhaustion/reuse/invalid-input checks\n", cases, releaseCases);
}
