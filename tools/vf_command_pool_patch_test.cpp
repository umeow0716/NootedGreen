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
		assert(image.size() >= 0x8b1c4 && image.size() >= 0x7cfb0 &&
		       image.size() >= 0x35960);
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
		assert(hasReviewedRectListCapacity(image.data() + 0x334b0,
		                                     reviewedRectListSize));
		matches = 0;
		for (size_t offset = 0; offset + sizeof(rectListCapacityFind) <= image.size();
		     ++offset) {
			bool equal = true;
			for (size_t i = 0; i < sizeof(rectListCapacityFind); ++i)
				equal &= image[offset + i] == rectListCapacityFind[i];
			if (equal) {
				assert(offset == 0x33c95);
				++matches;
			}
		}
		assert(matches == 1);
		for (size_t i = 0; i < sizeof(rectListCapacityReplace); ++i)
			image[0x33c95 + i] = rectListCapacityReplace[i];
		const uint32_t limit = static_cast<uint32_t>(image[0x33c98]) |
			static_cast<uint32_t>(image[0x33c99]) << 8 |
			static_cast<uint32_t>(image[0x33c9a]) << 16 |
			static_cast<uint32_t>(image[0x33c9b]) << 24;
		assert(limit == blit3dUsableBytes);
		assert(hasReviewedResolveHizCapacity(image.data() + 0x85a8c,
		                                      reviewedResolveHizSize));
		matches = 0;
		for (size_t offset = 0; offset + sizeof(resolveHizCapacityFind) <= image.size();
		     ++offset) {
			bool equal = true;
			for (size_t i = 0; i < sizeof(resolveHizCapacityFind); ++i)
				equal &= image[offset + i] == resolveHizCapacityFind[i];
			if (equal) {
				assert(offset == 0x85afa);
				++matches;
			}
		}
		assert(matches == 1);
		for (size_t i = 0; i < sizeof(resolveHizCapacityReplace); ++i)
			image[0x85afa + i] = resolveHizCapacityReplace[i];
		const uint32_t request = static_cast<uint32_t>(image[0x85afb]) |
			static_cast<uint32_t>(image[0x85afc]) << 8 |
			static_cast<uint32_t>(image[0x85afd]) << 16 |
			static_cast<uint32_t>(image[0x85afe]) << 24;
		assert(image[0x85afa] == 0xbe && request == resolveUsableDwords);
		assert(image[0x85b0f] == 0x48 && image[0x85b10] == 0x89 &&
		       image[0x85b11] == 0xdf && image[0x85b12] == 0x48 &&
		       image[0x85b13] == 0x89 && image[0x85b14] == 0x55 &&
		       image[0x85b15] == 0xc8);
	}
}
