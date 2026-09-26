// Offline model of the inspected TGL initWithOptions loop bounds.
// This is not a GPU emulation or a test of page-table coherency.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../NootedGreen/kern_ggtt_bounds.hpp"

struct Bounds {
    uint64_t firstBegin, firstEnd;
    bool secondRuns;
    uint64_t secondBegin, secondEnd;
};

static Bounds nativeBounds(uint64_t start, uint64_t length) {
    return {start >> 12, (start + length - 1) >> 12,
            length != UINT64_C(0x100000000),
            (start + length) >> 12, (start + UINT64_C(0x100000000)) >> 12};
}

int main() {
    const uint64_t values[] = {0, 1, 0xFFF, 0x1000, 0x2000, 0x100000,
        0x40000000, 0xFEE00000, 0xFFFFF000, UINT64_C(0x100000000),
        UINT64_C(0x100001000), UINT64_MAX - 0xFFF, UINT64_MAX};
    unsigned rangeCases = 0;
    for (auto base : values) for (auto size : values)
    for (auto start : values) for (auto length : values) {
        using Wide = unsigned __int128;
        const bool expected = ((base | size | start | length) & 0xFFF) == 0 &&
            size > 0 && Wide(base) + size <= (Wide(1) << 32) &&
            start >= base && Wide(start) + length <= Wide(base) + size;
        assert(NGGgtt::contains(base, size, start, length) == expected);
        ++rangeCases;
    }
    std::printf("PASS: %u GGTT range cases against 128-bit arithmetic oracle\n", rangeCases);
    const auto suppressed = nativeBounds(UINT64_MAX, UINT64_C(0x100000000));
    assert(suppressed.firstBegin > suppressed.firstEnd);
    assert(!suppressed.secondRuns);
    // Regression: a valid nonzero VF assignment was incorrectly passed to
    // native init, whose clearing loop extends beyond the full PTE aperture.
    unsigned cases = 0;
    for (uint64_t base = 0x100000; base < 0xF0000000; base += 0x100000) {
        const auto unsafe = nativeBounds(base, 0x100000);
        assert(unsafe.secondRuns);
        assert(unsafe.secondEnd > 0x100000); // 8 MiB / 8-byte PTE
        ++cases;
    }
    std::printf("PASS: %u native GGTT init overflow models and suppressed loops\n", cases);
}
