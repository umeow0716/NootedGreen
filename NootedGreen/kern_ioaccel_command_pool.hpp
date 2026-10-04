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
constexpr size_t recordOffset = 0x1860;
constexpr uint16_t slotCapacity = 256;
constexpr size_t reviewedGrowthSize = 0x202;
constexpr size_t reviewedExtendedInitSize = 0xf4;

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

// Replace exactly the post-pool-init cleanup/load/test sequence. The routed
// init bridge returns with ZF reflecting AL and RDI restored to the task. LEA
// discards the four stack arguments without changing ZF; false reaches the
// constructor's existing failure epilogue before backing/setup side effects.
constexpr uint8_t extendedInitFind[] = {
	0x48, 0x83, 0xc4, 0x20,             // add rsp, 0x20
	0x49, 0x8b, 0x74, 0x24, 0x10,       // mov rsi, [r12 + 0x10]
	0x48, 0x85, 0xf6,                   // test rsi, rsi
	0x74, 0x18,                         // je setup
	0x4c, 0x89, 0xf7,                   // mov rdi, r14
};
constexpr uint8_t extendedInitReplace[] = {
	0x48, 0x8d, 0x64, 0x24, 0x20,       // lea rsp, [rsp + 0x20], preserve ZF
	0x74, 0x47,                         // jz existing false epilogue
	0x49, 0x8b, 0x74, 0x24, 0x10,       // mov rsi, [r12 + 0x10]
	0x48, 0x85, 0xf6,                   // test rsi, rsi
	0x74, 0x15,                         // je setup; RDI supplied by bridge
};
static_assert(sizeof(extendedInitFind) == sizeof(extendedInitReplace),
	"extended-context init patch must preserve instruction extent");

inline bool hasReviewedExtendedInitContract(const uint8_t *body, size_t length) {
	if (!body || length != reviewedExtendedInitSize)
		return false;
	for (size_t i = 0; i < sizeof(extendedInitFind); ++i)
		if (body[0x99 + i] != extendedInitFind[i])
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
