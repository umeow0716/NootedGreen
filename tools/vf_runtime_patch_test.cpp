#include "../NootedGreen/kern_vf_runtime_patch.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

template <size_t N>
static size_t uniqueOffset(const std::vector<uint8_t> &image,
	                       const uint8_t (&pattern)[N])
{
	auto first = std::search(image.begin(), image.end(), pattern, pattern + N);
	assert(first != image.end());
	auto second = std::search(first + 1, image.end(), pattern, pattern + N);
	assert(second == image.end());
	return static_cast<size_t>(first - image.begin());
}

static void verify(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	assert(stream);
	std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), {});
	using namespace NGVfRuntimePatch;
	const size_t sku = uniqueOffset(image, spoofedSkuFind);
	const size_t slice = uniqueOffset(image, sliceFuseFind);
	const size_t dss = uniqueOffset(image, dssFuseFind);
	const size_t eu = uniqueOffset(image, euFuseFind);
	const size_t media = uniqueOffset(image, mediaFuseFind);
	const size_t rpm = uniqueOffset(image, rpmConfigFind);
	const size_t l3 = uniqueOffset(image, l3BranchFind);
	// The SKU admission comparison belongs to IntelAccelerator::probe().  The
	// remaining anchors belong to the single 0x48c-byte getGPUInfo body.  Keep
	// the two groups observably separate; the Mach-O symbol contract verifies
	// their exact owning functions.
	assert(sku < slice);
	assert(slice - sku > 0x4000U);
	assert(slice < dss && dss < eu && eu < media && media < rpm && rpm < l3);
	assert(l3 - slice < 0x48cU);
	std::printf("PASS: unique probe/getGPUInfo runtime anchors in %s "
	            "(probe 0x%zx, getGPUInfo 0x%zx..0x%zx)\n",
	            path, sku, slice, l3);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	verify(argv[1]);
	verify(argv[2]);
}
