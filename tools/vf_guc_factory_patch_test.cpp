#include "../NootedGreen/kern_vf_guc_factory_patch.hpp"
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

static void verify(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	assert(stream);
	std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), {});
	using namespace NGVfGuCFactoryPatch;
	using namespace NGVfPagePoolPatch;
	std::vector<uint8_t> poolBody(image.begin() + 0xed9e, image.begin() + 0xee6a);
	assert(prefixUnwindPreflight(poolBody.data(), poolBody.size()));
	assert(!prefixUnwindPreflight(nullptr, poolBody.size()));
	assert(!prefixUnwindPreflight(poolBody.data(), poolBody.size() - 1));
	for (size_t i = 0; i < sizeof(prefixUnwindFind); ++i) {
		auto mutated = poolBody;
		mutated[0x6b + i] ^= 1;
		assert(!prefixUnwindPreflight(mutated.data(), mutated.size()));
	}
	auto duplicatePool = poolBody;
	std::copy(std::begin(prefixUnwindFind), std::end(prefixUnwindFind), duplicatePool.begin());
	assert(!prefixUnwindPreflight(duplicatePool.data(), duplicatePool.size()));
	auto patchedPool = poolBody;
	std::copy(std::begin(prefixUnwindReplace), std::end(prefixUnwindReplace),
	          patchedPool.begin() + 0x6b);
	assert(!prefixUnwindPreflight(patchedPool.data(), patchedPool.size()));
	for (size_t i = 0; i < poolBody.size(); ++i) {
		if (i != 0x6c && i != 0x6d && i != 0x6e && i != 0x99 && i != 0x9a)
			assert(patchedPool[i] == poolBody[i]);
	}
	assert(prefixUnwindReplace[3] == 0x78 && prefixUnwindReplace[4] == 0x2c);
	assert(prefixUnwindReplace[47] == 0x79 && prefixUnwindReplace[48] == 0xd4);
	// The JS and JNS targets are respectively loop exit and first array load.
	assert(5 + static_cast<int8_t>(prefixUnwindReplace[4]) == 49);
	assert(49 + static_cast<int8_t>(prefixUnwindReplace[48]) == 5);
	for (size_t i = 5; i < 44; ++i)
		assert(prefixUnwindReplace[i] == prefixUnwindFind[i]);
	// Model only the decoded DEC/JS/DEC/JNS index control, not DMA or callbacks.
	for (int64_t created = 0; created <= 4096; ++created) {
		int64_t cursor = created - 1;
		int64_t released = 0;
		while (cursor >= 0) {
			assert(cursor == created - released - 1 && cursor < created);
			++released;
			--cursor;
			assert(released <= created);
		}
		assert(released == created && cursor == -1);
	}
	std::printf("PASS: bounded pool prefix unwind and 4097 offline index-control cases in %s\n", path);
	std::vector<uint8_t> body(image.begin() + 0x1d9d2, image.begin() + 0x1da81);
	assert(schedulerInitPreflight(body.data(), body.size()));
	assert(!schedulerInitPreflight(body.data(), body.size() - 1));
	assert(!schedulerInitPreflight(nullptr, body.size()));
	for (size_t index = 0; index < sizeof(schedulerInitFreeFind); ++index) {
		auto mutated = body;
		mutated[0x93 + index] ^= 1;
		const auto before = mutated;
		assert(!schedulerInitPreflight(mutated.data(), mutated.size()));
		assert(mutated == before);
	}
	auto duplicate = body;
	std::copy(std::begin(schedulerInitFreeFind), std::end(schedulerInitFreeFind), duplicate.begin());
	assert(!schedulerInitPreflight(duplicate.data(), duplicate.size()));
	std::fill(duplicate.begin() + 0x93, duplicate.begin() + 0x93 + sizeof(schedulerInitFreeFind), 0);
	assert(!schedulerInitPreflight(duplicate.data(), duplicate.size()));
	assert(offsetsInRange(image, schedulerInitFreeFind, 0x1d9d2, 0x1da81) ==
	       std::vector<size_t> {0x1da65});
	static_assert(sizeof(schedulerInitFreeFind) == sizeof(schedulerInitFreeReplace),
	              "scheduler patch must preserve instruction extent");
	auto patched = image;
	std::copy(std::begin(schedulerInitFreeReplace), std::end(schedulerInitFreeReplace),
	          patched.begin() + 0x1da65);
	for (size_t index = 0; index < image.size(); ++index) {
		if (index >= 0x1da6c && index < 0x1da72)
			assert(patched[index] == 0x90);
		else
			assert(patched[index] == image[index]);
	}
	// Retain factory release for base-init as well as streamer-init failure.
	assert(offsetsInRange(patched, releaseAfterFailedInitFind, 0x1d98a, 0x1d9d2) ==
	       std::vector<size_t> {0x1d9bf});

	// UUID-pinned symbol bounds:
	//   IGHardwareGuC::withOptions     [0x1fdd0, 0x1fe18)
	//   IGHardwareGuC::initWithOptions [0x1fe18, 0x1ffb2)
	assert(offsetsInRange(image, releaseAfterFailedInitFind,
	                      0x1fdd0, 0x1fe18) ==
	       std::vector<size_t> {0x1fe05});
	assert(offsetsInRange(image, releaseAfterFailedInitReplace,
	                      0x1fdd0, 0x1fe18).empty());
	// The removed call is followed by xor ebx,ebx, preserving the null return.
	assert(image[0x1fe0e] == 0x31 && image[0x1fe0f] == 0xdb);
	// Every init failure converges on a virtual free call at 0x1ff94 before
	// returning false; this is the deletion that makes the factory release a
	// use-after-free rather than ordinary reference-count cleanup.
	const uint8_t initFree[] = {
		0x49, 0x8b, 0x07, 0x4c, 0x89, 0xff,
		0xff, 0x90, 0x90, 0x00, 0x00, 0x00,
	};
	assert(offsetsInRange(image, initFree, 0x1fe18, 0x1ffb2) ==
	       std::vector<size_t> {0x1ff94});

	std::printf("PASS: bounded VF GuC factory double-destruction repair in %s\n",
	            path);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	verify(argv[1]);
	verify(argv[2]);
}
