/* UUID-pinned Tahoe TGL Blit3D scratch allocation-size repair. */
#ifndef NGREEN_VF_BLIT3D_SCRATCH_PATCH_HPP
#define NGREEN_VF_BLIT3D_SCRATCH_PATCH_HPP

#include <stdint.h>
#include "kern_pattern_match.hpp"

namespace NGVfBlit3dScratchPatch {

// Resource CCS resolve: replace the entire 34-byte allocation-result setup,
// not just its short JE. Null must enter the existing false cleanup at 73f13.
// The vector's three qwords were zeroed at 73bf8. On success, byte stores of 1
// therefore publish exactly the original qword count/capacity. RCX's removed
// constant load is dead: the caller overwrites RCX before its next read.
// This fixes only the CPU null write, not event-admission or DMA retirement.
constexpr uint8_t ccsAllocationFind[] = {
	0x48,0x85,0xc0,0x74,0x1d,0x48,0x89,0x85,0x60,0xff,0xff,0xff,
	0xb9,0x01,0x00,0x00,0x00,0x48,0x89,0x8d,0x58,0xff,0xff,0xff,
	0x48,0x89,0x8d,0x50,0xff,0xff,0xff,0x49,0x89,0xc4,
};
constexpr uint8_t ccsAllocationReplace[] = {
	0x48,0x85,0xc0,0x0f,0x84,0xf0,0x02,0x00,0x00,
	0x48,0x89,0x85,0x60,0xff,0xff,0xff,
	0xc6,0x85,0x58,0xff,0xff,0xff,0x01,
	0xc6,0x85,0x50,0xff,0xff,0xff,0x01,
	0x49,0x89,0xc4,0x90,
};
constexpr uint8_t ccsVectorZero[] = {
	0x45,0x31,0xe4,0x4c,0x89,0xa5,0x50,0xff,0xff,0xff,
	0x4c,0x89,0xa5,0x58,0xff,0xff,0xff,
	0x4c,0x89,0xa5,0x60,0xff,0xff,0xff,
};
constexpr uint8_t ccsFalseCleanup[] = {
	0x48,0x8b,0xbd,0x30,0xff,0xff,0xff,0x48,0x85,0xff,
};
inline bool ccsAllocationPreflight(const uint8_t *body, size_t length) {
	size_t allocation = 0, zero = 0, cleanup = 0;
	return length == 0x554 && NGPattern::findUnique(body, length,
		ccsAllocationFind, nullptr, sizeof(ccsAllocationFind), allocation) &&
		allocation == 0x1fa && NGPattern::findUnique(body, length,
		ccsVectorZero, nullptr, sizeof(ccsVectorZero), zero) && zero == 0x1d8 &&
		NGPattern::findUnique(body + 0x4f3, sizeof(ccsFalseCleanup),
			ccsFalseCleanup, nullptr, sizeof(ccsFalseCleanup), cleanup) && cleanup == 0;
}
static_assert(sizeof(ccsAllocationFind) == 34 &&
	      sizeof(ccsAllocationReplace) == sizeof(ccsAllocationFind),
	"CCS allocation repair must preserve setup length");

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
//   0x7db20 lea rax,[rip+0x33119]   ; load blit3d_scratch_space_size
//   0x7db27 mov rax,[rax]
//   0x7db2a mov [rip+0xcd6df],rax   ; ExtendedCtxParams[0x10]
// Rewrite the final store to an immediate store of 0xe000 and pad the
// removed load pair with NOPs. Keeping the 17-byte length leaves every
// following constructor store untouched. The replacement begins ten bytes
// earlier than the original store, so its RIP-relative displacement must be
// rebased from 0xcd6df to 0xcd6e5 to retain the 0x14b210 destination.
constexpr uint8_t scratchSizeFind[] = {
	0x48, 0x8d, 0x05, 0x19, 0x31, 0x03, 0x00, // lea (%rip),%rax
	0x48, 0x8b, 0x00,                         // mov (%rax),%rax
	0x48, 0x89, 0x05, 0xdf, 0xd6, 0x0c, 0x00, // mov %rax,(%rip)
};
constexpr uint8_t scratchSizeReplace[] = {
	0x48, 0xc7, 0x05, 0xe5, 0xd6, 0x0c, 0x00, // movq $0xe000,(%rip)
	0x00, 0xe0, 0x00, 0x00,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90,       // retain instruction length
};

static_assert(sizeof(scratchSizeFind) == sizeof(scratchSizeReplace) &&
	      sizeof(scratchSizeFind) == 17,
    "Blit3D scratch patch must preserve constructor length");

} // namespace NGVfBlit3dScratchPatch

#endif
