#include "../NootedGreen/kern_tgl_compat.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
	using namespace NGTglCompat;
	assert(validDsbLayout(dsbBytes, 0, ggttSmallBytes));
	assert(validDsbLayout(dsbBytes,
		(ggttSmallBytes / sizeof(uint64_t) - dsbBytes / pageBytes) * pageBytes,
		ggttSmallBytes));
	assert(!validDsbLayout(dsbBytes - pageBytes, 0, ggttSmallBytes));
	assert(!validDsbLayout(dsbBytes, 1, ggttSmallBytes));
	assert(!validDsbLayout(dsbBytes, 0, ggttSmallBytes / 2));
	assert(!validDsbLayout(dsbBytes,
		(ggttSmallBytes / sizeof(uint64_t) - dsbBytes / pageBytes + 1) * pageBytes,
		ggttSmallBytes));
	assert(!validDsbLayout(dsbBytes, UINT64_MAX & ~(pageBytes - 1U),
		ggttLargeBytes));

	uint64_t pte = 0;
	assert(encodeGgttPte(0, pte) && pte == gen8PagePresent);
	assert(encodeGgttPte(0x12345000, pte));
	assert(pte == 0x12345001);
	assert((pte & 0x1E) == 0); // no LM/VFID bits from HookCase's former | 7
	assert(!encodeGgttPte(0x12345001, pte));
	assert(!encodeGgttPte(1ULL << 46U, pte));
	assert(validGgttPhysicalRange(0x1000, dsbBytes));
	assert(validGgttPhysicalRange(
		gen12PteAddressMask - dsbBytes + pageBytes, dsbBytes));
	assert(!validGgttPhysicalRange(0, 0));
	assert(!validGgttPhysicalRange(1, dsbBytes));
	assert(!validGgttPhysicalRange(0x1000, dsbBytes - 1));
	assert(!validGgttPhysicalRange(
		gen12PteAddressMask - dsbBytes + 2 * pageBytes, dsbBytes));
	assert(!validGgttPhysicalRange(UINT64_MAX & ~(pageBytes - 1U),
		dsbBytes));

	assert(portMode(0, 0, false) == 1);
	assert(portMode(1, 0, false) == 1);
	for (uint32_t type = 2; type <= 5; type++) {
		const uint32_t shift = 6U + (type - 2U) * 8U;
		assert(portMode(type, 1U << shift, true) == 2);
		assert(portMode(type, 1U << (shift - 1U), true) == 3);
		assert(portMode(type, 0, true) == 0);
		assert(portMode(type, UINT32_MAX, false) == 0);
	}
	assert(portMode(6, UINT32_MAX, true) == 0);

	std::puts("PASS: TGL DSB GGTT and FIA port-mode compatibility contracts");
	return 0;
}
