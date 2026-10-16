#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "../../NootedGreen/kern_avc_gen12_commands.hpp"
#include "../../NootedGreen/kern_vf_device_observation.hpp"

// Exact signed producer ABI, not a library-validation or ownership bypass.
// A successful read does not authorize a write: the caller still needs the
// live native builder interval and a kernel protocol handshake.
namespace NgreenNativeAvcReservation {
struct Builder {
    uintptr_t argument=0,connection=0,base=0,cursor=0,end=0;
    uint32_t previousUsed=0,classes=0;
};
template<class Read,class Executable>
bool abi(uintptr_t image,Read read,Executable executable) noexcept {
    if(!image || image>UINTPTR_MAX-0x23e115)return false;
    // Complete used-byte arithmetic, class walk and both native import calls.
    constexpr unsigned char bytes[]{
        0x49,0x8d,0x5e,0x44,0x45,0x31,0xff,
        0x8b,0x53,0xdc,0x2b,0x53,0xcc,0x03,0x13,0x89,0x13,
        0x49,0x8b,0x7e,0x08,0x44,0x89,0xfe,0xe8,0x3f,0x38,0x23,0x00,
        0x48,0xc7,0x43,0xf4,0x00,0x00,0x00,0x00,
        0x49,0xff,0xc7,0x41,0x8b,0x46,0x58,0x48,0x83,0xc3,0x40,
        0x49,0x39,0xc7,0x72,0xd2,
        0x49,0x8b,0x7e,0x08,0x41,0x8b,0x76,0x50,0x49,0x8d,0x5e,0x54,
        0x48,0x8d,0x4d,0xd4,0x48,0x89,0xda,0xe8,0x15,0x38,0x23,0x00};
    unsigned char actual[sizeof(bytes)]{};
    return executable(image+0xa8ae,sizeof(bytes)) &&
        read(image+0xa8ae,actual,sizeof(actual)) && !std::memcmp(actual,bytes,sizeof(bytes));
}
template<class Read>
bool capture(uintptr_t argument,Read read,Builder& output) noexcept {
    output={};
    if(!argument || (argument&7U) || argument>UINTPTR_MAX-0x5c)return false;
    unsigned char bytes[0x5c]{};
    if(!read(argument,bytes,sizeof(bytes)))return false;
    Builder candidate;candidate.argument=argument;
    static_assert(sizeof(uintptr_t)==8,"native Tahoe x86_64 producer ABI");
    std::memcpy(&candidate.connection,bytes+0x08,8);
    std::memcpy(&candidate.base,bytes+0x10,8);
    std::memcpy(&candidate.end,bytes+0x18,8);
    std::memcpy(&candidate.cursor,bytes+0x20,8);
    std::memcpy(&candidate.previousUsed,bytes+0x44,4);
    std::memcpy(&candidate.classes,bytes+0x58,4);
    // Only the captured single-class AVC producer is reviewed. Do not infer
    // an arbitrary multi-class layout from the first class's cursor fields.
    if(!candidate.connection || candidate.classes!=1 || !candidate.base ||
       ((candidate.base|candidate.end|candidate.cursor|candidate.previousUsed)&3U) ||
       candidate.cursor<candidate.base || candidate.end<candidate.cursor ||
       candidate.cursor-candidate.base>6144)return false;
    output=candidate;return true;
}
inline bool unchanged(const Builder& a,const Builder& b) noexcept {
    return a.argument==b.argument && a.connection==b.connection && a.base==b.base &&
        a.cursor==b.cursor && a.end==b.end && a.previousUsed==b.previousUsed && a.classes==b.classes;
}
inline NgreenAvcGen12::Reservation plan(const Builder& builder,size_t outputWords) noexcept {
    if(!builder.argument || !builder.connection || builder.classes!=1)return {};
    return NgreenAvcGen12::reserve(builder.base,builder.cursor,builder.end,builder.previousUsed,
        (builder.cursor-builder.base)/4,outputWords);
}
struct CommitResult {
    bool applied=false,mustStop=false;
    size_t extraBytes=0;
    uint32_t usedBytes=0;
    const char* reason="admission";
};
// Owns no mapping or lock. The caller's SAME native builder interval and
// physical/context lease must surround every callback through native submit.
// Only unused tail and the aligned producer cursor are written; neither the
// original commands nor native sideband offsets/counts are translated here.
template<class Read,class Write,class Writable,class Live>
CommitResult commit(uintptr_t argument,const NGVfAvcPublication::Reply& capability,uint64_t registryId,
    uint32_t width,uint32_t height,NgreenAvcGen12::PublishScratch& scratch,
    Read read,Write write,Writable writable,Live live) noexcept {
    CommitResult result;Builder before,again;
    if(!NGVfAvcPublication::ready(capability,registryId) || !live() || !capture(argument,read,before))return result;
    const size_t inputWords=(before.cursor-before.base)/4;
    if(inputWords<2 || inputWords>1024 || !read(before.base,scratch.source,inputWords*4))return result;
    const auto converted=NgreenAvcGen12::produce(scratch.source,inputWords,width,height,
        scratch.produced,1536);
    if(!converted.ok){result.reason=converted.reason;return result;}
    const auto reservation=plan(before,converted.words);
    if(!reservation.ok || !writable(before.cursor,reservation.extraBytes) ||
       !writable(argument+0x20,8) || !live())return result;
    if(!capture(argument,read,again) || !unchanged(before,again) ||
       !read(before.base,scratch.undo,inputWords*4) ||
       std::memcmp(scratch.source,scratch.undo,inputWords*4))return result;
    // Output commands are needed only to calculate the reservation size.
    // Keep the original input frozen; use the other arrays for tail undo/zeros.
    if(!read(before.cursor,scratch.undo,reservation.extraBytes))return result;
    std::memset(scratch.produced,0,reservation.extraBytes);
    if(!live() || !capture(argument,read,again) || !unchanged(before,again))return result;
    auto restore=[&]() noexcept {
        Builder current;
        if(!live() || !capture(argument,read,current))return false;
        Builder original=current;original.cursor=before.cursor;
        if(!unchanged(before,original) ||
           (current.cursor!=before.cursor && current.cursor!=reservation.cursor))return false;
        if(current.cursor!=before.cursor) {
            if(!write(argument+0x20,&before.cursor,8) ||
               !capture(argument,read,current) || !unchanged(before,current))return false;
        }
        if(!read(before.cursor,scratch.produced,reservation.extraBytes))return false;
        if(std::memcmp(scratch.undo,scratch.produced,reservation.extraBytes) &&
           !write(before.cursor,scratch.undo,reservation.extraBytes))return false;
        if(!read(before.cursor,scratch.produced,reservation.extraBytes) ||
           std::memcmp(scratch.undo,scratch.produced,reservation.extraBytes))return false;
        return live() && capture(argument,read,current) && unchanged(before,current);
    };
    result.reason="tail-write";
    bool ok=write(before.cursor,scratch.produced,reservation.extraBytes);
    if(ok) {
        // Verify the zeroed tail before publishing a larger native used span.
        ok=read(before.cursor,scratch.produced,reservation.extraBytes);
        for(size_t i=0;ok && i<reservation.extraBytes/4;++i)ok=scratch.produced[i]==0;
    }
    if(ok)ok=live() && capture(argument,read,again) && unchanged(before,again);
    if(ok) {
        result.reason="cursor-write";
        ok=write(argument+0x20,&reservation.cursor,8);
    }
    Builder advanced=before;advanced.cursor=reservation.cursor;
    if(ok)ok=live() && capture(argument,read,again) && unchanged(advanced,again);
    if(!ok){result.mustStop=!restore();return result;}
    result.applied=true;result.extraBytes=reservation.extraBytes;
    result.usedBytes=reservation.usedBytes;result.reason="reserved";return result;
}
}
