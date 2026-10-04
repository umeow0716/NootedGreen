#include "../NootedGreen/kern_ioaccel_command_pool.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

using namespace NGIOAccelCommandPool;

int main() {
	std::vector<uint8_t> body(reviewedGrowthSize, 0);
	const uint8_t anchor[] = {
		0x66, 0x89, 0x83, 0x32, 0x18, 0x00, 0x00,
		0x0f, 0xbf, 0x75, 0xc8, 0x48, 0x89, 0xdf,
		0xe8, 0x7b, 0x02, 0x00, 0x00,
		0x41, 0xb6, 0x01, 0xe9, 0x13, 0x01, 0x00, 0x00,
	};
	for (size_t i = 0; i < sizeof(anchor); ++i)
		body[0xc2 + i] = anchor[i];
	assert(hasReviewedGrowthContract(body.data(), body.size()));
	assert(!hasReviewedGrowthContract(nullptr, body.size()));
	assert(!hasReviewedGrowthContract(body.data(), body.size() - 1));
	for (size_t i = 0; i < sizeof(anchor); ++i) {
		body[0xc2 + i] ^= 1;
		assert(!hasReviewedGrowthContract(body.data(), body.size()));
		body[0xc2 + i] ^= 1;
	}
	std::vector<uint8_t> extended(reviewedExtendedInitSize, 0);
	for (size_t i = 0; i < sizeof(extendedInitFind); ++i)
		extended[0x99 + i] = extendedInitFind[i];
	assert(hasReviewedExtendedInitContract(extended.data(), extended.size()));
	assert(!hasReviewedExtendedInitContract(nullptr, extended.size()));
	assert(!hasReviewedExtendedInitContract(extended.data(), extended.size() - 1));
	for (size_t i = 0; i < sizeof(extendedInitFind); ++i) {
		extended[0x99 + i] ^= 1;
		assert(!hasReviewedExtendedInitContract(extended.data(), extended.size()));
		extended[0x99 + i] ^= 1;
	}

	auto checkValid = [](uint16_t maximum, uint16_t previous) {
		const uint16_t published = previous == 0 ? 1 : previous * 2;
		assert(completedGrowth(true, maximum, previous, published,
		                       static_cast<int16_t>(previous), 1, 2, 3));
		assert(!completedGrowth(false, maximum, previous, published,
		                        static_cast<int16_t>(previous), 1, 2, 3));
		assert(!completedGrowth(true, maximum, previous,
		                        static_cast<uint16_t>(published - 1),
		                        static_cast<int16_t>(previous), 1, 2, 3));
		assert(!completedGrowth(true, maximum, previous, published,
		                        static_cast<int16_t>(previous == 0 ? -1 : previous - 1),
		                        1, 2, 3));
		for (unsigned missing = 0; missing < 3; ++missing)
			assert(!completedGrowth(true, maximum, previous, published,
			                        static_cast<int16_t>(previous),
			                        missing == 0 ? 0 : 1,
			                        missing == 1 ? 0 : 2,
			                        missing == 2 ? 0 : 3));
	};
	for (uint16_t maximum = 1; maximum <= slotCapacity; maximum <<= 1) {
		checkValid(maximum, 0);
		for (uint16_t previous = 1; previous < maximum; previous <<= 1)
			checkValid(maximum, previous);
	}
	assert(!completedGrowth(true, 0, 0, 1, 0, 1, 2, 3));
	assert(!completedGrowth(true, slotCapacity + 1, 0, 1, 0, 1, 2, 3));
	assert(!completedGrowth(true, slotCapacity, slotCapacity,
	                        slotCapacity, 0, 1, 2, 3));
	assert(!completedGrowth(true, 3, 2, 4, 2, 1, 2, 3));
}
