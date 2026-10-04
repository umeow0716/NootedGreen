#include "../NootedGreen/kern_ioaccel_command_pool.hpp"
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

using namespace NGIOAccelCommandPool;

static std::vector<uint8_t> readFile(const char *path) {
	std::ifstream input(path, std::ios::binary);
	assert(input.good());
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {});
}

int main(int argc, char **argv) {
	assert(argc == 3);
	for (int argument = 1; argument < argc; ++argument) {
		auto image = readFile(argv[argument]);
		assert(image.size() >= 0x7cfb0);
		assert(hasReviewedExtendedInitContract(image.data() + 0x7cebc,
		                                      reviewedExtendedInitSize));
		size_t matches = 0;
		for (size_t offset = 0; offset + sizeof(extendedInitFind) <= image.size();
		     ++offset) {
			bool equal = true;
			for (size_t i = 0; i < sizeof(extendedInitFind); ++i)
				equal &= image[offset + i] == extendedInitFind[i];
			if (equal) {
				assert(offset == 0x7cf55);
				++matches;
			}
		}
		assert(matches == 1);
		for (size_t i = 0; i < sizeof(extendedInitReplace); ++i)
			image[0x7cf55 + i] = extendedInitReplace[i];
		assert(image[0x7cf5a] == 0x74 &&
		       0x7cf5c + static_cast<int8_t>(image[0x7cf5b]) == 0x7cfa3);
		assert(image[0x7cf64] == 0x74 &&
		       0x7cf66 + static_cast<int8_t>(image[0x7cf65]) == 0x7cf7b);
	}
}
