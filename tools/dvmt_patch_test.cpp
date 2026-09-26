#include "../NootedGreen/kern_dvmt_patch.hpp"
#include <cassert>
#include <cstdio>

int main() {
    unsigned cases = 0;
    for (unsigned raw = 0; raw < 256; ++raw) {
        uint32_t decoded = 0xA5A5A5A5;
        const uint64_t mib = raw < 0xF0 ? uint64_t(raw) * 32 :
            uint64_t(raw - 0xF0) * 4 + 4;
        const uint64_t expected = mib * 1024 * 1024;
        const bool representable = raw != 0xFF && expected <= UINT32_MAX;
        assert(NGDvmt::decodeGen9Gms(static_cast<uint8_t>(raw), decoded) == representable);
        if (representable)
            assert(decoded == expected);
        else
            assert(decoded == 0xA5A5A5A5);
        ++cases;
    }
    uint32_t decoded = 0;
    assert(NGDvmt::decodeGen9Gms(0x10, decoded) && decoded == 512U * 1024 * 1024);
    assert(NGDvmt::decodeGen9Gms(0x20, decoded) && decoded == 1024U * 1024 * 1024);
    assert(NGDvmt::decodeGen9Gms(0x7F, decoded) && decoded == 4064U * 1024 * 1024);
    assert(NGDvmt::decodeGen9Gms(0xF0, decoded) && decoded == 4U * 1024 * 1024);
    assert(NGDvmt::decodeGen9Gms(0xFE, decoded) && decoded == 60U * 1024 * 1024);
    std::printf("PASS: %u GMS encodings checked against the 64-bit oracle\n", cases);
}
