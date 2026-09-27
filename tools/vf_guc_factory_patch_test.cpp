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
