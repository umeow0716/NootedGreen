#include "../NootedGreen/kern_vf_accelerator_stop.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

using NGVfAcceleratorStop::FinalizeAction;
using NGVfAcceleratorStop::OwnerReturnAction;
using NGVfAcceleratorStop::Phase;
using NGVfAcceleratorStop::StopAction;

namespace {

struct Model {
	Phase phase {Phase::Idle};
	unsigned finalizers {};
	unsigned nativeStops {};

	void beginFinalization() {
		assert(phase == Phase::Idle);
		phase = Phase::Finalizing;
		++finalizers;
	}

	void runNativeStop(bool ownsRetirement) {
		const auto action = NGVfAcceleratorStop::stopAction(
			phase, ownsRetirement);
		assert(action == StopAction::RunNativeStop);
		assert(phase == Phase::Finalizing || phase == Phase::Finalized);
		phase = Phase::NativeStopActive;
		++nativeStops;
		phase = Phase::NativeStopComplete;
	}
};

void stopFirst(bool synchronousReentry) {
	Model model {};
	assert(NGVfAcceleratorStop::stopAction(model.phase, false) ==
	       StopAction::BeginFinalization);
	model.beginFinalization();
	if (synchronousReentry)
		model.runNativeStop(true);
	const auto returned = NGVfAcceleratorStop::ownerReturnAction(
		model.phase, true);
	if (synchronousReentry)
		assert(returned == OwnerReturnAction::AlreadyComplete);
	else {
		assert(returned == OwnerReturnAction::RunNativeStop);
		model.runNativeStop(true);
	}
	assert(model.phase == Phase::NativeStopComplete);
	assert(model.finalizers == 1 && model.nativeStops == 1);
	assert(NGVfAcceleratorStop::stopAction(model.phase, false) ==
	       StopAction::AlreadyComplete);
}

void finalizerFirst(bool synchronousReentry) {
	Model model {};
	assert(NGVfAcceleratorStop::finalizeAction(model.phase) ==
	       FinalizeAction::BeginFinalization);
	model.beginFinalization();
	if (synchronousReentry)
		model.runNativeStop(true);
	const auto returned = NGVfAcceleratorStop::ownerReturnAction(
		model.phase, false);
	if (synchronousReentry)
		assert(returned == OwnerReturnAction::AlreadyComplete);
	else {
		assert(returned == OwnerReturnAction::PublishFinalized);
		model.phase = Phase::Finalized;
		assert(NGVfAcceleratorStop::stopAction(model.phase, false) ==
		       StopAction::RunNativeStop);
		model.runNativeStop(false);
	}
	assert(model.phase == Phase::NativeStopComplete);
	assert(model.finalizers == 1 && model.nativeStops == 1);
}

} // namespace

int main() {
	const std::array<Phase, 5> phases {{
		Phase::Idle,
		Phase::Finalizing,
		Phase::Finalized,
		Phase::NativeStopActive,
		Phase::NativeStopComplete,
	}};
	for (const auto phase : phases) {
		assert(NGVfAcceleratorStop::valid(phase));
		for (const bool owns : {false, true}) {
			const auto action = NGVfAcceleratorStop::stopAction(phase, owns);
			if (phase == Phase::Idle)
				assert(action == StopAction::BeginFinalization);
			else if (phase == Phase::Finalizing)
				assert(action == (owns ? StopAction::RunNativeStop :
				                         StopAction::Reject));
			else if (phase == Phase::Finalized)
				assert(action == StopAction::RunNativeStop);
			else if (phase == Phase::NativeStopActive)
				assert(action == StopAction::Reject);
			else
				assert(action == StopAction::AlreadyComplete);
		}
		const auto finalize = NGVfAcceleratorStop::finalizeAction(phase);
		assert(finalize == (phase == Phase::Idle ?
		       FinalizeAction::BeginFinalization : FinalizeAction::AlreadyOwned));
	}

	for (uint32_t raw = 5; raw < 256; ++raw) {
		const auto invalid = static_cast<Phase>(raw);
		assert(!NGVfAcceleratorStop::valid(invalid));
		assert(NGVfAcceleratorStop::finalizeAction(invalid) ==
		       FinalizeAction::Reject);
		assert(NGVfAcceleratorStop::stopAction(invalid, false) ==
		       StopAction::Reject);
		assert(NGVfAcceleratorStop::stopAction(invalid, true) ==
		       StopAction::Reject);
		assert(NGVfAcceleratorStop::ownerReturnAction(invalid, false) ==
		       OwnerReturnAction::Reject);
		assert(NGVfAcceleratorStop::ownerReturnAction(invalid, true) ==
		       OwnerReturnAction::Reject);
	}

	assert(NGVfAcceleratorStop::ownerReturnAction(
	       Phase::Finalizing, false) == OwnerReturnAction::PublishFinalized);
	assert(NGVfAcceleratorStop::ownerReturnAction(
	       Phase::Finalizing, true) == OwnerReturnAction::RunNativeStop);
	assert(NGVfAcceleratorStop::ownerReturnAction(
	       Phase::NativeStopComplete, false) == OwnerReturnAction::AlreadyComplete);
	for (const auto rejected : {Phase::Idle, Phase::Finalized,
	                            Phase::NativeStopActive}) {
		assert(NGVfAcceleratorStop::ownerReturnAction(rejected, false) ==
		       OwnerReturnAction::Reject);
		assert(NGVfAcceleratorStop::ownerReturnAction(rejected, true) ==
		       OwnerReturnAction::Reject);
	}

	stopFirst(false);
	stopFirst(true);
	finalizerFirst(false);
	finalizerFirst(true);
	std::puts("PASS: exhaustive VF accelerator finalize/stop state model");
}
