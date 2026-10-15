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
// Separate task-bound request. A response is meaningful only when produced
// under native SharedUserClient detach admission AND pinned context coverage;
// these wire types alone do not authorize retirement.
namespace NGVfTaskCompletion {
constexpr uint32_t selector=0x4E470002U, version=1, channels=8;
struct Request { uint32_t abiVersion,bytes; uint64_t expected[channels]; };
struct Record { uint32_t channel,contextSlot; uint64_t publishedSerial; uint32_t stamp,head; };
struct Reply {
    uint32_t abiVersion,bytes;uint64_t registryId;
    uint32_t recordCount,reserved;uint8_t familyUuid[16];Record records[channels];
};
static_assert(sizeof(Request)==72 && sizeof(Record)==24 && sizeof(Reply)==232,"task completion wire ABI");
template<class Args> bool inlineShape(const Args* args,uint32_t currentVersion,
    const void* dispatch,const void* target,const void* reference) noexcept {
    return args && !dispatch && !target && !reference && args->version==currentVersion &&
        args->selector==selector && !args->asyncWakePort && !args->asyncReference && !args->asyncReferenceCount &&
        !args->scalarInput && !args->scalarInputCount && !args->scalarOutput && !args->scalarOutputCount &&
        args->structureInput && args->structureInputSize==sizeof(Request) && !args->structureInputDescriptor &&
        args->structureOutput && args->structureOutputSize==sizeof(Reply) &&
        !args->structureOutputDescriptor && !args->structureOutputDescriptorSize && !args->structureVariableOutputData;
}
inline bool validRequest(const Request& request) noexcept {
    if(request.abiVersion!=version || request.bytes!=sizeof(Request))return false;
    uint32_t used=0;
    for(unsigned i=0;i<channels;++i) {
        const uint32_t channel=uint32_t(request.expected[i]);
        if(channel==UINT32_MAX)continue;
        if(channel>=channels || (used&(1U<<channel)))return false;
        used|=1U<<channel;
    }
    return used!=0;
}
inline bool validReply(const Reply& reply,uint64_t registryId,const Request& request) noexcept {
    if(!validRequest(request) || !registryId || reply.registryId!=registryId ||
        reply.abiVersion!=version || reply.bytes!=sizeof(Reply) || reply.reserved ||
        !reply.recordCount || reply.recordCount>channels)return false;
    for(unsigned n=0;n<16;++n)if(reply.familyUuid[n]!=NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid[n])return false;
    uint32_t required=0,seen=0;
    for(auto event:request.expected)if(uint32_t(event)!=UINT32_MAX)required|=1U<<uint32_t(event);
    for(unsigned n=0;n<reply.recordCount;++n) {
        const auto& record=reply.records[n];
        if(record.channel>=channels || record.contextSlot>=65535 || !record.publishedSerial ||
            (record.head&3) || !(required&(1U<<record.channel)) || (seen&(1U<<record.channel)))return false;
        for(unsigned previous=0;previous<n;++previous)
            if(reply.records[previous].contextSlot==record.contextSlot)return false;
        bool matches=false;
        for(auto event:request.expected)if(uint32_t(event)==record.channel && uint32_t(event>>32)==record.stamp)matches=true;
        if(!matches)return false;
        seen|=1U<<record.channel;
    }
    for(unsigned n=reply.recordCount;n<channels;++n) {
        const auto& record=reply.records[n];
        if(record.channel || record.contextSlot || record.publishedSerial || record.stamp || record.head)return false;
    }
    return seen==required;
}
template<class Call> bool query(uint64_t registryId,const Request& request,Call call,Reply& output) noexcept {
    if(!registryId || !validRequest(request))return false;
    Reply candidate{};size_t bytes=sizeof(candidate);
    if(call(selector,&request,sizeof(request),&candidate,&bytes)!=0 || bytes!=sizeof(candidate) ||
        !validReply(candidate,registryId,request))return false;
    output=candidate;return true;
}
}
#endif
