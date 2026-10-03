#include "../NootedGreen/kern_vf_submission_gate.hpp"

#include <cassert>
#include <cstdio>

int main()
{
	unsigned admitted = 0;
	unsigned consumers = 0;
	unsigned pollingConsumers = 0;
	for (unsigned bits = 0; bits < (1U << 9); ++bits) {
		const NGVfSubmission::State state {
			(bits & (1U << 0)) != 0,
			(bits & (1U << 1)) != 0,
			(bits & (1U << 2)) != 0,
			(bits & (1U << 3)) != 0,
			(bits & (1U << 4)) != 0,
			(bits & (1U << 5)) != 0,
			(bits & (1U << 6)) != 0,
			(bits & (1U << 7)) != 0,
			(bits & (1U << 8)) != 0,
		};
		const bool expected = (bits & 0x3FU) == 0x3FU &&
			(bits & 0x1C0U) == 0;
		assert(NGVfSubmission::ready(state) == expected);
		admitted += expected;
		const bool consumerExpected = (bits & 0x3FU) == 0x3FU &&
			(bits & ((1U << 6) | (1U << 8))) == 0;
		assert(NGVfSubmission::consumerReady(state) == consumerExpected);
		consumers += consumerExpected;
		const bool pollingExpected = (bits & 0x3BU) == 0x3BU &&
			(bits & ((1U << 6) | (1U << 8))) == 0;
		assert(NGVfSubmission::consumerReady(state, false) == pollingExpected);
		pollingConsumers += pollingExpected;
	}
	assert(admitted == 1);
	assert(consumers == 2); // ordinary operation and submission-stopped teardown
	assert(pollingConsumers == 4); // IRQ-disabled variants remain synchronously drainable

	// Actual transport policy across teardown stages. Merely announcing device
	// stop is NOT represented here as closed external admission or drained
	// native event/timer owners. Retirement transport must remain available.
	NGVfSubmission::State teardown {true, true, true, true, true, true,
	                                false, false, false};
	assert(NGVfSubmission::ready(teardown));
	assert(NGVfSubmission::consumerReady(teardown));
	teardown.submissionStopped = true;
	assert(!NGVfSubmission::ready(teardown));
	assert(NGVfSubmission::consumerReady(teardown));
	teardown.interruptReady = false;
	assert(!NGVfSubmission::consumerReady(teardown));
	assert(NGVfSubmission::consumerReady(teardown, false));
	teardown.ctbStopped = true;
	assert(!NGVfSubmission::consumerReady(teardown, false));

	unsigned syntheticBootstrap = 0;
	for (unsigned bits = 0; bits < (1U << 5); ++bits) {
		const bool nativeClassification = (bits & (1U << 0)) != 0;
		const bool virtualDevice = (bits & (1U << 1)) != 0;
		const bool taskPresent = (bits & (1U << 2)) != 0;
		const bool acceleratorPresent = (bits & (1U << 3)) != 0;
		const bool kernelTaskAssigned = (bits & (1U << 4)) != 0;
		const bool expected = nativeClassification ||
			(virtualDevice && taskPresent && acceleratorPresent && !kernelTaskAssigned);
		const bool actual = NGVfSubmission::bootstrapKernelTask(nativeClassification,
			virtualDevice, taskPresent, acceleratorPresent, kernelTaskAssigned);
		assert(actual == expected);
		if (!nativeClassification && actual)
			++syntheticBootstrap;
	}
	assert(syntheticBootstrap == 1);
	std::printf("PASS: 512 VF producer/consumer and 32 bootstrap-task admission states\n");
}
