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

} // namespace NGVfContextShutdown

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
	context.contextBacking = nullptr;
}

} // namespace NGVfContextEvent
