/* Tahoe event-vector growth postconditions used only by the classified VF. */
#ifndef NGREEN_EVENT_VECTOR_HPP
#define NGREEN_EVENT_VECTOR_HPP

#include <stddef.h>
#include <stdint.h>

namespace NGEventVector {

constexpr size_t sizeOffset = 0;
constexpr size_t capacityOffset = 8;
constexpr size_t storageOffset = 0x10;
constexpr size_t reviewedGrowSize = 0x78;

// Both archived instantiations are byte-identical. Avoid the external IOMalloc
// relocation itself and bind the capacity check, allocation-null branch and
// final storage/capacity publication used by the wrapper's rationale.
inline bool hasReviewedGrow(const uint8_t *body, size_t length) {
	constexpr uint8_t prologueAndCapacity[] = {
		0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
		0x53, 0x50, 0x48, 0x39, 0x77, 0x08, 0x73, 0x5b,
	};
	constexpr uint8_t allocationNull[] = {
		0x48, 0x85, 0xc0, 0x74, 0x43,
	};
	constexpr uint8_t publication[] = {
		0x4c, 0x89, 0x7b, 0x10,
		0x4c, 0x89, 0x73, 0x08,
		0xb0, 0x01, 0xeb, 0x02, 0x31, 0xc0,
	};
	if (!body || length != reviewedGrowSize)
		return false;
	for (size_t i = 0; i < sizeof(prologueAndCapacity); ++i)
		if (body[i] != prologueAndCapacity[i])
			return false;
	for (size_t i = 0; i < sizeof(allocationNull); ++i)
		if (body[0x23 + i] != allocationNull[i])
			return false;
	for (size_t i = 0; i < sizeof(publication); ++i)
		if (body[0x5f + i] != publication[i])
			return false;
	return true;
}

inline bool hasConsistentState(size_t size, size_t capacity,
                               uintptr_t storage) {
	if (size > capacity)
		return false;
	return capacity == 0 ? size == 0 && storage == 0 : storage != 0;
}

inline bool hasRepresentableRequest(size_t requested) {
	return requested <= SIZE_MAX / sizeof(uintptr_t);
}

// Native false means either allocation failure or that no growth was needed.
// Judge the published state instead of interpreting AL as a success status.
inline bool hasCapacity(size_t size, size_t capacity, uintptr_t storage,
                        size_t requested) {
	return hasConsistentState(size, capacity, storage) &&
	       hasRepresentableRequest(requested) && capacity >= requested;
}

} // namespace NGEventVector

#endif
