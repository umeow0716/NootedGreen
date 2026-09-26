// Pure media-12 direct-VF GGTT PTE policy.
#ifndef NGREEN_VF_GGTT_PTE_HPP
#define NGREEN_VF_GGTT_PTE_HPP

#include <stdint.h>

namespace NGVfGgttPte {

constexpr uint64_t pageMask = UINT64_C(0x7FFFFFF000);
constexpr uint64_t present = 1U;

// Tahoe's IGHardwarePageTable::attributeBits can produce only bits 1, 3, 4
// and 7. They describe its physical-driver memory/cache policy. On media-12
// GGTT, however, bits 4:2 are the PF-owned VFID and bit 1 selects local memory.
// A direct integrated-GPU VF must therefore validate, then discard, all four.
constexpr uint64_t appleAttributeMask = UINT64_C(0x9A);

inline bool validAppleAttributes(uint64_t flags)
{
	return (flags & ~appleAttributeMask) == 0;
}

inline uint64_t encodeSystemMemory(uint64_t physical)
{
	return (physical & pageMask) | present;
}

} // namespace NGVfGgttPte

#endif
