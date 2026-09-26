/* Pure GuC MMIO success-payload contracts used by the direct VF bootstrap. */
#ifndef NGREEN_VF_MMIO_RESPONSE_HPP
#define NGREEN_VF_MMIO_RESPONSE_HPP

#include <stdint.h>

namespace NGVfMmioResponse {

constexpr uint32_t data0Mask = 0x0FFFFFFFU;
constexpr uint32_t queryReservedMask = 0x0FFF0000U;
constexpr uint32_t queryLengthMask = 0x0000FFFFU;

// The transport has already established GuC origin and RESPONSE_SUCCESS type.
// These helpers validate the action-specific DATA0 payload without duplicating
// the mailbox ownership/type state machine.
inline bool noData(uint32_t header)
{
	return (header & data0Mask) == 0;
}

inline bool selfConfigAccepted(uint32_t header)
{
	return (header & data0Mask) == 1U;
}

inline bool queryKlvLength(uint32_t header, uint32_t expectedDwords)
{
	return expectedDwords <= 3U &&
	       (header & queryReservedMask) == 0 &&
	       (header & queryLengthMask) == expectedDwords;
}

} // namespace NGVfMmioResponse

#endif
