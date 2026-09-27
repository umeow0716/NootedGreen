/* UUID-pinned Tahoe TGL Blit3D scratch allocation-size repair. */
#ifndef NGREEN_VF_BLIT3D_SCRATCH_PATCH_HPP
#define NGREEN_VF_BLIT3D_SCRATCH_PATCH_HPP

#include <stdint.h>

namespace NGVfBlit3dScratchPatch {

// Tahoe's extended-context factory constructor loads
// blit3d_scratch_space_size (0xd240) and stores it into the shared
// ExtendedCtxParams record. IGHardwareBlit3DContext::initialize() then fills
// that scratch space through offset 0xd20f, while the VF-only
// IGSharedMappedBuffer CPU mapping ends at 0xd000. The final blend-state
// page write is therefore a kernel data fault before GuC ever runs.
// Preserve the native initializer and ownership and instead round the
// published allocation size up to a 0xe000 page-aligned value. The private
// consumer only indexes scratch contents, never the value itself, so the
// extra mapping slack is inert.
//
// The native sequence is three instructions sharing one global:
//   0x7db0d lea rax,[rip+0x33119]   ; load blit3d_scratch_space_size
//   0x7db14 mov rax,[rax]
//   0x7db17 mov rax,[rax]
//   0x7db1a mov [rip+0xcd6df],rax   ; ExtendedCtxParams[0x10]
// Rewrite the final store to an immediate store of 0xe000 and pad the
// removed load pair with NOPs. Keeping the 17-byte length leaves every
// following constructor store untouched.
constexpr uint8_t scratchSizeFind[] = {
	0x48, 0x8d, 0x05, 0x19, 0x31, 0x03, 0x00, // lea (%rip),%rax
	0x48, 0x8b, 0x00,                         // mov (%rax),%rax
	0x48, 0x89, 0x05, 0xdf, 0xd6, 0x0c, 0x00, // mov %rax,(%rip)
};
constexpr uint8_t scratchSizeReplace[] = {
	0x48, 0xc7, 0x05, 0xdf, 0xd6, 0x0c, 0x00, // movq $0xe000,(%rip)
	0x00, 0xe0, 0x00, 0x00,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90,       // retain instruction length
};

static_assert(sizeof(scratchSizeFind) == sizeof(scratchSizeReplace) &&
	      sizeof(scratchSizeFind) == 17,
    "Blit3D scratch patch must preserve constructor length");

} // namespace NGVfBlit3dScratchPatch

#endif
