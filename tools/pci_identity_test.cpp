#include "../NootedGreen/kern_pci_identity.hpp"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

int main() {
    uint64_t cases = 0;
    for (uint32_t device = 0; device <= 0xFFFFU; ++device) {
        const uint32_t original = device * 0x10001U ^ 0xC74E8086U;
        for (unsigned offset = 0; offset < 256; ++offset) {
            // Independent PCI model: 32-bit accesses align down to a DWORD;
            // 16-bit accesses align down to a word.
            const uint32_t expected32 = (offset & ~3U) == 0
                ? (original & 0xFFFFU) | (device << 16)
                : original;
            assert(NGPciIdentity::replaceDeviceId32(
                original, 0, offset, device) == expected32);

            const uint16_t original16 = static_cast<uint16_t>(original);
            const uint16_t expected16 = (offset & ~1U) == 2
                ? static_cast<uint16_t>(device)
                : original16;
            assert(NGPciIdentity::replaceDeviceId16(
                original16, 0, offset, device) == expected16);
            cases += 2;
        }
    }
    // Cross every extended page and low offset independently with boundary and
    // compatibility IDs. A nonzero extended-register nibble must never be
    // mistaken for the type-0 configuration header.
    for (uint32_t device : {0U, 0x9A49U, 0xA7A8U, 0xFFFFU}) {
        const uint32_t original = device * 0x10001U ^ 0xC74E8086U;
        for (uint32_t page = 1; page < 16; ++page) {
            const uint32_t space = page << 24;
            for (unsigned offset = 0; offset < 256; ++offset) {
                assert(NGPciIdentity::replaceDeviceId32(
                    original, space, offset, device) == original);
                assert(NGPciIdentity::replaceDeviceId16(
                    static_cast<uint16_t>(original), space, offset, device) ==
                    static_cast<uint16_t>(original));
                cases += 2;
            }
        }
    }
    for (uint32_t bad : {0x10000U, 0x9A490000U, 0xFFFFFFFFU}) {
        for (unsigned offset = 0; offset < 256; ++offset) {
            assert(NGPciIdentity::replaceDeviceId32(
                0xA7A88086U, 0, offset, bad) == 0xA7A88086U);
            assert(NGPciIdentity::replaceDeviceId16(
                0xA7A8U, 0, offset, bad) == 0xA7A8U);
            cases += 2;
        }
    }
    // Regression fixture from the controlled Tahoe panic: read32(2) returned
    // physical 0xa7a88086. It must become 0x9a498086, never 0xa7a89a49.
    assert(NGPciIdentity::replaceDeviceId32(
        0xA7A88086U, 0, 2, 0x9A49U) == 0x9A498086U);
    ++cases;
    printf("PASS %llu aligned PCI identity cases and panic regression\n",
           static_cast<unsigned long long>(cases));
}
