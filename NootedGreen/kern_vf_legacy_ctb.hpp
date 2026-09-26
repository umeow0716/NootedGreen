/* Pure contracts for Tahoe's legacy CTB registration bridge. */
#ifndef NGREEN_VF_LEGACY_CTB_HPP
#define NGREEN_VF_LEGACY_CTB_HPP

#include <stddef.h>
#include <stdint.h>

namespace NGVfLegacyCtb {

constexpr uint32_t registerAction = 0x4505U;
constexpr uint32_t deregisterAction = 0x4506U;
constexpr uint32_t legacyDescriptorBytes = 0x40U;
constexpr uint32_t legacyG2HDescriptorOffset = 0x400U;

inline uint32_t responseStatus(bool success)
{
	return success ? 0U : 1U;
}

inline bool registration(const uint32_t *request, size_t length,
                         uint32_t backingBase)
{
	if (!request || length != 4U || request[0] != registerAction ||
	    request[2] != legacyDescriptorBytes || request[3] > 1U)
		return false;
	const uint64_t expected = static_cast<uint64_t>(backingBase) +
	                          (request[3] ? legacyG2HDescriptorOffset : 0U);
	return expected <= UINT32_MAX && request[1] == expected;
}

inline bool deregistration(const uint32_t *request, size_t length,
                           uint32_t registrationToken)
{
	return request && length == 3U && request[0] == deregisterAction &&
	       request[1] == registrationToken && request[2] <= 1U;
}

} // namespace NGVfLegacyCtb

#endif
