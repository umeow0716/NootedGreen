#ifndef NGREEN_PATTERN_MATCH_HPP
#define NGREEN_PATTERN_MATCH_HPP
#include <stddef.h>
#include <stdint.h>

namespace NGPattern {
// Read-only preflight for fallback function routing. Ambiguity is failure;
// offset zero is a valid match. Output is untouched on every failure.
inline bool findUnique(const uint8_t *data, size_t length,
                       const uint8_t *pattern, const uint8_t *mask,
                       size_t patternLength, size_t &result) {
    if (!data || !pattern || !patternLength || patternLength > length)
        return false;
    if (mask) {
        bool constrained = false;
        for (size_t i = 0; i < patternLength; ++i)
            constrained |= mask[i] != 0;
        if (!constrained) return false;
    }
    bool found = false;
    size_t candidate = 0;
    for (size_t offset = 0; offset <= length - patternLength; ++offset) {
        size_t i = 0;
        for (; i < patternLength; ++i)
            // Match Lilu's ABI: patterns must already be masked, rather than
            // silently discarding unconstrained bits from the pattern itself.
            if ((data[offset + i] & (mask ? mask[i] : 0xFF)) != pattern[i])
                break;
        if (i != patternLength) continue;
        if (found) return false;
        found = true;
        candidate = offset;
    }
    if (found) result = candidate;
    return found;
}
}
#endif
