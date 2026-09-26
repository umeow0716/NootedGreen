#include "../NootedGreen/kern_vf_runtime.hpp"
#include <cassert>
#include <cstdio>

int main()
{
	using namespace NGVfRuntime;

	auto handshake = makeRelayRequest(opcodeHandshake,
		(static_cast<uint32_t>(iovMajor) << 16U) | iovMinor, 0, 0);
	assert((handshake.words[0] & 0xFFFFU) == actionMmioRelay);
	assert(((handshake.words[0] >> 16U) & 0xFFU) == opcodeHandshake);
	assert(((handshake.words[0] >> 24U) & 0xFU) == handshake.magic);
	assert((handshake.words[0] & 0xF0000000U) == 0);

	auto runtime = makeRelayRequest(opcodeGetRuntime, mirrorFuse3,
		euDisable, sliceEnable);
	assert(runtime.words[1] == mirrorFuse3);
	assert(runtime.words[2] == euDisable);
	assert(runtime.words[3] == sliceEnable);
	const uint32_t runtimeWithoutMagic[4] = {
		actionMmioRelay | (static_cast<uint32_t>(opcodeGetRuntime) << 16U),
		mirrorFuse3, euDisable, sliceEnable,
	};
	assert(runtime.magic == (crc32LeWordStream(runtimeWithoutMagic) & 0xFU));

	uint32_t response[4] = {
		originGuc | typeSuccess | (static_cast<uint32_t>(runtime.magic) << 24U),
		0x1234, 0x5678, 0x9ABC,
	};
	assert(validRelayResponse(response, runtime.magic));
	response[0] |= 1U;
	assert(!validRelayResponse(response, runtime.magic));
	assert(validRelayResponse(response, runtime.magic, false));
	response[0] ^= 1U << 24U;
	assert(!validRelayResponse(response, runtime.magic, false));

	Registers target = {
		0x10U,       // supported crystal clock encoding
		0xFFFFFFF0U, // all four L3 bank pairs enabled
		0x00U,       // all eight EU pairs enabled
		0x01U,       // one slice
		0x0FU,       // four DSS / eight traditional subslices
		0x000E00FEU, // one VDBOX and one VEBOX enabled
		0, 0,
	};
	Topology topology = {};
	assert(deriveTopology(target, topology));
	assert(topology.sliceCount == 1);
	assert(topology.traditionalSubsliceCount == 8);
	assert(topology.maxEusPerSubslice == 8);
	assert(topology.euCount == 64);
	assert(topology.l3BankCount == 8);
	assert(topology.geometryDssMask == 0x0F);

	Registers invalid = target;
	invalid.sliceEnable = 0;
	assert(!deriveTopology(invalid, topology));
	invalid = target;
	invalid.geometryDssEnable = 0;
	assert(!deriveTopology(invalid, topology));
	invalid = target;
	invalid.euDisable = 0xFF;
	assert(!deriveTopology(invalid, topology));
	invalid = target;
	invalid.mirrorFuse3 = 0xFFFFFFFFU;
	assert(!deriveTopology(invalid, topology));
	invalid = target;
	invalid.rpmConfig0 = 7U << 3U;
	assert(!deriveTopology(invalid, topology));
	invalid = target;
	invalid.veboxVdboxDisable = 0xFFFFFFFFU;
	assert(!deriveTopology(invalid, topology));

	uint8_t immediate[4] = {};
	writeImmediate32(immediate, 0xA1B2C3D4U);
	assert(immediate[0] == 0xD4 && immediate[1] == 0xC3 &&
	       immediate[2] == 0xB2 && immediate[3] == 0xA1);

	std::puts("PASS: VF MMIO relay encoding and media-12 runtime topology");
}
