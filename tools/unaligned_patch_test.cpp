#include "../NootedGreen/kern_unaligned_patch.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
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

static void expect(const std::vector<size_t> &actual,
	               std::initializer_list<size_t> expected)
{
	assert(actual == std::vector<size_t>(expected));
}

static void verify(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	assert(stream);
	std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), {});
	using namespace NGUnalignedPatch;

	// All anchors lie inside blit3d_submit_rectlist (0x334b0..0x35960).
	// The sixth short match begins one byte into a movapd instruction and
	// intentionally changes its shared opcode byte to the unaligned form.
	expect(offsets(image, movaps10Find), {0x35069});
	expect(offsets(image, movaps30Find), {0x35071});
	expect(offsets(image, movaps50Find), {0x3507c});
	expect(offsets(image, movaps00Find), {
		0x350cf, 0x350e3, 0x350ff, 0x35113, 0x35130, 0x3514e,
	});
	expect(offsets(image, movaps20Find), {
		0x350ee, 0x3511e, 0x3513b, 0x35159,
	});
	expect(offsets(image, movaps40Find), {0x35169});

	std::printf("PASS: bounded rect-list aligned-store inventory in %s\n", path);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	verify(argv[1]);
	verify(argv[2]);
}
