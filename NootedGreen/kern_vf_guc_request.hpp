/* Pure validation for the only GuC v70 FAST requests emitted by the VF bridge. */
#ifndef NGREEN_VF_GUC_REQUEST_HPP
#define NGREEN_VF_GUC_REQUEST_HPP

#include <stddef.h>
#include <stdint.h>
#include "kern_context_descriptor.hpp"

namespace NGVfGuCRequest {

constexpr uint32_t registerContext = 0x4502;
constexpr uint32_t deregisterContext = 0x4503;
constexpr uint32_t scheduleContext = 0x1000;
constexpr uint32_t scheduleContextModeSet = 0x1001;
constexpr uint32_t updateContextPolicies = 0x100B;
constexpr uint32_t tlbInvalidation = 0x7000;

struct Attributes {
	bool valid;
	bool retirement;
	uint32_t responseCredits;
};

inline bool validContextId(uint32_t id)
{
	return id < 0xFFFFU;
}

inline Attributes inspect(const uint32_t *request, size_t length)
{
	Attributes result = {false, false, 0};
	if (!request || !length)
		return result;

	const uint32_t action = request[0];
	switch (action) {
		case registerContext:
			// REGISTER_CONTEXT v70: KMD, one logical engine, no parent WQ,
			// and a 32-bit GGTT LRCA descriptor.
			result.valid = length == 12 && request[1] == 1U &&
				validContextId(request[2]) && request[3] <= 5U &&
				request[4] && !(request[4] & (request[4] - 1U)) &&
				request[5] == 0 && request[6] == 0 && request[7] == 0 &&
				request[8] == 0 && request[9] == 0 &&
				NGContextDescriptor::validGucHwlrca(request[10], request[11]);
			break;
		case updateContextPolicies:
			// The four one-dword KLVs emitted by vfSetContextPolicy, in the
			// same order and with the same bounded values as i915's v70 init.
			result.valid = length == 10 && validContextId(request[1]) &&
				request[2] == 0x20030001U && request[3] == 2U &&
				request[4] == 0x20010001U && request[5] == 1000U &&
				request[6] == 0x20020001U &&
				(request[7] == 7500000U || request[7] == 640000U) &&
				request[8] == 0x20050001U && request[9] == 0;
			break;
		case scheduleContext:
			result.valid = length == 2 && validContextId(request[1]);
			break;
		case scheduleContextModeSet:
			result.valid = length == 3 && validContextId(request[1]) &&
				request[2] <= 1U;
			result.retirement = result.valid && request[2] == 0;
			result.responseCredits = result.valid ? 4U : 0U;
			break;
		case deregisterContext:
			result.valid = length == 2 && validContextId(request[1]);
			result.retirement = result.valid;
			result.responseCredits = result.valid ? 3U : 0U;
			break;
		case tlbInvalidation:
			result.valid = length == 3 && request[2] == 0x80000003U;
			result.retirement = result.valid;
			result.responseCredits = result.valid ? 3U : 0U;
			break;
		default:
			break;
	}
	return result;
}

} // namespace NGVfGuCRequest

#endif
