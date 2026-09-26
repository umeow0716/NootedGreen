#ifndef NGREEN_DVMT_PATCH_HPP
#define NGREEN_DVMT_PATCH_HPP
#include <stddef.h>
#include <stdint.h>

namespace NGDvmt {
// Accept only adjacent 32-bit SHL reg,17; AND same-reg,0xFE000000.
// MOV alone would lose AND's flags: emit MOV; TEST; padding instead.
// No output is touched on rejection. Caller must establish image boundaries.
inline bool build(const uint8_t *shift, size_t shiftSize,
                  const uint8_t *andInsn, size_t andSize,
                  uint32_t stolen, uint8_t *output, size_t capacity) {
    if (!shift || !andInsn || !output || (shiftSize != 3 && shiftSize != 4) ||
        andSize < 5 || andSize > 7 || capacity < shiftSize + andSize)
        return false;
    const size_t s = shiftSize == 4 ? 1 : 0;
    if ((s && shift[0] != 0x41) || shift[s] != 0xC1 ||
        (shift[s + 1] & 0xF8) != 0xE0 || shift[s + 2] != 0x11)
        return false;
    const unsigned reg = (s ? 8 : 0) | (shift[s + 1] & 7);
    size_t immediate;
    unsigned andReg;
    if (andSize == 5 && andInsn[0] == 0x25) {
        immediate = 1;
        andReg = 0;
    } else {
        const size_t a = andSize == 7 ? 1 : 0;
        if (andSize != 6 + a || (a && andInsn[0] != 0x41) ||
            andInsn[a] != 0x81 || (andInsn[a + 1] & 0xF8) != 0xE0)
            return false;
        immediate = a + 2;
        andReg = (a ? 8 : 0) | (andInsn[a + 1] & 7);
    }
    if (andReg != reg || andInsn[immediate] || andInsn[immediate + 1] ||
        andInsn[immediate + 2] || andInsn[immediate + 3] != 0xFE)
        return false;
    for (size_t i = 0; i < shiftSize + andSize; ++i) output[i] = 0x90;
    size_t i = 0;
    if (reg >= 8) output[i++] = 0x41;
    output[i++] = 0xB8 | (reg & 7);
    for (unsigned byte = 0; byte < 4; ++byte) output[i++] = stolen >> (byte * 8);
    if (reg >= 8) output[i++] = 0x45; // TEST r8d..r15d needs both REX.R/B.
    output[i++] = 0x85;
    output[i] = 0xC0 | ((reg & 7) << 3) | (reg & 7);
    return true;
}
}
#endif
