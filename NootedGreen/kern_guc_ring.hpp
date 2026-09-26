#pragma once
#include <stdint.h>

// Pure CTB framing checks shared by the kernel bridge and offline tests.
// Caller serializes the consumer and supplies a tail acquired from GuC.
namespace NGGuCRing {
// Pure accounting only; callers atomically publish the returned value.
inline bool reserveCredits(uint32_t used, uint32_t capacity, uint32_t count,
                           uint32_t &next) {
    if (used > capacity || count > capacity - used)
        return false;
    next = used + count;
    return true;
}

inline bool releaseCredits(uint32_t used, uint32_t capacity, uint32_t count,
                           uint32_t &next) {
    if (used > capacity || count > used)
        return false;
    next = used - count;
    return true;
}

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

// Caller acquires the producer tail before entry and publishes the returned
// head with a DMA barrier afterwards. Invalid input leaves output/head intact.
inline bool readFrame(const volatile uint32_t *buffer, uint32_t ringDwords,
                      uint32_t &head, uint32_t tail, uint32_t *output,
                      uint32_t outputDwords) {
    if (!buffer || !output || ringDwords < 2 || head >= ringDwords ||
        tail >= ringDwords || head == tail)
        return false;
    const uint32_t frame = buffer[head];
    if (!validFrame(frame, ringDwords, head, tail, outputDwords))
        return false;
    const uint32_t payload = frame & 0xFFU;
    uint32_t next = head;
    output[0] = frame;
    for (uint32_t i = 1; i <= payload; ++i) {
        next = (next + 1) % ringDwords;
        output[i] = buffer[next];
    }
    head = (next + 1) % ringDwords;
    return true;
}
}
