#include "../NootedGreen/kern_vf_runtime.hpp"
#include "../NootedGreen/kern_vf_device_observation.hpp"
#include <cassert>
#include <cstdio>

int main()
{
	using namespace NGVfRuntime;
	{
		using namespace NGVfDeviceObservation;
		struct Envelope {
			uint32_t version=2, selector=NGVfDeviceObservation::selector;
			uintptr_t asyncWakePort=0, asyncReference=0, asyncReferenceCount=0;
			uintptr_t scalarInput=0, scalarInputCount=0, scalarOutput=0, scalarOutputCount=0;
			uintptr_t structureInput=0, structureInputSize=0, structureInputDescriptor=0;
			uintptr_t structureOutputDescriptor=0, structureOutputDescriptorSize=0, structureVariableOutputData=0;
			uintptr_t structureOutput=1, structureOutputSize=sizeof(Reply);
		} envelope;
		assert(inlineShape(&envelope,2,nullptr,nullptr,nullptr));
		assert(!inlineShape<Envelope>(nullptr,2,nullptr,nullptr,nullptr));
		for (unsigned i=0;i<3;++i)
			assert(!inlineShape(&envelope,2,i==0?&envelope:nullptr,i==1?&envelope:nullptr,i==2?&envelope:nullptr));
		uintptr_t Envelope::* forbidden[] = {
			&Envelope::asyncWakePort,&Envelope::asyncReference,&Envelope::asyncReferenceCount,
			&Envelope::scalarInput,&Envelope::scalarInputCount,&Envelope::scalarOutput,&Envelope::scalarOutputCount,
			&Envelope::structureInput,&Envelope::structureInputSize,&Envelope::structureInputDescriptor,
			&Envelope::structureOutputDescriptor,&Envelope::structureOutputDescriptorSize,&Envelope::structureVariableOutputData};
		for(auto field:forbidden) {
			envelope.*field=1;assert(!inlineShape(&envelope,2,nullptr,nullptr,nullptr));envelope.*field=0;
		}
		envelope.version=1;assert(!inlineShape(&envelope,2,nullptr,nullptr,nullptr));envelope.version=2;
		envelope.selector=0;assert(!inlineShape(&envelope,2,nullptr,nullptr,nullptr));envelope.selector=NGVfDeviceObservation::selector;
		envelope.structureOutput=0;assert(!inlineShape(&envelope,2,nullptr,nullptr,nullptr));envelope.structureOutput=1;
		const size_t wrongSizes[]={sizeof(Reply)-1,sizeof(Reply)+1};
		for(auto size:wrongSizes) {
			envelope.structureOutputSize=size;assert(!inlineShape(&envelope,2,nullptr,nullptr,nullptr));
		}
		Reply output{};unsigned reads=0;
		auto stable=[&](Counters& value) noexcept {++reads;value={7,0};return true;};
		assert(collect(0x12345678,stable,output) && reads==2);
		assert(output.abiVersion==1 && output.bytes==40 && output.registryId==0x12345678 &&
		    output.restartReports==7 && !output.terminated);
		for(unsigned i=0;i<16;++i) assert(output.familyUuid[i]==NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid[i]);
		const Reply saved=output;
		reads=0;assert(!collect(0,stable,output) && !reads);
		for(unsigned mode=0;mode<3;++mode) {
			reads=0;
			auto changed=[&](Counters& value) noexcept {
				++reads;value={uint32_t(mode==0?reads:7),uint32_t(mode==1?reads:0)};
				return mode!=2 || reads!=2;
			};
			assert(!collect(0x12345678,changed,output) && reads==2);
			assert(output.registryId==saved.registryId && output.restartReports==saved.restartReports &&
			    output.terminated==saved.terminated);
		}
		assert(collect(0x12345678,[](Counters& value) noexcept {value={8,1};return true;},output));
		assert(output.terminated==1); // Raw terminated observation, never hidden or treated as completion.
		const Reply nativeReply=output;
		for(unsigned mode=0;mode<8;++mode) {
			Reply observed=saved;unsigned calls=0;
			auto call=[&](uint32_t method,void *wire,size_t *bytes) noexcept {
				++calls;assert(method==NGVfDeviceObservation::selector && *bytes==sizeof(Reply));
				auto& reply=*static_cast<Reply*>(wire);reply=nativeReply;
				if(mode==1) *bytes-=1;
				if(mode==2) *bytes+=1;
				if(mode==3) reply.abiVersion+=1;
				if(mode==4) reply.bytes-=1;
				if(mode==5) reply.registryId+=1;
				if(mode==6) reply.familyUuid[15]^=1;
				return mode==7?31:0;
			};
			assert(query(nativeReply.registryId,call,observed)==(mode==0) && calls==1);
			assert(observed.restartReports==(mode==0?nativeReply.restartReports:saved.restartReports));
			assert(observed.terminated==(mode==0?1U:saved.terminated));
			calls=0;assert(!query(0,call,observed) && !calls);
		}
	}

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
