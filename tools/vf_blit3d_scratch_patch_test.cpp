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

// Selected setup instructions only, not whole-kernel/DMA emulation.
struct CcsSetup {
	uint64_t count = 0, capacity = 0, pointer = 0, r12 = 0;
	size_t destination = 0;
};
static CcsSetup runCcsSetup(const uint8_t *code, size_t length, uint64_t allocation)
{
	CcsSetup out;
	uint64_t rcx = 0;
	size_t pc = 0;
	while (pc < length) {
		if (code[pc] == 0x90) { ++pc; continue; }
		if (code[pc] == 0x48 && code[pc + 1] == 0x85) { pc += 3; continue; }
		if (code[pc] == 0x74) {
			if (!allocation) { out.destination = 0x73c1a + pc + 2 + code[pc + 1]; return out; }
			pc += 2; continue;
		}
		if (code[pc] == 0x0f && code[pc + 1] == 0x84) {
			uint32_t displacement = 0;
			for (size_t i = 0; i < 4; ++i) displacement |= uint32_t(code[pc + 2 + i]) << (8 * i);
			if (!allocation) { out.destination = 0x73c1a + pc + 6 + displacement; return out; }
			pc += 6; continue;
		}
		if (code[pc] == 0xb9) { assert(code[pc + 1] == 1); rcx = 1; pc += 5; continue; }
		if (code[pc] == 0x49) { assert(code[pc + 1] == 0x89 && code[pc + 2] == 0xc4); out.r12 = allocation; pc += 3; continue; }
		const bool byteStore = code[pc] == 0xc6;
		const size_t dispOffset = byteStore ? 2 : 3;
		const uint8_t field = code[pc + dispOffset];
		const uint64_t value = byteStore ? code[pc + 6] : (code[pc + 2] == 0x85 ? allocation : rcx);
		assert(byteStore || (code[pc] == 0x48 && code[pc + 1] == 0x89));
		assert(field == 0x50 || field == 0x58 || field == 0x60);
		if (field == 0x50) out.count = value;
		if (field == 0x58) out.capacity = value;
		if (field == 0x60) out.pointer = value;
		pc += 7;
	}
	out.destination = 0x73c3c;
	return out;
}

static void verify(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	assert(stream);
	std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), {});
	using namespace NGVfBlit3dScratchPatch;
	assert(ccsAllocationPreflight(image.data() + 0x73a20, 0x554));
	assert(!ccsAllocationPreflight(nullptr, 0));
	assert(!ccsAllocationPreflight(image.data() + 0x73a20, 0x553));
	assert(offsetsInRange(image, ccsAllocationFind, 0x73a20, 0x73f74) == std::vector<size_t>{0x73c1a});
	auto duplicate = std::vector<uint8_t>(image.begin() + 0x73a20, image.begin() + 0x73f74);
	std::copy(ccsAllocationFind, ccsAllocationFind + sizeof(ccsAllocationFind), duplicate.begin() + 0x100);
	assert(!ccsAllocationPreflight(duplicate.data(), duplicate.size()));
	auto patched = std::vector<uint8_t>(image.begin() + 0x73a20, image.begin() + 0x73f74);
	std::copy(ccsAllocationReplace, ccsAllocationReplace + sizeof(ccsAllocationReplace), patched.begin() + 0x1fa);
	assert(!ccsAllocationPreflight(patched.data(), patched.size()));
	assert(std::equal(patched.begin(), patched.begin() + 0x1fa, image.begin() + 0x73a20));
	assert(std::equal(patched.begin() + 0x21c, patched.end(), image.begin() + 0x73c3c));
	for (const auto range : {std::pair<size_t, size_t>{0x73c1a, sizeof(ccsAllocationFind)},
	                        {0x73bf8, sizeof(ccsVectorZero)}, {0x73f13, sizeof(ccsFalseCleanup)}}) {
		for (size_t i = 0; i < range.second; ++i) {
			auto mutated = std::vector<uint8_t>(image.begin() + 0x73a20, image.begin() + 0x73f74);
			mutated[range.first - 0x73a20 + i] ^= 1;
			assert(!ccsAllocationPreflight(mutated.data(), mutated.size()));
		}
	}
	const auto failure = runCcsSetup(ccsAllocationReplace, sizeof(ccsAllocationReplace), 0);
	assert(failure.destination == 0x73f13 && failure.count == 0 && failure.capacity == 0 && failure.pointer == 0 && failure.r12 == 0);
	for (uint64_t pointer : {uint64_t{1}, uint64_t{0x1000}, uint64_t{0xffffffff80001000}, UINT64_MAX}) {
		const auto before = runCcsSetup(ccsAllocationFind, sizeof(ccsAllocationFind), pointer);
		const auto after = runCcsSetup(ccsAllocationReplace, sizeof(ccsAllocationReplace), pointer);
		assert(before.destination == after.destination && before.count == after.count &&
		       before.capacity == after.capacity && before.pointer == after.pointer && before.r12 == after.r12);
	}
	assert(runCcsSetup(ccsAllocationFind, sizeof(ccsAllocationFind), 0).destination == 0x73c3c);
	std::printf("PASS: bounded CCS null branch, 68 anchor mutations and selected setup equivalence in %s\n", path);

	// UUID-pinned production bounds use stable exported symbols:
	//   IGHardwareBlit2DContext::initialize()  0x7d912
	//   IGAccelDisplayMachine::MetaClassC1()   0x7dbac
	// The private global constructor is [0x7d98a, 0x7db5c). The 17-byte
	// load/store group is the only match in either range and is unique across
	// the whole image.
	assert(offsetsInRange(image, scratchSizeFind, 0x7d912, 0x7dbac) ==
	       std::vector<size_t> {0x7db20});
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
	const uint32_t replacementDisplacement = static_cast<uint32_t>(
		scratchSizeReplace[3] | (scratchSizeReplace[4] << 8) |
		(scratchSizeReplace[5] << 16) | (scratchSizeReplace[6] << 24));
	const uint64_t replacementStoreTarget =
		0x7db20 + 11 + static_cast<uint64_t>(replacementDisplacement);
	assert(replacementStoreTarget == storeTarget);

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
