/* Tahoe IOAccelerator command-pool postconditions used by the Gen11+ VF path. */
#ifndef NGREEN_IOACCEL_COMMAND_POOL_HPP
#define NGREEN_IOACCEL_COMMAND_POOL_HPP

#include <stddef.h>
#include <stdint.h>

namespace NGIOAccelCommandPool {

constexpr size_t acceleratorOffset = 0x10;
constexpr size_t slotsOffset = 0x30;
constexpr size_t slotStride = 0x18;
constexpr size_t maximumOffset = 0x1830;
constexpr size_t countOffset = 0x1832;
constexpr size_t currentOffset = 0x1842;
constexpr uint16_t slotCapacity = 256;
constexpr size_t reviewedGrowthSize = 0x202;

// UUID admission is primary. These exact instruction anchors additionally
// bind the count publication, void selection call and false-success tail used
// by the wrapper's postcondition rationale.
inline bool hasReviewedGrowthContract(const uint8_t *body, size_t length) {
	constexpr uint8_t countAndSelection[] = {
		0x66, 0x89, 0x83, 0x32, 0x18, 0x00, 0x00,
		0x0f, 0xbf, 0x75, 0xc8, 0x48, 0x89, 0xdf,
		0xe8, 0x7b, 0x02, 0x00, 0x00,
		0x41, 0xb6, 0x01, 0xe9, 0x13, 0x01, 0x00, 0x00,
	};
	if (!body || length != reviewedGrowthSize)
		return false;
	for (size_t i = 0; i < sizeof(countAndSelection); ++i)
		if (body[0xc2 + i] != countAndSelection[i])
			return false;
	return true;
}

// Native growth publishes count before its void slot-selection call and then
// returns true unconditionally. Accept success only when the newly added first
// slot is current and all three members needed by later getter/submit paths are
// present. The caller applies this only to its exact VF accelerator owner.
inline bool completedGrowth(bool nativeSuccess, uint16_t maximum,
                            uint16_t previousCount, uint16_t publishedCount,
                            int16_t current, uintptr_t memory,
                            uintptr_t gpuMapping, uintptr_t cpuMapping) {
	if (!nativeSuccess || maximum == 0 || maximum > slotCapacity ||
	    previousCount >= maximum || previousCount >= slotCapacity)
		return false;
	const uint32_t expectedCount = previousCount == 0 ? 1U :
		static_cast<uint32_t>(previousCount) * 2U;
	return expectedCount <= maximum && publishedCount == expectedCount &&
		current == static_cast<int16_t>(previousCount) && memory != 0 &&
		gpuMapping != 0 && cpuMapping != 0;
}

} // namespace NGIOAccelCommandPool

#endif
