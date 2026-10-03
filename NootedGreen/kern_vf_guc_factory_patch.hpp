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

#endif
