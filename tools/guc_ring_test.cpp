#include "../NootedGreen/kern_guc_ring.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

int main() {
    using namespace NGGuCRing;
    // An independently walked ring supplies the expected available length.
    // Exhaust all positions, all ABI lengths and several destination bounds.
    unsigned long checks = 0;
    for (uint32_t size = 2; size <= 33; ++size) {
        for (uint32_t head = 0; head < size; ++head) {
            for (uint32_t tail = 0; tail < size; ++tail) {
                uint32_t available = 0;
                for (uint32_t p = head; p != tail; p = (p + 1) % size)
                    ++available;
                for (uint32_t payload = 0; payload <= 255; ++payload) {
                    for (uint32_t capacity : {2U, 16U, 32U}) {
                        const bool expected = payload != 0 &&
                            payload + 1 <= available && payload + 1 <= capacity;
                        assert(validFrame(0xFFFF0000U | payload, size, head,
                                          tail, capacity) == expected);
                        assert(!validFrame(0x100U | payload, size, head, tail, capacity));
                        assert(!validFrame(0x1000U | payload, size, head, tail, capacity));
                        ++checks;
                    }
                }
            }
        }
    }
    assert(!validFrame(1, 0, 0, 0, 32));
    assert(!validFrame(1, 1024, 1024, 1, 32));
    assert(!validFrame(1, 1024, 0, 1024, 32));
    assert(!validFrame(1, 1024, 0, 2, 0));
    assert(validDescriptor(16384, 16384, 4095, 0, 0));
    assert(!validDescriptor(0, 16384, 0, 0, 0));
    assert(!validDescriptor(16384, 16384, 4096, 0, 0));
    assert(!validDescriptor(16384, 16384, 0, 4096, 0));
    assert(!validDescriptor(16384, 16384, 0, 0, 1));
    assert(!validDescriptor(16388, 16384, 0, 0, 0));
    std::printf("PASS: %lu CTB boundary cases plus malformed descriptor cases\n", checks);

    unsigned long copies = 0;
    constexpr uint32_t guard = 0xDEADBEEFU;
    for (uint32_t size = 2; size <= 33; ++size) {
        std::vector<uint32_t> ring(size);
        for (uint32_t head = 0; head < size; ++head) {
            for (uint32_t tail = 0; tail < size; ++tail) {
                uint32_t available = 0;
                for (uint32_t p = head; p != tail; p = p + 1 == size ? 0 : p + 1)
                    ++available;
                for (uint32_t payload = 0; payload <= 33; ++payload) {
                    for (uint32_t capacity : {2U, 16U, 32U}) {
                        for (uint32_t p = 0; p < size; ++p)
                            ring[p] = 0xA5000000U | p;
                        ring[head] = 0x12340000U | payload;
                        std::vector<uint32_t> output(capacity + 2, guard);
                        uint32_t cursor = head;
                        const bool expected = payload != 0 && payload + 1 <= available &&
                                              payload + 1 <= capacity;
                        assert(readFrame(ring.data(), size, cursor, tail,
                                         output.data() + 1, capacity) == expected);
                        uint32_t next = head;
                        for (uint32_t p = 0; p < capacity; ++p) {
                            if (expected && p <= payload) {
                                assert(output[p + 1] == ring[next]);
                                next = next + 1 == size ? 0 : next + 1;
                            } else {
                                assert(output[p + 1] == guard);
                            }
                        }
                        assert(cursor == next);
                        assert(output.front() == guard && output.back() == guard);
                        ++copies;
                    }
                }
            }
        }
    }
    uint32_t cursor = 0, output[32] = {};
    assert(!readFrame(nullptr, 2, cursor, 1, output, 32));
    const uint32_t invalidFormat[2] = {0x101, 0};
    assert(!readFrame(invalidFormat, 2, cursor, 1, output, 32));
    assert(cursor == 0 && output[0] == 0);
    std::printf("PASS: %lu bounded-copy cases with output guards and cursor checks\n", copies);
}
