#include "../NootedGreen/kern_vf_guc_request.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

using NGVfGuCRequest::inspect;

template <size_t N>
static void valid(const uint32_t (&request)[N], bool retirement,
	              uint32_t credits)
{
	const auto attributes = inspect(request, N);
	assert(attributes.valid);
	assert(attributes.retirement == retirement);
	assert(attributes.responseCredits == credits);
	for (size_t length = 0; length <= 13; ++length) {
		if (length == N)
			continue;
		assert(!inspect(request, length).valid);
	}
}

int main()
{
	const uint32_t registration[] = {
		0x4502, 1, 12, 4, 1U << 3, 0, 0, 0, 0, 0, 0x1234500d, 0,
	};
	const uint32_t policy[] = {
		0x100B, 12,
		0x20030001, 2,
		0x20010001, 1000,
		0x20020001, 7500000,
		0x20050001, 0,
	};
	const uint32_t schedule[] = {0x1000, 12};
	const uint32_t enable[] = {0x1001, 12, 1};
	const uint32_t disable[] = {0x1001, 12, 0};
	const uint32_t deregister[] = {0x4503, 12};
	const uint32_t invalidate[] = {0x7000, 0x89abcdef, 0x80000003};
	valid(registration, false, 0);
	valid(policy, false, 0);
	valid(schedule, false, 0);
	valid(enable, false, 4);
	valid(disable, true, 4);
	valid(deregister, true, 3);
	valid(invalidate, true, 3);

	auto badRegistration = std::array<uint32_t, 12>({
		0x4502, 1, 12, 4, 8, 0, 0, 0, 0, 0, 0x1234500d, 0,
	});
	for (size_t field : {size_t(1), size_t(5), size_t(6), size_t(7),
	                     size_t(8), size_t(9), size_t(11)}) {
		badRegistration[field] = field == 1 ? 0 : 1;
		assert(!inspect(badRegistration.data(), badRegistration.size()).valid);
		badRegistration[field] = field == 1 ? 1 : 0;
	}
	badRegistration[2] = 0xFFFF;
	assert(!inspect(badRegistration.data(), badRegistration.size()).valid);
	badRegistration[2] = 12;
	badRegistration[3] = 6;
	assert(!inspect(badRegistration.data(), badRegistration.size()).valid);
	badRegistration[3] = 4;
	badRegistration[4] = 3;
	assert(!inspect(badRegistration.data(), badRegistration.size()).valid);
	badRegistration[4] = 8;
	badRegistration[10] = 0xd;
	assert(!inspect(badRegistration.data(), badRegistration.size()).valid);

	auto badPolicy = std::array<uint32_t, 10>({
		0x100B, 12, 0x20030001, 2, 0x20010001, 1000,
		0x20020001, 7500000, 0x20050001, 0,
	});
	for (size_t field : {size_t(2), size_t(4), size_t(6), size_t(8)}) {
		badPolicy[field] ^= 1U;
		assert(!inspect(badPolicy.data(), badPolicy.size()).valid);
		badPolicy[field] ^= 1U;
	}
	badPolicy[3] = 3;
	assert(!inspect(badPolicy.data(), badPolicy.size()).valid);
	badPolicy[3] = 2;
	badPolicy[5] = 0;
	assert(!inspect(badPolicy.data(), badPolicy.size()).valid);
	badPolicy[5] = 1000;
	badPolicy[7] = 0;
	assert(!inspect(badPolicy.data(), badPolicy.size()).valid);
	badPolicy[7] = 7500000;
	badPolicy[9] = 1;
	assert(!inspect(badPolicy.data(), badPolicy.size()).valid);
	badPolicy[9] = 0;
	badPolicy[7] = 640000;
	assert(inspect(badPolicy.data(), badPolicy.size()).valid);

	const uint32_t badMode[] = {0x1001, 12, 2};
	const uint32_t badId[] = {0x1000, 0xFFFF};
	const uint32_t badInvalidate[] = {0x7000, 1, 0x80000001};
	const uint32_t unknown[] = {0x1234, 0};
	assert(!inspect(badMode, 3).valid);
	assert(!inspect(badId, 2).valid);
	assert(!inspect(badInvalidate, 3).valid);
	assert(!inspect(unknown, 2).valid);
	assert(!inspect(nullptr, 0).valid);

	std::puts("PASS: GuC v70 FAST-request shapes and reply-credit contracts");
}
