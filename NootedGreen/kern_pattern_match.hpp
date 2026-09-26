#ifndef NGREEN_PATTERN_MATCH_HPP
#define NGREEN_PATTERN_MATCH_HPP
#include <stddef.h>
#include <stdint.h>

namespace NGPattern {
// Lilu replacement skips overlapping matches and advances by patternLength
// (our replacement buffers have the same length). Require enough candidates
// before the first write; this is not synchronization against another patcher.
inline bool hasAtLeast(const uint8_t *data, size_t length,
                       const uint8_t *pattern, const uint8_t *mask,
                       size_t patternLength, size_t required) {
    if (!data || !pattern || !patternLength || patternLength > length || !required)
        return false;
    if (mask) {
        bool constrained = false;
        for (size_t i = 0; i < patternLength; ++i)
            constrained |= mask[i] != 0;
        if (!constrained) return false;
    }
    size_t offset = 0;
    while (offset <= length - patternLength) {
        size_t i = 0;
        for (; i < patternLength; ++i)
            if ((data[offset + i] & (mask ? mask[i] : 0xFF)) != pattern[i])
                break;
        if (i == patternLength) {
            if (--required == 0) return true;
            offset += patternLength;
        } else ++offset;
    }
    return false;
}

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
