#include "../NootedGreen/kern_binary_identity.hpp"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <fstream>
#include <iterator>

using namespace NGBinaryIdentity;
static void put(std::vector<uint8_t> &v, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) v[at + i] = value >> (8 * i);
}
static std::vector<uint8_t> readFile(const char *path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}
int main(int argc, char **argv) {
    std::vector<uint8_t> good(72, 0);
    put(good, 0, 0xFEEDFACF); put(good, 4, 0x01000007); put(good, 12, 0xB);
    put(good, 16, 3); put(good, 20, 40);
    put(good, 32, 0x99); put(good, 36, 8);
    put(good, 40, 0x1B); put(good, 44, 24);
    for (size_t i = 0; i < 16; ++i) good[48 + i] = tglVfPayloadUuid[i];
    put(good, 64, 0x99); put(good, 68, 8);
    auto matches = [](const std::vector<uint8_t> &v) {
        return matchesKextUuid(v.data(), v.size(), tglVfPayloadUuid);
    };
    assert(matches(good));
    assert(!matchesKextUuid(nullptr, good.size(), tglVfPayloadUuid));
    assert(!matchesKextUuid(good.data(), good.size(), nullptr));
    for (size_t length = 0; length < good.size(); ++length) {
        std::vector<uint8_t> truncated(good.begin(), good.begin() + length);
        assert(!matches(truncated));
    }
    for (size_t i = 0; i < 16; ++i)
        for (unsigned value = 0; value < 256; ++value) {
            auto v = good; v[48 + i] = value;
            assert(matches(v) == (value == tglVfPayloadUuid[i]));
        }
    for (size_t offset : {size_t(0), size_t(4), size_t(12), size_t(16), size_t(20),
                          size_t(36), size_t(44), size_t(68)}) {
        for (uint32_t value : {0U, 1U, 7U, 0xFFFFFFF8U, 0xFFFFFFFFU}) {
            auto v = good; put(v, offset, value); assert(!matches(v));
        }
    }
    auto missing = good; put(missing, 40, 0x99); assert(!matches(missing));
    auto duplicate = good;
    duplicate.insert(duplicate.end(), good.begin() + 40, good.begin() + 64);
    put(duplicate, 16, 4); put(duplicate, 20, 64); assert(!matches(duplicate));
    auto padded = good; padded.resize(80); put(padded, 20, 48); assert(!matches(padded));
    // Unaligned starts are parsed bytewise, without casting to Mach structs.
    auto unaligned = good; unaligned.insert(unaligned.begin(), 0);
    assert(matchesKextUuid(unaligned.data() + 1, good.size(), tglVfPayloadUuid));
    auto framebuffer = good;
    for (size_t i = 0; i < 16; ++i)
        framebuffer[48 + i] = tglFramebufferProductionUuid[i];
    assert(matchesKextUuid(framebuffer.data(), framebuffer.size(),
                           tglFramebufferProductionUuid));
    assert(!matchesKextUuid(framebuffer.data(), framebuffer.size(),
                            tglFramebufferDebugUuid));
    for (size_t i = 0; i < 16; ++i)
        framebuffer[48 + i] = tglFramebufferDebugUuid[i];
    assert(matchesKextUuid(framebuffer.data(), framebuffer.size(),
                           tglFramebufferDebugUuid));
    assert(!matchesKextUuid(framebuffer.data(), framebuffer.size(),
                            tglFramebufferProductionUuid));
    auto ioAccelerator = good;
    for (size_t i = 0; i < 16; ++i)
        ioAccelerator[48 + i] = ioAcceleratorTahoe25G229Uuid[i];
    assert(matchesKextUuid(ioAccelerator.data(), ioAccelerator.size(),
                           ioAcceleratorTahoe25G229Uuid));
    assert(!matchesKextUuid(ioAccelerator.data(), ioAccelerator.size(),
                            tglVfPayloadUuid));
    // Deterministic malformed command/header fuzz, sanitizer bounds checking.
    uint32_t seed = 0x8E0291;
    for (size_t n = 0; n < 50000; ++n) {
        auto v = good;
        for (unsigned j = 0; j < 4; ++j) {
            seed = seed * 1664525U + 1013904223U;
            v[seed % v.size()] ^= (seed >> 24) | 1U;
        }
        (void)matches(v);
    }
    if (argc >= 2) {
        const auto payload = readFile(argv[1]);
        assert(matches(payload));
        puts("PASS pinned on-disk TGL payload UUID");
    }
    if (argc == 3) {
        const auto production = readFile(argv[1]);
        const auto signedSystem = readFile(argv[2]);
        assert(matches(production));
        assert(matches(signedSystem));
        assert(production.size() == signedSystem.size());
        const size_t bundleOffsets[] = {
            1805640, 1805641, 1805642, 1805643, 1805644,
            1819931, 1819932, 1819933, 1819934, 1819935,
        };
        size_t next = 0;
        for (size_t i = 0; i < production.size(); ++i) {
            if (production[i] == signedSystem[i])
                continue;
            assert(next < sizeof(bundleOffsets) / sizeof(bundleOffsets[0]));
            assert(i == bundleOffsets[next]);
            assert(production[i] == static_cast<uint8_t>('x'));
            assert(signedSystem[i] == static_cast<uint8_t>("apple"[next % 5]));
            ++next;
        }
        assert(next == sizeof(bundleOffsets) / sizeof(bundleOffsets[0]));
        puts("PASS TGL payload variants differ only in two bundle identifiers");
    }
    puts("PASS Mach-O truncation, UUID mutations, malformed commands and 50000 fuzz inputs");
}
