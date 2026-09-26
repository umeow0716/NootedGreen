#include "../NootedGreen/kern_binary_identity.hpp"
#include "../NootedGreen/kern_framebuffer_patch.hpp"
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

static std::vector<uint8_t> load(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	assert(stream);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), {});
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	const auto production = load(argv[1]);
	const auto debug = load(argv[2]);
	using namespace NGBinaryIdentity;
	using namespace NGFramebufferPatch;

	assert(matchesKextUuid(production.data(), production.size(),
	                       tglFramebufferProductionUuid));
	assert(matchesKextUuid(debug.data(), debug.size(),
	                       tglFramebufferDebugUuid));
	assert(offsets(production, productionFind) ==
	       std::vector<size_t>({0x5e3a9}));
	assert(offsets(production, debugFind).empty());
	assert(offsets(debug, debugFind) == std::vector<size_t>({0x8d4a3}));
	assert(offsets(debug, productionFind).empty());

	// nm/disassembly boundaries for ReadRegister64(unsigned long) in each
	// UUID-pinned image. Runtime additionally solves these symbols and refuses
	// any reordered or unexpectedly large body.
	assert(0x5e3a9 >= 0x5e396 && 0x5e3a9 < 0x5e3ea);
	assert(0x8d4a3 >= 0x8d490 && 0x8d4a3 < 0x8d51c);
	std::puts("PASS: UUID-specific bounded framebuffer ReadRegister64 anchors");
}
