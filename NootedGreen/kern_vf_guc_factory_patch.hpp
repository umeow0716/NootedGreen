/* Exact failed-init factory repair for the UUID-pinned Tahoe TGL accelerator. */
#ifndef NGREEN_VF_GUC_FACTORY_PATCH_HPP
#define NGREEN_VF_GUC_FACTORY_PATCH_HPP

#include <stdint.h>

namespace NGVfGuCFactoryPatch {

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
