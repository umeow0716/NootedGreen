#include "../NootedGreen/kern_vf_mmio_response.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
	using namespace NGVfMmioResponse;
	constexpr uint32_t success = 0xF0000000U;
	uint64_t cases = 0;

	assert(noData(success));
	assert(!selfConfigAccepted(success));
	assert(!noData(success | 1U));
	assert(selfConfigAccepted(success | 1U));
	++cases;

	for (uint32_t bit = 0; bit < 28U; ++bit) {
		const uint32_t payload = 1U << bit;
		assert(noData(success | payload) == false);
		assert(selfConfigAccepted(success | payload) == (payload == 1U));
		++cases;
	}

	for (uint32_t expected = 0; expected <= 4U; ++expected) {
		for (uint32_t length = 0; length <= 0xFFFFU; ++length) {
			assert(queryKlvLength(success | length, expected) ==
			       (expected <= 3U && length == expected));
			++cases;
		}
		for (uint32_t bit = 16; bit < 28U; ++bit) {
			assert(!queryKlvLength(success | (1U << bit) |
			                       (expected & queryLengthMask), expected));
			++cases;
		}
	}

	std::printf("PASS: %llu GuC MMIO response payload contracts\n",
	            static_cast<unsigned long long>(cases));
}
