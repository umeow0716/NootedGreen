#include "../NootedGreen/kern_vf_legacy_ctb.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
	using namespace NGVfLegacyCtb;
	constexpr uint32_t base = 0x12345000U;
	constexpr uint32_t token = 0x89ABCDEFU;
	uint64_t cases = 0;

	for (uint32_t type = 0; type <= 2U; ++type) {
		std::array<uint32_t, 4> request = {
			registerAction,
			base + (type == 1U ? legacyG2HDescriptorOffset : 0U),
			legacyDescriptorBytes,
			type,
		};
		for (size_t length = 0; length <= 5U; ++length) {
			assert(registration(request.data(), length, base) ==
			       (type <= 1U && length == request.size()));
			++cases;
		}
		for (size_t word = 0; word < request.size(); ++word) {
			auto mutated = request;
			mutated[word] ^= 1U << ((word * 7U) & 31U);
			assert(!registration(mutated.data(), mutated.size(), base));
			++cases;
		}
	}
	assert(!registration(nullptr, 4, base));
	assert(!registration(std::array<uint32_t, 4>{
		registerAction, 0x000003FFU, legacyDescriptorBytes, 1U}.data(), 4,
		0xFFFFFFFFU));
	cases += 2;

	for (uint32_t type = 0; type <= 2U; ++type) {
		std::array<uint32_t, 3> request = {deregisterAction, token, type};
		for (size_t length = 0; length <= 4U; ++length) {
			assert(deregistration(request.data(), length, token) ==
			       (type <= 1U && length == request.size()));
			++cases;
		}
		for (size_t word = 0; word < request.size(); ++word) {
			auto mutated = request;
			mutated[word] ^= 1U << ((word * 11U) & 31U);
			assert(!deregistration(mutated.data(), mutated.size(), token));
			++cases;
		}
	}
	assert(!deregistration(nullptr, 3, token));
	++cases;

	std::printf("PASS: %llu legacy CTB request contracts\n",
	            static_cast<unsigned long long>(cases));
}
