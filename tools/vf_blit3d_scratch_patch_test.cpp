#include "../NootedGreen/kern_vf_blit3d_scratch_patch.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

template <size_t N>
static std::vector<size_t> offsetsInRange(const std::vector<uint8_t> &image,
	                                      const uint8_t (&pattern)[N],
	                                      size_t begin, size_t end)
{
	assert(begin <= end && end <= image.size());
	std::vector<size_t> found;
	auto cursor = image.begin() + begin;
	const auto finish = image.begin() + end;
	while (cursor != finish) {
		auto match = std::search(cursor, finish, pattern, pattern + N);
		if (match == finish)
			break;
		found.push_back(static_cast<size_t>(match - image.begin()));
		cursor = match + 1;
	}
	return found;
}

static uint64_t le64(const std::vector<uint8_t> &image, size_t offset)
{
	uint64_t value = 0;
	for (size_t index = 0; index < 8; ++index)
		value |= static_cast<uint64_t>(image[offset + index]) << (index * 8);
	return value;
}

static void verify(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	assert(stream);
	std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), {});
	using namespace NGVfBlit3dScratchPatch;

	// UUID-pinned extended-context constructor symbols:
	//   __GLOBAL__sub_I_IGHardwareContext.cpp [0x7d98a, 0x7db5c)
	//   __GLOBAL__D_a                         0x7db5c
	// The 17-byte load/store group is the only match inside that range and
	// is unique across the whole image.
	assert(offsetsInRange(image, scratchSizeFind, 0x7d98a, 0x7db5c) ==
	       std::vector<size_t> {0x7db20});
	assert(offsetsInRange(image, scratchSizeFind, 0, image.size()) ==
	       std::vector<size_t> {0x7db20});
	assert(offsetsInRange(image, scratchSizeReplace,
	                      0x7d98a, 0x7db5c).empty());

	// The load group dereferences the pinned blit3d_scratch_space_size
	// object at 0xb0c40, whose native value is the non-page-aligned
	// 0xd240 that causes the final page fault.
	const uint32_t loadDisplacement = static_cast<uint32_t>(
		scratchSizeFind[3] | (scratchSizeFind[4] << 8) |
		(scratchSizeFind[5] << 16) | (scratchSizeFind[6] << 24));
	const uint64_t loadTarget =
		0x7db27 + static_cast<uint64_t>(loadDisplacement);
	assert(loadTarget == 0xb0c40);
	assert(le64(image, loadTarget) == 0xd240);

	// The shared-store displacement targets the same ExtendedCtxParams
	// record slot as the original RIP-relative store.
	const uint32_t storeDisplacement = static_cast<uint32_t>(
		scratchSizeFind[13] | (scratchSizeFind[14] << 8) |
		(scratchSizeFind[15] << 16) | (scratchSizeFind[16] << 24));
	const uint64_t storeTarget =
		0x7db2a + 7 + static_cast<uint64_t>(storeDisplacement);
	assert(storeTarget == 0x14b210);
	assert(scratchSizeReplace[0] == 0x48 && scratchSizeReplace[1] == 0xc7 &&
	       scratchSizeReplace[2] == 0x05);
	assert(scratchSizeReplace[7] == 0x00 && scratchSizeReplace[8] == 0xe0 &&
	       scratchSizeReplace[9] == 0x00 && scratchSizeReplace[10] == 0x00);
	assert(storeTarget == 0x14b210);

	// Padding must be exactly the removed two-instruction load pair length
	// so the following constructor stores keep their addresses.
	for (size_t index = 11; index < sizeof(scratchSizeReplace); ++index)
		assert(scratchSizeReplace[index] == 0x90);

	std::printf("PASS: bounded Blit3D scratch allocation repair in %s\n", path);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	verify(argv[1]);
	verify(argv[2]);
}
