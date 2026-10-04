// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.
#pragma once

#include <stdint.h>

// Modern GuC submission assigns one GuC ID to one logical ring context. Keep
// the lifecycle values and the shutdown decision in a freestanding header so
// every state can be exhaustively checked without loading a kernel or VM.
enum VfGucContextState : uint8_t {
	kVfGucContextEmpty = 0,
	kVfGucContextTombstone,
	kVfGucContextRegistering,
	kVfGucContextRegistered,
	kVfGucContextPendingEnable,
	kVfGucContextEnabled,
	kVfGucContextPendingDisable,
	kVfGucContextDisabled,
	kVfGucContextPendingDeregister,
};

namespace NGVfContextShutdown {

// Pinned native encoders address a 64-byte stamp slot and issue an eight-byte
// PIPE_CONTROL post-sync scratch write. Validate before GuC registration;
// subtraction/division avoids overflow even for malformed backing lengths.
inline bool validPacketBacking(int32_t stampIndex, uint64_t stampBytes,
	                             uint64_t scratchBytes) {
	return stampIndex >= 0 && stampBytes >= 64 && scratchBytes >= 8 &&
		static_cast<uint64_t>(stampIndex) <= (stampBytes - 64) / 64;
}

enum class Action : uint8_t {
	Complete,
	Wait,
	Disable,
	Deregister,
};

inline Action action(VfGucContextState state) {
	switch (state) {
		case kVfGucContextEmpty:
		case kVfGucContextTombstone:
			return Action::Complete;
		case kVfGucContextRegistering:
		case kVfGucContextPendingEnable:
		case kVfGucContextPendingDisable:
		case kVfGucContextPendingDeregister:
			return Action::Wait;
		case kVfGucContextEnabled:
			return Action::Disable;
		case kVfGucContextRegistered:
		case kVfGucContextDisabled:
			return Action::Deregister;
	}
	return Action::Wait;
}

// A failed attach is locally recoverable only when no REGISTER_CONTEXT reached
// GuC, or when the compensating DEREGISTER_CONTEXT was both published and
// observed complete. Returning false to Apple's caller is not enough after
// firmware ownership began: the caller can destroy the context image next.
inline bool registrationCleanupComplete(bool registered,
	                                     bool deregisterSent,
	                                     bool tombstoneReached) {
	return !registered || (deregisterSent && tombstoneReached);
}

} // namespace NGVfContextShutdown

// Producer metadata policy only: NOT by itself a hardware completion/idle
// predicate. The production Scheduler4/GuC bridge serializes access, pins the
// concrete context/ring, validates owner identity and calls publish only after
// real CTB publication. A consumer must still prove GPU-written stamp and ring
// head progress, and exclude software termination/restart/fault paths.
namespace NGVfSubmissionCoverage {
constexpr uint32_t ringHeadMask = 0x001FFFFCU;
constexpr uint32_t miReportHead = 0x03800000U;

// submitToRing always finishes the native ring transaction with
// MI_REPORT_HEAD and, when needed, one MI_NOOP so the published tail is QWord
// aligned. Validate the two final ring words, but deliberately accept only an
// observed head equal to the final published tail. Intel defines head==tail as
// the empty-ring condition; predicting the value captured while the report
// command itself executes would be weaker and generation-dependent. A padded
// report may therefore produce a conservative false-busy result, never a
// false-idle result. ringSize is power-of-two in Tahoe's native contract.
inline bool reportHeadForTail(uint32_t ringTail, uint32_t ringSize,
	                          uint32_t lastDword, uint32_t previousDword,
	                          uint32_t &reportHead) {
	if (ringSize < 16 || ringSize > ringHeadMask + sizeof(uint32_t) ||
	    (ringSize & (ringSize - 1U)) != 0 ||
	    (ringTail & (sizeof(uint64_t) - 1U)) != 0 || ringTail >= ringSize)
		return false;
	if (lastDword == miReportHead) {
		reportHead = ringTail;
		return true;
	}
	if (lastDword == 0 && previousDword == miReportHead) {
		reportHead = ringTail;
		return true;
	}
	return false;
}

struct Tracker {
	// Keep this type trivial: the production context table is allocated with
	// IOMallocZero and has no per-element constructor pass.  Zero is the exact
	// initial state; slot reuse goes through resetForReuse() without resetting
	// the monotonic serial.
	uint64_t serial;
	uint64_t owner;
	uint32_t stamp;
	uint32_t tail;
	bool carriesStamp;
	bool claimed;
	bool published;
	bool tainted;
	bool covered;
	bool submitted;
	uint32_t reportHead;
	uint32_t coveredStamp;
	uint32_t coveredTail;
	uint32_t coveredHead;

	uint64_t begin(uint64_t caller, uint32_t value, uint32_t byteTail,
	               uint32_t expectedHead, bool marker) {
		if (!caller || owner || serial == UINT64_MAX)
			return 0;
		++serial;
		owner = caller;
		stamp = value;
		tail = byteTail;
		reportHead = expectedHead;
		carriesStamp = marker;
		claimed = published = tainted = false;
		return serial;
	}

	bool matches(uint64_t token, uint64_t caller, uint32_t value, uint32_t byteTail) const {
		return owner && owner == caller && token && token == serial &&
			stamp == value && tail == byteTail;
	}

	bool claim(uint64_t token, uint64_t caller, uint32_t value, uint32_t byteTail) {
		if (claimed || !matches(token, caller, value, byteTail))
			return false;
		claimed = true;
		return true;
	}

	bool publish(uint64_t token, uint64_t caller, uint32_t value, uint32_t byteTail) {
		if (!claimed || published || !matches(token, caller, value, byteTail))
			return false;
		published = true;
		submitted = true;
		covered = carriesStamp && !tainted;
		coveredStamp = value;
		coveredTail = byteTail;
		coveredHead = reportHead;
		return true;
	}

	void invalidate() {
		covered = false;
		if (owner)
			tainted = true;
	}

	bool finish(uint64_t token, uint64_t caller, bool accepted) {
		if (!owner || owner != caller || !token || token != serial)
			return false;
		const bool consistent = accepted == published;
		if (!consistent)
			invalidate();
		owner = 0;
		claimed = published = carriesStamp = tainted = false;
		return consistent;
	}

	bool hasMarkerCoverage() const { return !owner && covered; }
	bool hasSubmittedWork() const { return submitted; }

	bool gpuComplete(uint32_t observedStamp, uint32_t observedHead,
	                bool softwareCompletionPossible) const {
		return !softwareCompletionPossible && hasMarkerCoverage() &&
			static_cast<int32_t>(coveredStamp - observedStamp) <= 0 &&
			(observedHead & ringHeadMask) == coveredHead;
	}

	// Preserve serial across context-slot reuse. Resetting/reconstructing this
	// tracker would permit an old token to alias a new invocation (ABA).
	// Production must also ensure stale callers cannot outlive table teardown.
	bool resetForReuse() {
		invalidate();
		if (owner)
			return false;
		stamp = tail = reportHead = coveredStamp = coveredTail = coveredHead = 0;
		claimed = published = carriesStamp = tainted = false;
		submitted = false;
		return true;
	}
};
static_assert(__is_trivial(Tracker),
	"VF submission coverage must remain valid in an IOMallocZero table");
} // namespace NGVfSubmissionCoverage

namespace NGVfContextEvent {

struct ScheduleDone {
	bool handled;
	VfGucContextState state;
	bool enablePending;
	bool disablePending;
};

// MODE_DONE carries the firmware's resulting runnable state.  A queued enable
// completion must retire before a later disable completion; accepting only the
// matching payload prevents an out-of-order or corrupt event from advancing
// teardown while the GuC still reports the context runnable.
inline ScheduleDone scheduleDone(VfGucContextState state, bool enablePending,
	                              bool disablePending, uint32_t runnableState) {
	ScheduleDone result {false, state, enablePending, disablePending};
	if (enablePending) {
		const bool enableState = state == kVfGucContextPendingEnable ||
		                         state == kVfGucContextEnabled ||
		                         state == kVfGucContextPendingDisable;
		if (runnableState != 1U || !enableState)
			return result;
		result.enablePending = false;
		if (state == kVfGucContextPendingEnable)
			result.state = kVfGucContextEnabled;
		result.handled = true;
	} else if (disablePending) {
		if (runnableState != 0U || state != kVfGucContextPendingDisable)
			return result;
		result.disablePending = false;
		result.state = kVfGucContextDisabled;
		result.handled = true;
	}
	return result;
}

// DEREGISTER_DONE ends firmware ownership, but native Apple context objects can
// still hold references during device-wide shutdown. Preserve the complete
// descriptor/engine/backing identity until the final late detach releases it.
template <typename Context>
inline bool deregisterDone(Context &context) {
	if (context.state != kVfGucContextPendingDeregister)
		return false;
	context.enablePending = false;
	context.disablePending = false;
	context.state = kVfGucContextTombstone;
	return true;
}

// Called only after firmware deregistration and the last native owner. Keeping
// identity clearing in this boundary prevents a tombstone from becoming
// unmatchable while a late detach still needs its class/instance.
template <typename Context>
inline void clearReleasedIdentity(Context &context) {
	context.lrcaPage = 0;
	context.descriptorLo = 0;
	context.descriptorHi = 0;
	context.engineClass = 0;
	context.engineInstance = 0;
	context.task = nullptr;
	context.contextBacking = nullptr;
	context.ringBacking = nullptr;
	context.stampBacking = nullptr;
	context.scratchBacking = nullptr;
}

} // namespace NGVfContextEvent
