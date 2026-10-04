#include "../NootedGreen/kern_ioaccel_command_pool.hpp"
#include <cassert>
#include <cstdint>
#include <utility>
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
	std::vector<uint8_t> getter(reviewedGetBufferSize, 0);
	const uint8_t getterInitial[] = {
		0x48, 0x8b, 0x87, 0x58, 0x18, 0x00, 0x00,
		0x41, 0x89, 0xf6, 0x4a, 0x8d, 0x0c, 0xb0,
		0x48, 0x3b, 0x8f, 0x50, 0x18, 0x00, 0x00,
	};
	const uint8_t getterSelection[] = {
		0x0f, 0xbf, 0xf1, 0x48, 0x89, 0xdf,
		0xe8, 0x04, 0xfe, 0xff, 0xff,
	};
	const uint8_t getterPublish[] = {
		0x48, 0x89, 0x8b, 0x50, 0x18, 0x00, 0x00,
		0x48, 0x89, 0x83, 0x58, 0x18, 0x00, 0x00,
	};
	for (size_t i = 0; i < sizeof(getterInitial); ++i)
		getter[0x7 + i] = getterInitial[i];
	for (size_t i = 0; i < sizeof(getterSelection); ++i)
		getter[0x6f + i] = getterSelection[i];
	for (size_t i = 0; i < sizeof(getterPublish); ++i)
		getter[0xfa + i] = getterPublish[i];
	assert(hasReviewedGetBufferContract(getter.data(), getter.size()));
	assert(!hasReviewedGetBufferContract(nullptr, getter.size()));
	assert(!hasReviewedGetBufferContract(getter.data(), getter.size() - 1));
	for (auto range : {std::pair<size_t, size_t> {0x7, sizeof(getterInitial)},
	                   std::pair<size_t, size_t> {0x6f, sizeof(getterSelection)},
	                   std::pair<size_t, size_t> {0xfa, sizeof(getterPublish)}}) {
		for (size_t i = 0; i < range.second; ++i) {
			getter[range.first + i] ^= 1;
			assert(!hasReviewedGetBufferContract(getter.data(), getter.size()));
			getter[range.first + i] ^= 1;
		}
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
	for (uint32_t request = 0; request <= blit3dBufferBytes * 2; request += 64) {
		const bool admitted = request <= blit3dUsableBytes;
		assert(admitted == (request <= 0xffc0));
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

	constexpr uintptr_t start = 0x1000;
	constexpr uintptr_t end = start + 0xff8;
	for (uint32_t request = 0; request <= 0x400; ++request)
		assert(hasReturnedCapacity(8, 1, 0, 1, 2, start,
		                           start, end, start, start, request) ==
		       (request <= 0x3fe));
	constexpr uintptr_t partial = start + 0x9b8;
	assert(hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                           start, end, partial, partial, 0x18e));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            start, end, partial, partial, 0x3fe));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            start, end, start, start, UINT32_MAX));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            start, end, start, start + 4, 1));
	assert(!hasReturnedCapacity(8, 1, -1, 1, 2, start,
	                            start, end, start, start, 1));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 0, start,
	                            start, end, start, start, 1));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            end, start, start, start, 1));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start + 4,
	                            start, end, start, start, 1));
}
