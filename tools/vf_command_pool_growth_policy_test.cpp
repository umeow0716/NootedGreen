#include "../NootedGreen/kern_ioaccel_command_pool.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>

using namespace NGIOAccelCommandPool;

int main() {
	ControlFrame before {};
	for (size_t index = 0; index < slotCapacity; ++index)
		before.slot(index, true, index % 2 == 0, index == slotCapacity - 1);
	for (size_t word = 0; word < 8; ++word) {
		assert(before.memory[word] == UINT32_MAX);
		assert(before.gpu[word] == 0x55555555U);
		assert(before.cpu[word] == (word == 7 ? 0x80000000U : 0));
	}
	ControlFrame after = before;
	after.slot(31, false, true, true);
	after.slot(slotCapacity, true, true, true);
	assert(after.cpu[0] == 0x80000000U && before.cpu[0] == 0);
	assert(after.cpu[7] == before.cpu[7]);
	assert(boundedOffset(0x1000, 0x1004, 4) == 4);
	assert(boundedOffset(0, 0x1004, UINT32_MAX) == UINT64_MAX);
	assert(boundedOffset(0x1000, 0xffc, UINT32_MAX) == UINT64_MAX);
	assert(boundedOffset(0x1000, UINTPTR_MAX, UINT32_MAX) == UINT64_MAX);
	assert(boundedOffset(UINTPTR_MAX - 3, UINTPTR_MAX, 3) == 3);
	ControlFrame maximal {};
	maximal.maximum = maximal.count = UINT16_MAX;
	maximal.current = -1;
	maximal.bytes = maximal.reserved = maximal.allocationOptions =
		maximal.mappingOptions = maximal.links = UINT32_MAX;
	maximal.alignment = UINT64_MAX;
	for (size_t index = 0; index < slotCapacity; ++index)
		maximal.slot(index, true, true, true);
	char panicText[1024] {};
	const int panicLength = std::snprintf(panicText, sizeof(panicText),
		"NootedGreen    ngreen: @ V272: refusing inadequate VF command buffer request=%u current=%d count=%u"
		" V362: reason=%s remaining=%llu BEFORE" NG_POOL_FRAME_FORMAT
		" AFTER" NG_POOL_FRAME_FORMAT,
		UINT32_MAX, -32768, UINT16_MAX, "start-mapping-mismatch", ~0ULL,
		NG_POOL_FRAME_ARGS(maximal), NG_POOL_FRAME_ARGS(maximal));
	assert(panicLength > 0 && panicLength < 950);
	assert(std::strlen(panicText) == static_cast<size_t>(panicLength));
	assert(std::strstr(panicText, " AFTER max=") != nullptr);
	std::vector<uint8_t> body(reviewedGrowthSize, 0);
	const uint8_t anchor[] = {
		0x66, 0x89, 0x83, 0x32, 0x18, 0x00, 0x00,
		0x0f, 0xbf, 0x75, 0xc8, 0x48, 0x89, 0xdf,
		0xe8, 0x7b, 0x02, 0x00, 0x00,
		0x41, 0xb6, 0x01, 0xe9, 0x13, 0x01, 0x00, 0x00,
	};
	for (size_t i = 0; i < sizeof(anchor); ++i)
		body[0xc2 + i] = anchor[i];
	assert(hasReviewedGrowthContract(body.data(), body.size()));
	assert(!hasReviewedGrowthContract(nullptr, body.size()));
	assert(!hasReviewedGrowthContract(body.data(), body.size() - 1));
	for (size_t i = 0; i < sizeof(anchor); ++i) {
		body[0xc2 + i] ^= 1;
		assert(!hasReviewedGrowthContract(body.data(), body.size()));
		body[0xc2 + i] ^= 1;
	}
	std::vector<uint8_t> getter(reviewedGetBufferSize, 0);
	const uint8_t getterInitial[] = {
		0x48, 0x8b, 0x87, 0x58, 0x18, 0x00, 0x00,
		0x41, 0x89, 0xf6, 0x4a, 0x8d, 0x0c, 0xb0,
		0x48, 0x3b, 0x8f, 0x50, 0x18, 0x00, 0x00,
	};
	const uint8_t getterSelection[] = {
		0x0f, 0xbf, 0xf1, 0x48, 0x89, 0xdf,
		0xe8, 0x04, 0xfe, 0xff, 0xff,
	};
	const uint8_t getterPublish[] = {
		0x48, 0x89, 0x8b, 0x50, 0x18, 0x00, 0x00,
		0x48, 0x89, 0x83, 0x58, 0x18, 0x00, 0x00,
	};
	for (size_t i = 0; i < sizeof(getterInitial); ++i)
		getter[0x7 + i] = getterInitial[i];
	for (size_t i = 0; i < sizeof(getterSelection); ++i)
		getter[0x6f + i] = getterSelection[i];
	for (size_t i = 0; i < sizeof(getterPublish); ++i)
		getter[0xfa + i] = getterPublish[i];
	assert(hasReviewedGetBufferContract(getter.data(), getter.size()));
	assert(!hasReviewedGetBufferContract(nullptr, getter.size()));
	assert(!hasReviewedGetBufferContract(getter.data(), getter.size() - 1));
	for (auto range : {std::pair<size_t, size_t> {0x7, sizeof(getterInitial)},
	                   std::pair<size_t, size_t> {0x6f, sizeof(getterSelection)},
	                   std::pair<size_t, size_t> {0xfa, sizeof(getterPublish)}}) {
		for (size_t i = 0; i < range.second; ++i) {
			getter[range.first + i] ^= 1;
			assert(!hasReviewedGetBufferContract(getter.data(), getter.size()));
			getter[range.first + i] ^= 1;
		}
	}
	std::vector<uint8_t> extended(reviewedExtendedInitSize, 0);
	for (size_t i = 0; i < sizeof(extendedInitFind); ++i)
		extended[0x99 + i] = extendedInitFind[i];
	assert(hasReviewedExtendedInitContract(extended.data(), extended.size()));
	assert(!hasReviewedExtendedInitContract(nullptr, extended.size()));
	assert(!hasReviewedExtendedInitContract(extended.data(), extended.size() - 1));
	for (size_t i = 0; i < sizeof(extendedInitFind); ++i) {
		extended[0x99 + i] ^= 1;
		assert(!hasReviewedExtendedInitContract(extended.data(), extended.size()));
		extended[0x99 + i] ^= 1;
	}
	for (uint32_t request = 0; request <= blit3dBufferBytes * 2; request += 64) {
		const bool admitted = request <= blit3dUsableBytes;
		assert(admitted == (request <= 0xffc0));
	}

	auto checkValid = [](uint16_t maximum, uint16_t previous) {
		const uint16_t published = previous == 0 ? 1 : previous * 2;
		assert(completedGrowth(true, maximum, previous, published,
		                       static_cast<int16_t>(previous), 1, 2, 3));
		assert(!completedGrowth(false, maximum, previous, published,
		                        static_cast<int16_t>(previous), 1, 2, 3));
		assert(!completedGrowth(true, maximum, previous,
		                        static_cast<uint16_t>(published - 1),
		                        static_cast<int16_t>(previous), 1, 2, 3));
		assert(!completedGrowth(true, maximum, previous, published,
		                        static_cast<int16_t>(previous == 0 ? -1 : previous - 1),
		                        1, 2, 3));
		for (unsigned missing = 0; missing < 3; ++missing)
			assert(!completedGrowth(true, maximum, previous, published,
			                        static_cast<int16_t>(previous),
			                        missing == 0 ? 0 : 1,
			                        missing == 1 ? 0 : 2,
			                        missing == 2 ? 0 : 3));
	};
	for (uint16_t maximum = 1; maximum <= slotCapacity; maximum <<= 1) {
		checkValid(maximum, 0);
		for (uint16_t previous = 1; previous < maximum; previous <<= 1)
			checkValid(maximum, previous);
	}
	assert(!completedGrowth(true, 0, 0, 1, 0, 1, 2, 3));
	assert(!completedGrowth(true, slotCapacity + 1, 0, 1, 0, 1, 2, 3));
	assert(!completedGrowth(true, slotCapacity, slotCapacity,
	                        slotCapacity, 0, 1, 2, 3));
	assert(!completedGrowth(true, 3, 2, 4, 2, 1, 2, 3));

	constexpr uintptr_t start = 0x1000;
	constexpr uintptr_t end = start + 0xff8;
	for (uint32_t request = 0; request <= 0x400; ++request)
		assert(hasReturnedCapacity(8, 1, 0, 1, 2, start,
		                           start, end, start, start, request) ==
		       (request <= 0x3fe));
	constexpr uintptr_t partial = start + 0x9b8;
	assert(hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                           start, end, partial, partial, 0x18e));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            start, end, partial, partial, 0x3fe));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            start, end, start, start, UINT32_MAX));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            start, end, start, start + 4, 1));
	assert(!hasReturnedCapacity(8, 1, -1, 1, 2, start,
	                            start, end, start, start, 1));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 0, start,
	                            start, end, start, start, 1));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start,
	                            end, start, start, start, 1));
	assert(!hasReturnedCapacity(8, 1, 0, 1, 2, start + 4,
	                            start, end, start, start, 1));
	for (uint32_t request = 0; request <= 0x400; ++request) {
		const char *reason = returnedCapacityRefusal(8, 1, 0, 1, 2, start,
			start, end, start, start, request);
		assert(std::strcmp(reason, request <= 0x3fe ? "none" : "capacity") == 0);
	}
	struct RefusalCase { uint16_t maximum,count; int16_t current;
		uintptr_t memory,gpu,cpu,begin,limit,cursor,returned; const char *reason; };
	const RefusalCase refusals[] = {
		{0,1,0,1,2,start,start,end,start,start,"slot-state"},
		{257,1,0,1,2,start,start,end,start,start,"slot-state"},
		{8,0,0,1,2,start,start,end,start,start,"slot-state"},
		{8,1,-1,1,2,start,start,end,start,start,"slot-state"},
		{8,1,1,1,2,start,start,end,start,start,"slot-state"},
		{8,1,0,0,2,start,start,end,start,start,"memory-missing"},
		{8,1,0,1,0,start,start,end,start,start,"gpu-mapping-missing"},
		{8,1,0,1,2,0,start,end,start,start,"cpu-mapping-missing"},
		{8,1,0,1,2,start,start,end,0,0,"return-null"},
		{8,1,0,1,2,start+4,start,end,start,start,"start-mapping-mismatch"},
		{8,1,0,1,2,start,start,end,start,start+4,"return-cursor-mismatch"},
		{8,1,0,1,2,start,start,end,start-4,start-4,"return-range"},
		{8,1,0,1,2,start,start,end,end+4,end+4,"return-range"},
		{8,1,0,1,2,start,start,end,end,end,"capacity"},
	};
	for (const auto &item : refusals) {
		assert(!hasReturnedCapacity(item.maximum,item.count,item.current,
			item.memory,item.gpu,item.cpu,item.begin,item.limit,item.cursor,item.returned,1));
		assert(std::strcmp(returnedCapacityRefusal(item.maximum,item.count,item.current,
			item.memory,item.gpu,item.cpu,item.begin,item.limit,item.cursor,item.returned,1),item.reason)==0);
	}
}
