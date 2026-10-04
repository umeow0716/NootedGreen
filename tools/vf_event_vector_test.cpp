#include "../NootedGreen/kern_event_vector.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

using namespace NGEventVector;

int main() {
	std::vector<uint8_t> body(reviewedGrowSize, 0);
	const uint8_t prologue[] = {
		0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
		0x53, 0x50, 0x48, 0x39, 0x77, 0x08, 0x73, 0x5b,
	};
	const uint8_t allocationNull[] = {0x48, 0x85, 0xc0, 0x74, 0x43};
	const uint8_t publication[] = {
		0x4c, 0x89, 0x7b, 0x10, 0x4c, 0x89, 0x73, 0x08,
		0xb0, 0x01, 0xeb, 0x02, 0x31, 0xc0,
	};
	for (size_t i = 0; i < sizeof(prologue); ++i)
		body[i] = prologue[i];
	for (size_t i = 0; i < sizeof(allocationNull); ++i)
		body[0x23 + i] = allocationNull[i];
	for (size_t i = 0; i < sizeof(publication); ++i)
		body[0x5f + i] = publication[i];
	assert(hasReviewedGrow(body.data(), body.size()));
	assert(!hasReviewedGrow(nullptr, body.size()));
	assert(!hasReviewedGrow(body.data(), body.size() - 1));
	for (size_t offset : {size_t {0}, size_t {0x23}, size_t {0x5f}}) {
		body[offset] ^= 1;
		assert(!hasReviewedGrow(body.data(), body.size()));
		body[offset] ^= 1;
	}
	const uintptr_t bodyStart = reinterpret_cast<uintptr_t>(body.data());
	assert(locateReviewedGrow(bodyStart - 0x20,
	                          bodyStart + reviewedGrowSize + 0x20, 0x20) == bodyStart);
	assert(locateReviewedGrow(0, bodyStart + reviewedGrowSize, 0) == 0);
	assert(locateReviewedGrow(bodyStart, bodyStart, 0) == 0);
	assert(locateReviewedGrow(bodyStart, bodyStart + reviewedGrowSize - 1, 0) == 0);
	assert(locateReviewedGrow(bodyStart, bodyStart + reviewedGrowSize, 1) == 0);
	body[0] ^= 1;
	assert(locateReviewedGrow(bodyStart, bodyStart + reviewedGrowSize, 0) == 0);
	body[0] ^= 1;

	assert(hasCapacity(0, 0, 0, 0));
	assert(hasCapacity(0, 4, 0x1000, 4));
	assert(hasCapacity(3, 4, 0x1000, 4));
	assert(hasCapacity(4, 8, 0x1000, 6));
	assert(!hasCapacity(1, 0, 0, 0));
	assert(!hasCapacity(5, 4, 0x1000, 4));
	assert(!hasCapacity(0, 4, 0, 4));
	assert(!hasCapacity(0, 0, 0x1000, 0));
	assert(!hasCapacity(0, 4, 0x1000, 5));
	assert(!hasCapacity(0, SIZE_MAX, 0x1000, SIZE_MAX));
	assert(hasConsistentState(0, 0, 0));
	assert(hasConsistentState(3, 4, 0x1000));
	assert(!hasConsistentState(1, 0, 0));
	assert(!hasConsistentState(0, 4, 0));
	assert(hasRepresentableRequest(SIZE_MAX / sizeof(uintptr_t)));
	assert(!hasRepresentableRequest(SIZE_MAX / sizeof(uintptr_t) + 1));
}
