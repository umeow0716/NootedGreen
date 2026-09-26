/* Pure Gen12 VF memory-interrupt layout and context-image programming. */
#ifndef NGREEN_VF_MEMIRQ_HPP
#define NGREEN_VF_MEMIRQ_HPP

#include <stddef.h>
#include <stdint.h>

namespace NGVfMemIrq {

constexpr uint32_t statusOffset = 0x000;
constexpr uint32_t sourceOffset = 0x400;
constexpr uint32_t enableOffset = 0x440;
constexpr uint32_t gucIrqOffset = 25;
constexpr uint32_t statusStride = 16;
constexpr size_t sourceBytes = 64;

struct EngineRoute {
	uint8_t irqOffset;
	uint8_t callbackBit;
};

// intel_engines[].irq_offset on Gen12 media-12 platforms, mapped to Tahoe's
// IGHwCsType callback bit order. These platforms expose VCS0 and VCS2; the
// second video engine is deliberately offset 34, not the absent VCS1 at 33.
constexpr EngineRoute engineRoutes[] = {
	{0, 0},   // RCS0
	{4, 1},   // CCS0
	{15, 2},  // BCS0
	{32, 3},  // VCS0
	{34, 4},  // VCS2
	{63, 5},  // VECS0
};
constexpr size_t engineRouteCount =
	sizeof(engineRoutes) / sizeof(engineRoutes[0]);

inline bool routeForIrqOffset(uint32_t irqOffset, uint32_t &callbackBit)
{
	for (size_t i = 0; i < engineRouteCount; ++i) {
		if (engineRoutes[i].irqOffset == irqOffset) {
			callbackBit = engineRoutes[i].callbackBit;
			return true;
		}
	}
	return false;
}

constexpr size_t contextRegisterDwords = 0x5A;
constexpr uint32_t lrmHeader = 0x14C80002U;
constexpr uint32_t lriHeader = 0x11081003U;
constexpr uint32_t ringInterruptMask = 0xA8;
constexpr uint32_t ringInterruptStatus = 0xAC;
constexpr uint32_t ringInterruptSource = 0xA4;

inline bool validPage(uint32_t page)
{
	return page != 0 && (page & 0xFFFU) == 0 &&
		page <= UINT32_MAX - enableOffset;
}

inline bool prepareContextRegisters(volatile uint32_t *registers,
	                                size_t dwordCount, uint32_t page)
{
	if (!registers || dwordCount < contextRegisterDwords || !validPage(page))
		return false;

	registers[0x50] = lrmHeader;
	registers[0x51] = ringInterruptMask;
	registers[0x52] = page + enableOffset;
	registers[0x53] = 0;
	registers[0x55] = lriHeader;
	registers[0x56] = ringInterruptStatus;
	registers[0x57] = page + statusOffset;
	registers[0x58] = ringInterruptSource;
	registers[0x59] = page + sourceOffset;
	return true;
}

} // namespace NGVfMemIrq

#endif
