#include "../NootedGreen/kern_vf_submission_gate.hpp"

#include <cassert>
#include <cstdio>

int main()
{
	unsigned admitted = 0;
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
	}
	assert(admitted == 1);

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
	std::printf("PASS: 512 VF producer and 32 bootstrap-task admission states\n");
}
