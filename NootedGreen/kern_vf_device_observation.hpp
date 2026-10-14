#ifndef NGREEN_VF_DEVICE_OBSERVATION_HPP
#define NGREEN_VF_DEVICE_OBSERVATION_HPP

#include <stdint.h>
#include <stddef.h>
#include "kern_binary_identity.hpp"

// Owned diagnostic ABI, not an Intel selector or completion certificate.
namespace NGVfDeviceObservation {
constexpr uint32_t selector = 0x4E470001U;
constexpr uint32_t version = 1;
struct Counters { uint32_t restartReports, terminated; };
struct Reply {
    uint32_t abiVersion, bytes;
    uint64_t registryId;
    uint32_t restartReports, terminated;
    uint8_t familyUuid[16];
};
static_assert(sizeof(Reply)==40, "fixed owned observation wire layout");

// Inspect only the kernel-owned argument envelope, never any input buffer.
template<class Args> bool inlineShape(const Args *args, uint32_t currentVersion,
                                    const void *dispatch, const void *target,
                                    const void *reference) noexcept {
    return args && !dispatch && !target && !reference &&
        args->version==currentVersion && args->selector==selector &&
        !args->asyncWakePort && !args->asyncReference && !args->asyncReferenceCount &&
        !args->scalarInput && !args->scalarInputCount && !args->scalarOutput && !args->scalarOutputCount &&
        !args->structureInput && !args->structureInputSize && !args->structureInputDescriptor &&
        !args->structureOutputDescriptor && !args->structureOutputDescriptorSize &&
        !args->structureVariableOutputData && args->structureOutput && args->structureOutputSize==sizeof(Reply);
}

// Caller pins the exact VF instance and exact family ABI. Read must use
// ordered aligned loads, never MMIO or borrowed userspace addresses.
template<class Read> bool collect(uint64_t registryId,Read read,Reply& output) noexcept {
    static_assert(noexcept(read(*static_cast<Counters*>(nullptr))), "ordered counter read ABI");
    if(!registryId) return false;
    Counters first{},second{};
    if(!read(first) || !read(second) || first.restartReports!=second.restartReports ||
        first.terminated!=second.terminated) return false;
    Reply candidate{version,sizeof(Reply),registryId,second.restartReports,second.terminated,{}};
    for(unsigned i=0;i<16;++i) candidate.familyUuid[i]=NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid[i];
    output.abiVersion=candidate.abiVersion;
    output.bytes=candidate.bytes;
    output.registryId=candidate.registryId;
    output.restartReports=candidate.restartReports;
    output.terminated=candidate.terminated;
    for(unsigned i=0;i<16;++i) output.familyUuid[i]=candidate.familyUuid[i];
    return true;
}

// Userspace transport boundary. Caller supplies a lifetime-pinned shared
// connection and the independently admitted accelerator registry ID.
// A well-formed terminated reply remains observable; it is not completion.
inline bool validReply(const Reply& reply,uint64_t expectedRegistryId) noexcept {
    if(!expectedRegistryId || reply.abiVersion!=version || reply.bytes!=sizeof(Reply) ||
       reply.registryId!=expectedRegistryId) return false;
    for(unsigned i=0;i<16;++i)
        if(reply.familyUuid[i]!=NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid[i]) return false;
    return true;
}
template<class Call> bool query(uint64_t expectedRegistryId,Call call,Reply& output) noexcept {
    static_assert(noexcept(call(selector,static_cast<void*>(nullptr),static_cast<size_t*>(nullptr))),
                  "owned struct-method transport ABI");
    if(!expectedRegistryId) return false;
    Reply candidate{};size_t bytes=sizeof(candidate);
    if(call(selector,&candidate,&bytes)!=0 || bytes!=sizeof(candidate) ||
       !validReply(candidate,expectedRegistryId)) return false;
    output.abiVersion=candidate.abiVersion;output.bytes=candidate.bytes;
    output.registryId=candidate.registryId;output.restartReports=candidate.restartReports;
    output.terminated=candidate.terminated;
    for(unsigned i=0;i<16;++i) output.familyUuid[i]=candidate.familyUuid[i];
    return true;
}
}
#endif
