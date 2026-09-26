#include "../NootedGreen/kern_vf_tlb_patch.hpp"
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
	using namespace NGVfTlbPatch;

	// This is the complete 0xCEE8 inventory in both admitted accelerator
	// payloads. Seven native bodies remain callable and are bounded-patched;
	// five other bodies are isolated at their public entry routes.
	expect(offsets(image, writeImmediateFind), {
		0x193e2, 0x1e483, 0x1e529, 0x1f43c, 0x1f5b9,
		0x20b1c, 0x213de, 0x219cf, 0x21afe, 0x22230,
	});
	expect(offsets(image, writeEcxFind), {0x20314});
	expect(offsets(image, writeR8Find), {0x21fae});
	expect(offsets(image, pollEcxFind), {
		0x193ec, 0x1e48d, 0x1e533, 0x1f446, 0x2031a,
		0x20b26, 0x21b08, 0x21fb5, 0x2223a,
	});
	expect(offsets(image, pollEdxFind), {0x213e8});
	expect(offsets(image, pollEsiFind), {0x219d9});
	expect(offsets(image, pollMemoryFind), {0x1f5c3});
	expect(offsets(image, ctbInitFind), {0x1f43c});

	std::printf("PASS: complete physical-TLB inventory in %s\n", path);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	verify(argv[1]);
	verify(argv[2]);
}
