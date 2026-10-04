/* Exact failed-init ownership repairs for the UUID-pinned Tahoe TGL accelerator. */
#ifndef NGREEN_VF_GUC_FACTORY_PATCH_HPP
#define NGREEN_VF_GUC_FACTORY_PATCH_HPP

#include <stdint.h>
#include "kern_pattern_match.hpp"

namespace NGVfGuCFactoryPatch {

// Scheduler4's streamer-failure branch manually deletes the scheduler, then
// withAccelerator releases it again. Keep the factory's required release for
// all failures (including base-init failures); omit only this premature free.
constexpr uint8_t schedulerInitFreeFind[] = {
	0x49, 0x8b, 0x04, 0x24, 0x4c, 0x89, 0xe7,
	0xff, 0x90, 0x90, 0x00, 0x00, 0x00,
	0x45, 0x31, 0xf6,
};
constexpr uint8_t schedulerInitFreeReplace[] = {
	0x49, 0x8b, 0x04, 0x24, 0x4c, 0x89, 0xe7,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
	0x45, 0x31, 0xf6,
};

inline bool schedulerInitPreflight(const uint8_t *body, size_t length) {
	size_t offset = 0;
	return length == 0xaf && NGPattern::findUnique(body, length,
		schedulerInitFreeFind, nullptr, sizeof(schedulerInitFreeFind), offset) &&
		offset == 0x93;
}

// IGHardwareGuC::initWithOptions() calls the object's virtual free() on every
// failure exit. OSObject::free() deletes the instance, but withOptions() then
// performs a second virtual release through the freed object's vtable. Remove
// only that second dispatch; the following xor ebx,ebx still returns nullptr.
constexpr uint8_t releaseAfterFailedInitFind[] = {
	0x48, 0x8b, 0x03,       // mov (%rbx), %rax
	0x48, 0x89, 0xdf,       // mov %rbx, %rdi
	0xff, 0x50, 0x28,       // call *0x28(%rax)
};
constexpr uint8_t releaseAfterFailedInitReplace[] = {
	0x90, 0x90, 0x90,
	0x90, 0x90, 0x90,
	0x90, 0x90, 0x90,
};

} // namespace NGVfGuCFactoryPatch

namespace NGVfPagePoolPatch {
// Resource-state predicate only. Factory-exclusive ownership and the exact
// native ABI must be established separately before deleting an object.
struct FailedFactoryState {
	uintptr_t object;
	uintptr_t accelerator; // borrowed by the native pool, not retained
	uint64_t availablePages;
	uint64_t nextPageId;
	uintptr_t queue;
	uintptr_t freeHead;
	uintptr_t freeTail;
	uintptr_t interruptSource;
	uintptr_t timerSource;
	uint8_t timerScheduled;
	uintptr_t lock;
};
inline bool failedFactoryStateIsEmpty(const FailedFactoryState &state) {
	return state.object != 0 && state.object <= UINTPTR_MAX - 0x30 &&
		state.accelerator != 0 && state.availablePages == 0 && state.nextPageId == 0 &&
		state.queue == 0 && state.freeHead == 0 && state.freeTail == state.object + 0x30 &&
		state.interruptSource == 0 && state.timerSource == 0 && state.timerScheduled == 0 &&
		state.lock == 0;
}
// initPagePool has created [0, RBX); RBX is the failed factory index.
// Pre-decrement before the first access, then descend through zero. A plain
// INC-to-DEC replacement would still skip the first successfully created pool.
constexpr uint8_t prefixUnwindFind[] = {
	0x48,0x85,0xdb,0x74,0x2c,0x49,0x8b,0x84,0x24,0x10,0x01,0x00,0x00,
	0x48,0x8b,0x3c,0xd8,0x48,0x85,0xff,0x74,0x0e,0x48,0x8b,0x07,
	0xff,0x50,0x28,0x49,0x8b,0x84,0x24,0x10,0x01,0x00,0x00,
	0x48,0xc7,0x04,0xd8,0x00,0x00,0x00,0x00,0x48,0xff,0xc3,0x75,0xd4,
};
constexpr uint8_t prefixUnwindReplace[] = {
	0x48,0xff,0xcb,0x78,0x2c,0x49,0x8b,0x84,0x24,0x10,0x01,0x00,0x00,
	0x48,0x8b,0x3c,0xd8,0x48,0x85,0xff,0x74,0x0e,0x48,0x8b,0x07,
	0xff,0x50,0x28,0x49,0x8b,0x84,0x24,0x10,0x01,0x00,0x00,
	0x48,0xc7,0x04,0xd8,0x00,0x00,0x00,0x00,0x48,0xff,0xcb,0x79,0xd4,
};
inline bool prefixUnwindPreflight(const uint8_t *body, size_t length) {
	size_t offset = 0;
	return length == 0xcc && NGPattern::findUnique(body, length,
		prefixUnwindFind, nullptr, sizeof(prefixUnwindFind), offset) && offset == 0x6b;
}
static_assert(sizeof(prefixUnwindFind) == sizeof(prefixUnwindReplace),
	"pool unwind patch must preserve instruction extent");
} // namespace NGVfPagePoolPatch

#endif
