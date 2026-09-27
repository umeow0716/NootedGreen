// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.

#pragma once

#include <stdint.h>

namespace NGTglCompat {
constexpr uint64_t pageBytes = 0x1000;
constexpr uint32_t dsbBytes = 0x12000;
constexpr uint32_t ggttSmallBytes = 0x100000;
constexpr uint32_t ggttLargeBytes = 0x800000;
constexpr uint64_t gen12PteAddressMask = 0x00003FFFFFFFF000ULL;
constexpr uint64_t gen8PagePresent = 1ULL;
constexpr uint32_t portTxDflexDpsp1 = 0x1638A0;

inline bool validDsbLayout(uint32_t size, uint64_t gpuOffset,
                           uint32_t ggttBytes)
{
	if (size != dsbBytes || (gpuOffset & (pageBytes - 1U)) != 0 ||
	    (ggttBytes != ggttSmallBytes && ggttBytes != ggttLargeBytes))
		return false;
	const uint64_t firstEntry = gpuOffset / pageBytes;
	const uint64_t entryCount = size / pageBytes;
	const uint64_t tableEntries = ggttBytes / sizeof(uint64_t);
	return firstEntry <= tableEntries && entryCount <= tableEntries - firstEntry;
}

inline bool encodeGgttPte(uint64_t physical, uint64_t &pte)
{
	if ((physical & (pageBytes - 1U)) != 0 ||
	    (physical & ~gen12PteAddressMask) != 0)
		return false;
	pte = physical | gen8PagePresent;
	return true;
}

inline bool validGgttPhysicalRange(uint64_t physical, uint64_t bytes)
{
	if (!bytes || (bytes & (pageBytes - 1U)) != 0 ||
	    bytes - pageBytes > UINT64_MAX - physical)
		return false;
	uint64_t first = 0, last = 0;
	return encodeGgttPte(physical, first) &&
	       encodeGgttPte(physical + bytes - pageBytes, last);
}

inline uint32_t portMode(uint32_t portType, uint32_t flexDpsp,
                         bool registerValid)
{
	if (portType <= 1)
		return 1;
	if (!registerValid || portType > 5)
		return 0;
	const uint32_t shift = 6U + (portType - 2U) * 8U;
	if (flexDpsp & (1U << shift))
		return 2;
	if (flexDpsp & (1U << (shift - 1U)))
		return 3;
	return 0;
}
}
