#include "../NootedGreen/kern_vf_standalone_patch.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

template <size_t N>
static std::vector<size_t> offsets(const std::vector<uint8_t> &image,
	                               const uint8_t (&pattern)[N])
{
	std::vector<size_t> found;
	auto cursor = image.begin();
	while (cursor != image.end()) {
		auto match = std::search(cursor, image.end(), pattern, pattern + N);
		if (match == image.end())
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
	const auto found = offsets(image, NGVfStandalonePatch::waitFind);
	assert(found == std::vector<size_t> {0x24947});
	assert(offsets(image, NGVfStandalonePatch::waitReplace).empty());
	std::printf("PASS: unique VF framebuffer-wait anchor in %s\n", path);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	verify(argv[1]);
	verify(argv[2]);
}
