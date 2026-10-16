#pragma once
#include <cstddef>
#include <cstdint>

// Candidate CPU producer, deliberately NOT wired to adapter/kernel submission.
// Callers must independently prove writable extent, task/physical ownership,
// generation, relocation completion and native descriptor/count publication.
// Intel generated Gen12 definitions: media-driver 2a32c7fafd8fa169112e962587777969cc8c654f.
// This profile preserves the observed AVC/NV12 noncompressed feature flags;
// it is not a general HEVC/AV1/compressed-reference translator.
namespace NgreenAvcGen12 {
struct Result { bool ok=false; size_t words=0; const char* reason="input"; };
inline uint32_t opcode(uint32_t header) noexcept {
    return header<0x60000000U ? header&0xff800000U : header&0xffff0000U;
}
inline size_t packetWords(uint32_t header) noexcept {
    const uint32_t op=opcode(header);
    if(!header)return 1;
    if(op==0x70480000U)return (header&0xfffU)+2;
    size_t n=0;
    switch(op) {
    case 0x13000000: case 0x10000000: case 0x70000000:n=5;break;
    case 0x12000000:n=4;break;
    case 0x70010000: case 0x70810000: case 0x70820000: case 0x70830000:n=6;break;
    case 0x70020000:n=68;break;
    case 0x70030000:n=26;break;
    case 0x70040000:n=10;break;
    case 0x70070000:n=18;break;
    case 0x70080000:n=34;break;
    case 0x71000000:n=21;break;
    case 0x71030000:n=11;break;
    case 0x70800000: case 0x77800000:n=2;break;
    case 0x70840000:n=37;break;
    case 0x70850000:n=35;break;
    case 0x70860000:n=61;break;
    case 0x70870000:n=4;break;
    default:return 0;
    }
    const uint32_t mask=op==0x13000000U?0x3fU:0xfffU;
    return (header&mask)+2==n?n:0;
}
inline uint32_t relativeMfc(uint32_t legacy) noexcept {
    switch(legacy){case 0x128a0:return 0x8a0;case 0x128a4:return 0x8a4;
    case 0x128b4:return 0x8b4;case 0x128b8:return 0x8b8;default:return 0;}
}
inline Result produce(const uint32_t* input,size_t count,uint32_t width,uint32_t height,
                      uint32_t* output,size_t capacity) noexcept {
    Result r;
    if(!input || !output || count<2 || count>1024 || !width || width>4096 ||
       !height || height>4096 || (width&1U) || (height&1U))return r;
    const uintptr_t src=reinterpret_cast<uintptr_t>(input),dst=reinterpret_cast<uintptr_t>(output);
    if((src&3U) || (dst&3U) || capacity>1536 || src>UINTPTR_MAX-count*4 || dst>UINTPTR_MAX-capacity*4 ||
       (src<dst+capacity*4 && dst<src+count*4)){r.reason="alias";return r;}
    if(input[count-1] || input[count-2]){r.reason="native-end-reserve";return r;}
    size_t needed=0;unsigned mfx=0,mode=0,ds=0,pipe=0,walkers=0,srm=0;
    uint32_t nextRow=0,registerMask=0;
    // Entire validation pass precedes the first output write.
    for(size_t at=0;at<count;) {
        const uint32_t op=opcode(input[at]);const size_t n=packetWords(input[at]);
        if(!n || n>count-at){r.reason="packet";return r;}
        size_t emitted=n;
        if(op==0x70000000U){++mfx;emitted+=2;}
        if(op==0x70800000U){++mode;emitted=6;
            if(input[at+1]!=0x82U){r.reason="mode-profile";return r;}}
        if(op==0x70830000U){++ds;emitted=10;}
        if(op==0x70840000U){++pipe;emitted=71;
            if(input[at+7] || input[at+8]){r.reason="legacy-bwd-reference";return r;}
            for(size_t k=3;k<37;k+=3)if(input[at+k]&(1U<<9)){
                r.reason="compressed-resource";return r;}}
        if(op==0x70870000U){++walkers;emitted=27;
            if(input[at+1]!=nextRow || input[at+2]<=nextRow ||
               input[at+2]>(height+15)/16 || input[at+3]!=0){r.reason="walker-profile";return r;}
            nextRow=input[at+2];}
        if(op==0x12000000U){++srm;
            if(!relativeMfc(input[at+1]) || input[at]&(1U<<19)){
                r.reason="srm-profile";return r;}
            const uint32_t bit=input[at+1]==0x128a0?1U:input[at+1]==0x128a4?2U:
                               input[at+1]==0x128b4?4U:8U;
            if(registerMask&bit){r.reason="duplicate-srm";return r;}registerMask|=bit;}
        needed+=emitted;at+=n;
    }
    if(mfx!=1 || mode!=1 || ds!=1 || pipe!=1 || !walkers || walkers>32 || srm!=4 || registerMask!=15 ||
       nextRow!=(height+15)/16){r.reason="frame-profile";return r;}
    if(needed>capacity){r.reason="capacity";return r;}
    size_t out=0;
    for(size_t at=0;at<count;) {
        const uint32_t op=opcode(input[at]);const size_t n=packetWords(input[at]);
        if(op==0x70000000U)output[out++]=0x68000100U;
        size_t emitted=n;
        if(op==0x70800000U)emitted=6;
        if(op==0x70830000U)emitted=10;
        if(op==0x70840000U)emitted=71;
        if(op==0x70870000U)emitted=27;
        const size_t start=out;
        for(size_t k=0;k<emitted;++k)output[out++]=k<n?input[at+k]:0;
        if(emitted!=n)output[start]=(input[at]&~0xfffU)|static_cast<uint32_t>(emitted-2);
        if(op==0x70800000U){output[start+2]=0x002b030aU;output[start+3]=0x07000303U;}
        if(op==0x70830000U)output[start+7]=3;
        if(op==0x70840000U)output[start+7]=output[start+8]=output[start+9]=0;
        if(op==0x70870000U){output[start+5]=width-1;output[start+14]=0x3f400000U;
            output[start+16]=0x003f3f3fU;}
        if(op==0x12000000U){output[start]|=1U<<19;output[start+1]=relativeMfc(input[at+1]);}
        if(op==0x70000000U)output[out++]=0x68000100U;
        at+=n;
    }
    r.ok=true;r.words=out;r.reason="cpu-produced-not-admitted";return r;
}
}
