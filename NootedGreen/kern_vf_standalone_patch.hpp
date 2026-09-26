/* Exact VF-only framebuffer wait anchor in the UUID-pinned Tahoe accelerator. */
#ifndef NGREEN_VF_STANDALONE_PATCH_HPP
#define NGREEN_VF_STANDALONE_PATCH_HPP

#include <stdint.h>

namespace NGVfStandalonePatch {

// registerWithFramebufferController() passes 30 seconds in nanoseconds to
// IOService::waitForMatchingService. A VF deliberately has no physical
// framebuffer, so pass zero and retain the native nonblocking lookup and exact
// standalone fallback path.
constexpr uint8_t waitFind[] = {
	0x48, 0xbe, 0x00, 0xac, 0x23, 0xfc, 0x06, 0x00, 0x00, 0x00,
};
constexpr uint8_t waitReplace[] = {
	0x48, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

} // namespace NGVfStandalonePatch

#endif
