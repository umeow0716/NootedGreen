#include "../NootedGreen/kern_ggtt_rotation.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    uint64_t cases = 0;
    uint64_t pagesChecked = 0;
    for (uint32_t width = 1; width <= 64; ++width) {
        for (uint32_t height = 1; height <= 64; ++height) {
            const uint64_t pages = static_cast<uint64_t>(width) * height;
            NGGgttRotation::Spec spec {
                0x40000000ULL, 0x40000000ULL,
                0x50000000ULL, pages * 0x1000ULL, pages * 0x1000ULL,
                0x50000000ULL + (height - 1ULL) * 0x1000ULL,
                0, width, height,
            };
            assert(NGGgttRotation::valid(spec));
            std::vector<bool> seen(static_cast<size_t>(pages), false);
            for (uint64_t source = 0; source < pages; ++source) {
                uint64_t address = 0;
                assert(NGGgttRotation::destination(spec, source, address));
                assert(address >= spec.rangeStart);
                const uint64_t destination =
                    (address - spec.rangeStart) / 0x1000ULL;
                assert(destination < pages && !seen[destination]);
                seen[destination] = true;
                pagesChecked++;
            }
            uint64_t address = 0;
            assert(!NGGgttRotation::destination(spec, pages, address));
            for (bool present : seen)
                assert(present);
            cases++;
        }
    }

    const NGGgttRotation::Spec valid {
        0x10000000ULL, 0x20000000ULL,
        0x18000000ULL, 0x6000ULL, 0x6000ULL,
        0x18002000ULL, 0, 2, 3,
    };
    assert(NGGgttRotation::valid(valid));
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        auto malformed = valid;
        switch (mutation) {
            case 0: malformed.assignmentSize = 0; break;
            case 1: malformed.rangeStart++; break;
            case 2: malformed.rangeLength = 0; break;
            case 3: malformed.rangeLength += 0x1000; break;
            case 4: malformed.physicalLength -= 0x1000; break;
            case 5: malformed.cursor += 0x1000; break;
            case 6: malformed.sourcePage = 1; break;
            case 7: malformed.widthPages = 0; break;
            case 8: malformed.heightPages = 0; break;
            case 9: malformed.rangeStart = malformed.assignmentBase - 0x1000; break;
            case 10:
                malformed.widthPages = UINT32_MAX;
                malformed.heightPages = UINT32_MAX;
                break;
        }
        assert(!NGGgttRotation::valid(malformed));
    }

    std::printf("PASS: %llu rotation matrices and %llu unique destination pages\n",
                static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(pagesChecked));
}
