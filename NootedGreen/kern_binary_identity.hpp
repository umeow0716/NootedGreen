#pragma once
#include <stddef.h>
#include <stdint.h>

namespace NGBinaryIdentity {
inline uint32_t little32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// An ABI version gate, not a cryptographic integrity/authenticity check.
// Walk only the caller-provided loaded header extent; never trust cmdsize.
inline bool matchesKextUuid(const uint8_t *image, size_t size, const uint8_t *uuid) {
    if (!image || !uuid || size < 32 || little32(image) != 0xFEEDFACFU ||
        little32(image + 4) != 0x01000007U || little32(image + 12) != 0xBU)
        return false; // little-endian 64-bit x86 KEXT_BUNDLE
    const uint32_t count = little32(image + 16), bytes = little32(image + 20);
    if (bytes > size - 32 || !count || count > bytes / 8)
        return false;
    const auto *commands = image + 32;
    size_t pos = 0;
    bool found = false;
    for (uint32_t n = 0; n < count; ++n) {
        if (bytes - pos < 8)
            return false;
        const uint32_t cmd = little32(commands + pos);
        const uint32_t length = little32(commands + pos + 4);
        if (length < 8 || (length & 7U) || length > bytes - pos)
            return false;
        if (cmd == 0x1BU) { // LC_UUID
            if (found || length != 24)
                return false;
            for (size_t i = 0; i < 16; ++i)
                if (commands[pos + 8 + i] != uuid[i])
                    return false;
            found = true;
        }
        pos += length;
    }
    return found && pos == bytes;
}

// AppleIntelTGLGraphics v213 reference, SHA256 in the protocol audit.
constexpr uint8_t tglVfPayloadUuid[16] = {
    0xBA, 0x3A, 0xA1, 0xC0, 0xFE, 0x6B, 0x33, 0xB3,
    0x9D, 0x85, 0x73, 0xF8, 0x48, 0x39, 0x4E, 0x3D
};

// IOAcceleratorFamily2 embedded in macOS Tahoe 25G229 SystemKernelExtensions.
// Private command-pool layout/routes must not be installed on another image.
constexpr uint8_t ioAcceleratorTahoe25G229Uuid[16] = {
    0x6C, 0x4A, 0x16, 0xB1, 0x66, 0xDC, 0x3A, 0x8B,
    0x9F, 0x0B, 0xD3, 0xE0, 0x27, 0xB1, 0x6E, 0xB1
};

// AppleIntelTGLGraphicsFramebuffer 16.0.32 reference variants.
constexpr uint8_t tglFramebufferProductionUuid[16] = {
    0xB6, 0x07, 0x87, 0x43, 0xF7, 0x95, 0x32, 0x02,
    0xA5, 0x5E, 0xEA, 0x69, 0x0F, 0xA4, 0x09, 0x49
};
constexpr uint8_t tglFramebufferDebugUuid[16] = {
    0xB9, 0x6A, 0x84, 0x48, 0x9D, 0x17, 0x3F, 0x78,
    0xB7, 0x6D, 0x15, 0xA0, 0xB3, 0x59, 0xA9, 0xA0
};
}
