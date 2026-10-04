#include "../NootedGreen/kern_vf_submission_gate.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

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

	uint64_t ringCases = 0;
	for (uint32_t ringBytes = 16; ringBytes <= 65536; ringBytes <<= 1) {
		const uint32_t dwordCapacity = ringBytes / 4;
		const uint32_t cursors[] = {0, 4, ringBytes / 2,
		                            ringBytes >= 8 ? ringBytes - 8 : 0,
		                            ringBytes - 4};
		const uint32_t requests[] = {0, 1, 2, 7, 16,
		                             dwordCapacity > 0 ? dwordCapacity - 1 : 0,
		                             dwordCapacity, dwordCapacity + 1,
		                             UINT32_MAX};
		for (const auto cursor : cursors)
		for (const auto requested : requests)
		for (unsigned trailer = 0; trailer <= 1; ++trailer)
		for (unsigned tlb = 0; tlb <= 1; ++tlb)
		for (const uint32_t flush : {0U, 5U, 6U, 10U, 12U, 17U})
		for (unsigned aux = 0; aux <= 1; ++aux) {
			uint64_t dwords = requested + (trailer ? 4ULL : 1ULL);
			if (tlb)
				dwords += flush;
			if (aux)
				dwords += 3;
			if (cursor & 7U)
				dwords++;
			dwords = (dwords + 1ULL) & ~1ULL;
			const uint64_t bytes = dwords * 4ULL;
			const bool geometry = cursor < ringBytes && (cursor & 3U) == 0;
			const bool flushValid = (!tlb && flush == 0) ||
				(tlb && flush > 0 && flush <= 16);
			const bool expected = geometry && flushValid &&
				bytes <= UINT32_MAX && bytes <= ringBytes - 8U;
			const auto actual = NGVfSubmission::ringReservation(
				requested, ringBytes, cursor, trailer != 0, tlb != 0,
				flush, aux != 0);
			assert(actual.valid == expected);
			assert(actual.bytes == (expected ? static_cast<uint32_t>(bytes) : 0));
			if (actual.valid) {
				assert(NGVfSubmission::ringReservationSatisfied(
					actual, actual.bytes, ringBytes));
				if (actual.bytes)
					assert(!NGVfSubmission::ringReservationSatisfied(
						actual, actual.bytes - 1U, ringBytes));
				assert(!NGVfSubmission::ringReservationSatisfied(
					actual, ringBytes, ringBytes));
			}
			++ringCases;
		}
	}
	for (const uint32_t badRing : {0U, 4U, 8U, 12U, 24U, 4095U})
		assert(!NGVfSubmission::ringReservation(
			1, badRing, 0, false, false, 0, false).valid);
	std::printf("PASS: 512 VF producer/consumer, 32 bootstrap-task and %llu ring reservation states\n",
	            static_cast<unsigned long long>(ringCases));
}
