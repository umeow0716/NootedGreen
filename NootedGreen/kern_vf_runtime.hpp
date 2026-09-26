/*
 * Intel SR-IOV early MMIO relay and media-12 topology helpers.
 *
 * The wire layout mirrors i915's iov_actions_mmio_abi.h.  Keep this header
 * freestanding so the protocol encoder and fuse decoder can be exhaustively
 * tested without booting macOS or touching hardware.
 */
#ifndef NGREEN_VF_RUNTIME_HPP
#define NGREEN_VF_RUNTIME_HPP

#include <stdint.h>

namespace NGVfRuntime {

constexpr uint32_t actionMmioRelay = 0x5005U;
constexpr uint8_t opcodeHandshake = 0x01U;
constexpr uint8_t opcodeGetRuntime = 0x10U;
constexpr uint16_t iovMajor = 1U;
constexpr uint16_t iovMinor = 0U;

constexpr uint32_t originGuc = 0x80000000U;
constexpr uint32_t typeMask = 0x70000000U;
constexpr uint32_t typeSuccess = 0x70000000U;
constexpr uint32_t magicMask = 0x0F000000U;
constexpr uint32_t data0Mask = 0x00FFFFFFU;

constexpr uint32_t rpmConfig0 = 0x0D00U;
constexpr uint32_t mirrorFuse3 = 0x9118U;
constexpr uint32_t euDisable = 0x9134U;
constexpr uint32_t sliceEnable = 0x9138U;
constexpr uint32_t geometryDssEnable = 0x913CU;
constexpr uint32_t veboxVdboxDisable = 0x9140U;
constexpr uint32_t ctcMode = 0xA26CU;
constexpr uint32_t hucKernelLoadInfo = 0xC1DCU;

struct RelayRequest {
	uint32_t words[4];
	uint8_t magic;
};

struct Registers {
	uint32_t rpmConfig0;
	uint32_t mirrorFuse3;
	uint32_t euDisable;
	uint32_t sliceEnable;
	uint32_t geometryDssEnable;
	uint32_t veboxVdboxDisable;
	uint32_t ctcMode;
	uint32_t hucKernelLoadInfo;
};

struct Topology {
	uint32_t sliceCount;
	uint32_t traditionalSubsliceCount;
	uint32_t maxEusPerSubslice;
	uint32_t euCount;
	uint32_t l3BankCount;
	uint32_t geometryDssMask;
	uint32_t enabledMediaMask;
};

inline uint32_t crc32LeWordStream(const uint32_t words[4])
{
	// Same reflected polynomial and seed used by crc32_le(0, request, 16).
	uint32_t crc = 0;
	for (uint32_t i = 0; i < 4; i++) {
		for (uint32_t byte = 0; byte < 4; byte++) {
			crc ^= (words[i] >> (byte * 8U)) & 0xFFU;
			for (uint32_t bit = 0; bit < 8; bit++)
				crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
		}
	}
	return crc;
}

inline RelayRequest makeRelayRequest(uint8_t opcode, uint32_t data1,
	                                  uint32_t data2, uint32_t data3)
{
	RelayRequest request = {{
		actionMmioRelay | (static_cast<uint32_t>(opcode) << 16U),
		data1, data2, data3,
	}, 0};
	request.magic = static_cast<uint8_t>(crc32LeWordStream(request.words) & 0xFU);
	request.words[0] |= static_cast<uint32_t>(request.magic) << 24U;
	return request;
}

inline bool validRelayResponse(const uint32_t response[4], uint8_t magic,
	                            bool requireZeroData0 = true)
{
	if (!response || (response[0] & originGuc) != originGuc ||
	    (response[0] & typeMask) != typeSuccess ||
	    ((response[0] & magicMask) >> 24U) != magic)
		return false;
	return !requireZeroData0 || (response[0] & data0Mask) == 0;
}

inline uint32_t popcount32(uint32_t value)
{
	uint32_t count = 0;
	while (value) {
		value &= value - 1U;
		count++;
	}
	return count;
}

inline bool deriveTopology(const Registers &registers, Topology &topology)
{
	// media-12 (TGL/ADL/RPL) exposes one slice and at most six geometry DSS.
	const uint32_t sliceMask = registers.sliceEnable & 0xFFU;
	const uint32_t dssMask = registers.geometryDssEnable & 0x3FU;
	const uint32_t euDisabled = registers.euDisable & 0xFFU;
	const uint32_t l3Pairs = popcount32((~registers.mirrorFuse3) & 0xFU);
	const uint32_t mediaMask = (~registers.veboxVdboxDisable) & 0x000F00FFU;
	const uint32_t slices = popcount32(sliceMask);
	const uint32_t dss = popcount32(dssMask);
	const uint32_t eusPerTraditionalSubslice = 8U - popcount32(euDisabled);

	// Reserved crystal-clock encodings must not enter Apple's timestamp math.
	const uint32_t crystalClockEncoding = (registers.rpmConfig0 >> 3U) & 0x7U;
	if (slices != 1U || dss == 0U || dss > 6U ||
	    eusPerTraditionalSubslice == 0U || eusPerTraditionalSubslice > 8U ||
	    l3Pairs == 0U || l3Pairs > 4U || crystalClockEncoding > 3U ||
	    (mediaMask & 0xFFU) == 0U || (mediaMask & 0x000F0000U) == 0U)
		return false;

	topology.sliceCount = slices;
	topology.traditionalSubsliceCount = dss * 2U;
	topology.maxEusPerSubslice = eusPerTraditionalSubslice;
	topology.euCount = topology.traditionalSubsliceCount * eusPerTraditionalSubslice;
	topology.l3BankCount = l3Pairs * 2U;
	topology.geometryDssMask = dssMask;
	topology.enabledMediaMask = mediaMask;
	return topology.euCount <= 96U;
}

inline void writeImmediate32(uint8_t *destination, uint32_t value)
{
	destination[0] = static_cast<uint8_t>(value);
	destination[1] = static_cast<uint8_t>(value >> 8U);
	destination[2] = static_cast<uint8_t>(value >> 16U);
	destination[3] = static_cast<uint8_t>(value >> 24U);
}

} // namespace NGVfRuntime

#endif
