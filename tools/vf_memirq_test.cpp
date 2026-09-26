#include "../NootedGreen/kern_vf_memirq.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
	using namespace NGVfMemIrq;
	static_assert(engineRouteCount == 6, "Tahoe exposes six engine callbacks");
	static_assert(contextRegisterDwords == 0x5A,
	              "context image must include register dword 0x59");

	const EngineRoute expected[] = {
		{0, 0}, {4, 1}, {15, 2}, {32, 3}, {34, 4}, {63, 5},
	};
	for (size_t i = 0; i < engineRouteCount; ++i) {
		assert(engineRoutes[i].irqOffset == expected[i].irqOffset);
		assert(engineRoutes[i].callbackBit == expected[i].callbackBit);
	}

	for (uint32_t offset = 0; offset < sourceBytes; ++offset) {
		uint32_t callback = 0xFFFFFFFFU;
		const bool found = routeForIrqOffset(offset, callback);
		bool expectedFound = false;
		for (const auto &route : expected) {
			if (route.irqOffset == offset) {
				expectedFound = true;
				assert(callback == route.callbackBit);
			}
		}
		assert(found == expectedFound);
	}
	uint32_t callback = 0;
	assert(!routeForIrqOffset(33, callback));
	assert(routeForIrqOffset(34, callback) && callback == 4);

	// Exhaust every combination of the six active engine source bytes.
	for (uint32_t mask = 0; mask < (1U << engineRouteCount); ++mask) {
		std::array<uint8_t, sourceBytes> source = {};
		uint64_t pending = 0;
		for (size_t i = 0; i < engineRouteCount; ++i) {
			if (mask & (1U << i))
				source[engineRoutes[i].irqOffset] = 0xFF;
			if (source[engineRoutes[i].irqOffset])
				pending |= 1ULL << engineRoutes[i].callbackBit;
		}
		assert(pending == mask);
	}

	constexpr uint32_t page = 0x12345000U;
	std::array<uint32_t, 0x60> registers;
	registers.fill(0xDEADBEEFU);
	assert(prepareContextRegisters(registers.data(), registers.size(), page));
	for (size_t i = 0; i < registers.size(); ++i) {
		uint32_t expectedValue = 0xDEADBEEFU;
		switch (i) {
			case 0x50: expectedValue = lrmHeader; break;
			case 0x51: expectedValue = ringInterruptMask; break;
			case 0x52: expectedValue = page + enableOffset; break;
			case 0x53: expectedValue = 0; break;
			case 0x55: expectedValue = lriHeader; break;
			case 0x56: expectedValue = ringInterruptStatus; break;
			case 0x57: expectedValue = page + statusOffset; break;
			case 0x58: expectedValue = ringInterruptSource; break;
			case 0x59: expectedValue = page + sourceOffset; break;
			default: break;
		}
		assert(registers[i] == expectedValue);
	}

	const auto unchanged = registers;
	assert(!prepareContextRegisters(nullptr, contextRegisterDwords, page));
	assert(!prepareContextRegisters(registers.data(),
	                               contextRegisterDwords - 1, page));
	assert(!prepareContextRegisters(registers.data(), registers.size(), 0));
	assert(!prepareContextRegisters(registers.data(), registers.size(), page + 1));
	assert(registers == unchanged);
	assert(prepareContextRegisters(registers.data(), registers.size(),
	                               0xFFFFF000U));

	std::puts("PASS: Gen12 VF memory-IRQ routes and context image programming");
	return 0;
}
