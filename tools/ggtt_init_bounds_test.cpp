// Offline model of the inspected TGL initWithOptions loop bounds.
// This is not a GPU emulation or a test of page-table coherency.
#include <cassert>
#include <cstdint>
#include <cstdio>

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
