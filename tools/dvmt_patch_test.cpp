#include "../NootedGreen/kern_dvmt_patch.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

int main() {
    unsigned cases = 0;
    for (unsigned reg = 0; reg < 16; ++reg) {
        std::vector<uint8_t> shift, andInsn;
        if (reg >= 8) { shift.push_back(0x41); andInsn.push_back(0x41); }
        shift.push_back(0xC1); shift.push_back(0xE0 | (reg & 7)); shift.push_back(0x11);
        andInsn.push_back(0x81); andInsn.push_back(0xE0 | (reg & 7));
        andInsn.insert(andInsn.end(), {0, 0, 0, 0xFE});
        for (uint32_t value : {0U, 0x2000000U, 0xFE000000U, 0xFFFFFFFFU}) {
            uint8_t output[13]; std::memset(output, 0xCC, sizeof(output));
            assert(NGDvmt::build(shift.data(), shift.size(), andInsn.data(), andInsn.size(),
                value, output + 1, 11));
            assert(output[0] == 0xCC && output[12] == 0xCC);
            size_t i = 1;
            if (reg >= 8) assert(output[i++] == 0x41);
            assert(output[i++] == (0xB8 | (reg & 7)));
            uint32_t decodedValue = 0;
            for (unsigned byte = 0; byte < 4; ++byte) decodedValue |= uint32_t(output[i++]) << (byte * 8);
            assert(decodedValue == value);
            if (reg >= 8) assert(output[i++] == 0x45);
            assert(output[i++] == 0x85);
            assert(output[i++] == (0xC0 | ((reg & 7) << 3) | (reg & 7)));
            for (; i <= shift.size() + andInsn.size(); ++i) assert(output[i] == 0x90);
            for (; i < sizeof(output); ++i) assert(output[i] == 0xCC);
            ++cases;
        }
        // Every one-byte corruption must either preserve the narrowly accepted
        // encoding or be rejected without modifying output.
        for (size_t byte = 0; byte < andInsn.size(); ++byte)
        for (unsigned value = 0; value < 256; ++value) {
            auto changed = andInsn; changed[byte] = value;
            uint8_t output[11]; std::memset(output, 0xCC, sizeof(output));
            bool ok = NGDvmt::build(shift.data(), shift.size(), changed.data(), changed.size(), 1, output, 11);
            assert(ok == (changed == andInsn));
            if (!ok) for (auto c : output) assert(c == 0xCC);
            ++cases;
        }
    }
    const uint8_t shift[] = {0xC1, 0xE0, 0x11}, accumulatorAnd[] = {0x25, 0, 0, 0, 0xFE};
    uint8_t output[11] {};
    assert(NGDvmt::build(shift, 3, accumulatorAnd, 5, 0x2000000, output, 11));
    assert(!NGDvmt::build(shift, 3, accumulatorAnd, 5, 0, output, 7));
    assert(!NGDvmt::build(nullptr, 3, accumulatorAnd, 5, 0, output, 11));
    assert(!NGDvmt::build(shift, 3, nullptr, 5, 0, output, 11));
    assert(!NGDvmt::build(shift, 3, accumulatorAnd, 5, 0, nullptr, 11));
    std::printf("PASS: %u DVMT register/encoding/corruption cases, output guards and accumulator encoding\n", cases);
}
