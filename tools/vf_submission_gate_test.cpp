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
	std::printf("PASS: 512 VF native-producer admission states\n");
}
