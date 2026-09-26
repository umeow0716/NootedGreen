#pragma once
#include <stdint.h>

// Pure CTB framing checks shared by the kernel bridge and offline tests.
// Caller serializes the consumer and supplies a tail acquired from GuC.
namespace NGGuCRing {
inline bool validDescriptor(uint32_t bytes, uint32_t expectedBytes,
                            uint32_t head, uint32_t tail, uint32_t status) {
    return expectedBytes >= 8 && !(expectedBytes & 3U) &&
           bytes == expectedBytes && !status &&
           head < expectedBytes / 4 && tail < expectedBytes / 4;
}

inline bool validFrame(uint32_t frame, uint32_t ringDwords, uint32_t head,
                      uint32_t tail, uint32_t outputDwords) {
    if (ringDwords < 2 || head >= ringDwords || tail >= ringDwords ||
        outputDwords < 2 || (frame & 0xFF00U))
        return false;
    const uint32_t payload = frame & 0xFFU;
    const uint32_t available = tail >= head ? tail - head : ringDwords - head + tail;
    return payload && payload < outputDwords && payload < available;
}
}
