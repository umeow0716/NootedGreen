// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.
#pragma once

#include "kern_ggtt_bounds.hpp"
#include <stdint.h>

namespace NGGgttRotation {

constexpr uint64_t PageSize = UINT64_C(0x1000);

struct Spec {
	uint64_t assignmentBase;
	uint64_t assignmentSize;
	uint64_t rangeStart;
	uint64_t rangeLength;
	uint64_t physicalLength;
	uint64_t cursor;
	uint32_t sourcePage;
	uint32_t widthPages;
	uint32_t heightPages;
};

inline bool valid(const Spec &spec) {
	if (!NGGgtt::contains(spec.assignmentBase, spec.assignmentSize,
	                      spec.rangeStart, spec.rangeLength) ||
	    !spec.rangeLength || spec.physicalLength != spec.rangeLength ||
	    spec.sourcePage != 0 || !spec.widthPages || !spec.heightPages ||
	    spec.widthPages > UINT64_MAX / spec.heightPages)
		return false;
	const uint64_t pages = spec.rangeLength / PageSize;
	const uint64_t matrixPages =
		static_cast<uint64_t>(spec.widthPages) * spec.heightPages;
	if (pages != matrixPages || pages > UINT32_MAX)
		return false;
	const uint64_t initialPage = spec.heightPages - 1U;
	return initialPage < pages &&
	       spec.cursor == spec.rangeStart + initialPage * PageSize;
}

// Tahoe's iterator maps a row-major source page to a 90-degree rotated,
// column-major destination: col * height + (height - 1 - row).
inline bool destination(const Spec &spec, uint64_t sourcePage,
	                    uint64_t &address) {
	if (!valid(spec))
		return false;
	const uint64_t pages = spec.rangeLength / PageSize;
	if (sourcePage >= pages)
		return false;
	const uint64_t row = sourcePage / spec.widthPages;
	const uint64_t column = sourcePage % spec.widthPages;
	const uint64_t destinationPage =
		column * spec.heightPages + (spec.heightPages - 1U - row);
	if (destinationPage >= pages)
		return false;
	address = spec.rangeStart + destinationPage * PageSize;
	return true;
}

} // namespace NGGgttRotation
