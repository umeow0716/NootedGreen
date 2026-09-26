#include "../NootedGreen/kern_pattern_match.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    size_t result = 999;
    const uint8_t zero = 0;
    assert(!NGPattern::findUnique(nullptr, 1, &zero, nullptr, 1, result));
    assert(!NGPattern::findUnique(&zero, 1, nullptr, nullptr, 1, result));
    assert(!NGPattern::findUnique(&zero, 1, &zero, nullptr, 0, result));
    assert(!NGPattern::findUnique(&zero, 1, &zero, nullptr, 2, result));
    assert(!NGPattern::findUnique(&zero, 1, &zero, &zero, 1, result));
    assert(result == 999);
    unsigned cases = 0;
    for (size_t length = 0; length <= 8; ++length)
    for (unsigned bits = 0; bits < (1U << length); ++bits)
    for (size_t width = 1; width <= 4; ++width)
    for (unsigned needle = 0; needle < (1U << width); ++needle)
    for (unsigned maskBits = 0; maskBits < (1U << width); ++maskBits) {
        // Exact-size heap allocations let ASan catch reads past either input.
        std::vector<uint8_t> data(length), pattern(width), mask(width);
        for (size_t i = 0; i < length; ++i) data[i] = (bits >> i) & 1;
        for (size_t i = 0; i < width; ++i) {
            pattern[i] = (needle >> i) & 1;
            mask[i] = (maskBits >> i) & 1 ? 0xFF : 0;
        }
        unsigned matches = 0;
        size_t expected = 999;
        if (maskBits) for (size_t offset = 0; offset + width <= length; ++offset) {
            bool equal = true;
            for (size_t i = 0; i < width; ++i)
                if ((data[offset + i] & mask[i]) != pattern[i]) equal = false;
            if (equal) { ++matches; expected = offset; }
        }
        result = 999;
        const bool found = NGPattern::findUnique(data.data(), data.size(),
            pattern.data(), mask.data(), width, result);
        assert(found == (matches == 1));
        assert(result == (found ? expected : 999));
        unsigned nonoverlapping = 0;
        if (maskBits) for (size_t offset = 0; offset + width <= length;) {
            bool equal = true;
            for (size_t i = 0; i < width; ++i)
                if ((data[offset + i] & mask[i]) != pattern[i]) equal = false;
            if (equal) { ++nonoverlapping; offset += width; }
            else ++offset;
        }
        for (unsigned required = 0; required <= 5; ++required)
            assert(NGPattern::hasAtLeast(data.data(), data.size(), pattern.data(),
                mask.data(), width, required) == (required && nonoverlapping >= required));
        ++cases;
    }
    const uint8_t data[] = {0xA1, 0xB2, 0xC3};
    const uint8_t pattern[] = {0xA0, 0xB0}, mask[] = {0xF0, 0xF0};
    assert(NGPattern::findUnique(data, 3, pattern, mask, 2, result) && result == 0);
    const uint8_t unmaskedPattern[] = {0xAF, 0xB9};
    assert(!NGPattern::findUnique(data, 3, unmaskedPattern, mask, 2, result));
    assert(NGPattern::findUnique(data, 3, data + 2, nullptr, 1, result) && result == 2);
    assert(NGPattern::hasAtLeast(data, 3, data + 2, nullptr, 1, 1));
    assert(!NGPattern::hasAtLeast(data, 3, data + 2, nullptr, 1, 2));
    assert(!NGPattern::hasAtLeast(nullptr, 3, data, nullptr, 1, 1));
    assert(!NGPattern::hasAtLeast(data, 3, nullptr, nullptr, 1, 1));
    std::printf("PASS: %u unique/missing/ambiguous masked-pattern cases\n", cases);
}
