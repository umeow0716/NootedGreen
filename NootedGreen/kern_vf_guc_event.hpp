/* Pure validation for GuC events accepted by the direct VF CTB bridge. */
#ifndef NGREEN_VF_GUC_EVENT_HPP
#define NGREEN_VF_GUC_EVENT_HPP

#include <stdint.h>

namespace NGVfGuCEvent {

constexpr uint32_t originGuc = 0x80000000U;
constexpr uint32_t typeMask = 0x70000000U;
constexpr uint32_t typeEvent = 0x10000000U;

constexpr uint32_t scheduleContextModeDone = 0x1002U;
constexpr uint32_t contextResetNotification = 0x1008U;
constexpr uint32_t engineFailureNotification = 0x1009U;
constexpr uint32_t deregisterContextDone = 0x4600U;
constexpr uint32_t tlbInvalidationDone = 0x7001U;

enum class Kind : uint8_t {
	Invalid,
	ScheduleContextModeDone,
	DeregisterContextDone,
	TlbInvalidationDone,
	ContextReset,
	EngineFailure,
};

struct Attributes {
	Kind kind;
	uint32_t responseCredits;

	bool valid() const { return kind != Kind::Invalid; }
	bool fatal() const {
		return kind == Kind::ContextReset || kind == Kind::EngineFailure;
	}
};

// A TLB_DONE retires exactly one outstanding sequence.  Sequence zero is
// valid after the 32-bit counter wraps, so compare the predecessor modulo
// 2^32 rather than treating zero as a sentinel.
inline bool expectedTlbCompletion(bool waitActive, uint32_t waitSeqno,
                                  uint32_t doneSeqno, uint32_t eventSeqno)
{
	return waitActive && eventSeqno == waitSeqno &&
	       doneSeqno == eventSeqno - 1U;
}

inline Attributes inspect(uint32_t hxg, uint32_t length)
{
	Attributes result = {Kind::Invalid, 0};
	if ((hxg & originGuc) == 0 || (hxg & typeMask) != typeEvent ||
	    !length || length > 31U)
		return result;

	// These are the only G2H actions produced by requests implemented by this
	// bridge, plus the two firmware failures for which it has no replay path.
	// Length includes the HXG header but excludes the CT transport header.
	switch (hxg & 0xFFFFU) {
		case scheduleContextModeDone:
			if (length == 3U)
				result = {Kind::ScheduleContextModeDone, 4U};
			break;
		case deregisterContextDone:
			if (length == 2U)
				result = {Kind::DeregisterContextDone, 3U};
			break;
		case tlbInvalidationDone:
			if (length == 2U)
				result = {Kind::TlbInvalidationDone, 3U};
			break;
		case contextResetNotification:
			if (length == 2U)
				result = {Kind::ContextReset, 0U};
			break;
		case engineFailureNotification:
			if (length == 4U)
				result = {Kind::EngineFailure, 0U};
			break;
		default:
			break;
	}
	return result;
}

} // namespace NGVfGuCEvent

#endif
