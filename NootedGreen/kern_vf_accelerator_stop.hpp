// SPDX-License-Identifier: Thou-Shalt-Not-Profit-1.0
#ifndef kern_vf_accelerator_stop_hpp
#define kern_vf_accelerator_stop_hpp

#include <stdint.h>

// Pure transition policy for the Tahoe accelerator-finalize/stop boundary.
// The production wrapper performs each transition with a compare-and-swap;
// keeping the decision table freestanding makes every phase/owner pair
// exhaustively testable without a kernel or a GPU.
namespace NGVfAcceleratorStop {

enum class Phase : uint32_t {
	Idle = 0,
	Finalizing,
	Finalized,
	NativeStopActive,
	NativeStopComplete,
};

enum class FinalizeAction : uint8_t {
	BeginFinalization,
	AlreadyOwned,
	Reject,
};

enum class StopAction : uint8_t {
	BeginFinalization,
	RunNativeStop,
	AlreadyComplete,
	Reject,
};

enum class OwnerReturnAction : uint8_t {
	PublishFinalized,
	RunNativeStop,
	AlreadyComplete,
	Reject,
};

constexpr uint32_t raw(Phase phase) {
	return static_cast<uint32_t>(phase);
}

constexpr bool valid(Phase phase) {
	return raw(phase) <= raw(Phase::NativeStopComplete);
}

// A finalizer is a single lifecycle owner, not an external producer. Once any
// lifecycle owner has claimed the accelerator, duplicate event-source
// deliveries must not run Apple's one-shot handler concurrently.
constexpr FinalizeAction finalizeAction(Phase phase) {
	return !valid(phase) ? FinalizeAction::Reject :
	       phase == Phase::Idle ? FinalizeAction::BeginFinalization :
	       FinalizeAction::AlreadyOwned;
}

// Tahoe's finalize handler can synchronously tail-call IOService::finalize,
// which re-enters IntelAccelerator::stop on the same thread. Only that
// thread-affine retirement owner may advance Finalizing to NativeStopActive.
// A later ordinary stop may resume Finalized; no caller may duplicate an
// active or completed native stop.
constexpr StopAction stopAction(Phase phase, bool ownsRetirement) {
	return !valid(phase) ? StopAction::Reject :
	       phase == Phase::Idle ? StopAction::BeginFinalization :
	       phase == Phase::Finalizing ?
	           (ownsRetirement ? StopAction::RunNativeStop : StopAction::Reject) :
	       phase == Phase::Finalized ? StopAction::RunNativeStop :
	       phase == Phase::NativeStopComplete ? StopAction::AlreadyComplete :
	       StopAction::Reject;
}

// After Apple's one-shot finalizer returns, a stop owner must provide the
// exactly-once native-stop fallback when synchronous re-entry did not occur.
// An event-source owner instead publishes Finalized and lets the ordinary
// IOService stop edge resume it. A nested stop may already have completed the
// lifecycle before either outer owner returns.
constexpr OwnerReturnAction ownerReturnAction(Phase phase, bool stopOwner) {
	return !valid(phase) ? OwnerReturnAction::Reject :
	       phase == Phase::Finalizing ?
	           (stopOwner ? OwnerReturnAction::RunNativeStop :
	                        OwnerReturnAction::PublishFinalized) :
	       phase == Phase::NativeStopComplete ?
	           OwnerReturnAction::AlreadyComplete :
	       OwnerReturnAction::Reject;
}

} // namespace NGVfAcceleratorStop

#endif /* kern_vf_accelerator_stop_hpp */
