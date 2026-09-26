#include "../NootedGreen/kern_pci_identity.hpp"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

int main() {
    for (uint32_t device = 0; device <= 0xFFFFU; ++device) {
        // Independent byte-level reconstruction of the four-byte read.
        const uint32_t original = device * 0x10001U ^ 0xC74E8086U;
        for (unsigned offset = 0; offset < 256; ++offset) {
            uint8_t bytes[4];
            for (unsigned i = 0; i < 4; ++i) bytes[i] = original >> (8 * i);
            if (offset == 0 || offset == 2) {
                const unsigned low = offset == 0 ? 2 : 0;
                bytes[low] = device;
                bytes[low + 1] = device >> 8;
            }
            uint32_t expected = 0;
            for (unsigned i = 0; i < 4; ++i) expected |= uint32_t(bytes[i]) << (8 * i);
            assert(NGPciIdentity::replaceDeviceId32(original, offset, device) == expected);
        }
    }
    for (uint32_t bad : {0x10000U, 0x9A490000U, 0xFFFFFFFFU})
        for (unsigned offset = 0; offset < 256; ++offset)
            assert(NGPciIdentity::replaceDeviceId32(0xA7A88086U, offset, bad) == 0xA7A88086U);
    puts("PASS 16777216 PCI ID/offset cases and invalid-ID passthrough");
}
