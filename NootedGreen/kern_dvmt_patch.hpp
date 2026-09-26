#ifndef NGREEN_DVMT_PATCH_HPP
#define NGREEN_DVMT_PATCH_HPP
#include <stddef.h>
#include <stdint.h>

namespace NGDvmt {
// Intel gen9+ Graphics Mode Select encoding, matching Linux
// arch/x86/kernel/early-quirks.c:gen9_stolen_size(). The private framebuffer
// ABI stores the result in 32 bits, so reject encodings whose byte count cannot
// be represented instead of silently wrapping them.
inline bool decodeGen9Gms(uint8_t gms, uint32_t &bytes) {
    if (gms == 0xFF)
        return false;
    const uint64_t mib = gms < 0xF0 ? uint64_t(gms) * 32 :
        uint64_t(gms - 0xF0) * 4 + 4;
    const uint64_t decoded = mib * UINT64_C(1024) * UINT64_C(1024);
    if (decoded > UINT32_MAX)
        return false;
    bytes = static_cast<uint32_t>(decoded);
    return true;
}

}
#endif
