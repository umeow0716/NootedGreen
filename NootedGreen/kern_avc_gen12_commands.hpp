#pragma once
#include <stddef.h>
#include <stdint.h>

// Shared kernel/CPU producer; native post-relocation submission is separately gated.
// Callers must independently prove writable extent, task/physical ownership,
// generation, relocation completion and native descriptor/count publication.
// Intel generated Gen12 definitions: media-driver 2a32c7fafd8fa169112e962587777969cc8c654f.
// This profile preserves the observed AVC/NV12 noncompressed feature flags;
// it is not a general HEVC/AV1/compressed-reference translator.
namespace NgreenAvcGen12 {
struct Result { bool ok=false; size_t words=0; const char* reason="input"; };
// Arithmetic only: callers must prove the native builder lock, current buffer
// identity, writable tail and kernel feature handshake before applying this.
// Advancing the producer cursor reserves bytes through native SetUsedBytes;
// sideband offsets and the original command count must remain unchanged.
struct Reservation {
    bool ok=false;
    uintptr_t cursor=0;
    uint32_t usedBytes=0;
    size_t extraBytes=0;
};
inline Reservation reserve(uintptr_t base,uintptr_t cursor,uintptr_t end,
                           uint32_t previousUsedBytes,size_t inputWords,
                           size_t outputWords) noexcept {
    Reservation r;
    if(!base || ((base|cursor|end|previousUsedBytes)&3U) ||
       cursor<base || end<cursor || inputWords<2 || inputWords>1024 ||
       outputWords<=inputWords || outputWords>1536 ||
       cursor-base!=inputWords*4U)return r;
    const size_t extra=(outputWords-inputWords)*4U;
    if(extra>end-cursor || cursor-base>UINT32_MAX-previousUsedBytes)return r;
    const uint32_t used=previousUsedBytes+static_cast<uint32_t>(cursor-base);
    if(extra>UINT32_MAX-used)return r;
    r.ok=true;r.cursor=cursor+extra;r.usedBytes=used+static_cast<uint32_t>(extra);
    r.extraBytes=extra;return r;
}
inline uint32_t opcode(uint32_t header) noexcept {
    return header<0x60000000U ? header&0xff800000U : header&0xffff0000U;
}
inline size_t packetWords(uint32_t header) noexcept {
    const uint32_t op=opcode(header);
    if(!header)return 1;
    // All supported pinned MFX/VDENC media headers reserve bits12..15.
    // opcode() and the length mask otherwise silently discard these bits.
    // MI packets below the media range have their own field layouts.
    if(header>=0x60000000U && (header&0xf000U))return 0;
    // Gen12 SRM reserves bits8..16,18,20..21. Keep functional bits
    // MMIO-remap17, CS-offset19 and global-GTT22 distinct from this mask.
    if(op==0x12000000U && (header&0x0035ff00U))return 0;
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
inline bool legacyNv12Surface(const uint32_t* packet,uint32_t width,uint32_t height) noexcept {
    // Gen9 bits28..31=format4, bit27=interleave. Gen12 bits27..31=format4;
    // blind copying 0x48000000 would select Gen12 RGBA_10_10_10_2 (format9).
    return (packet[2]&15U)==0 && ((packet[2]>>4)&0x3fffU)+1==width &&
        (packet[2]>>18)+1==height && (packet[3]&0xfff00000U)==0x48000000U &&
        // Accepted PLANAR_420_8/NV12 has one byte per luma pixel. Gen12
        // SurfacePitch bits3..19 encode byte stride minus one for all three
        // source/reference/downscaled NV12 surfaces; it must cover a row.
        ((packet[3]>>3)&0x1ffffU)+1>=width;
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
    size_t needed=0;unsigned mfx=0,mfxPipe=0,mfxImage=0,image=0,mode=0;
    unsigned srcSurface=0,refSurface=0,ds=0,pipe=0,walkers=0,srm=0;
    uint32_t nextRow=0,registerMask=0;
    // Entire validation pass precedes the first output write.
    for(size_t at=0;at<count;) {
        const uint32_t op=opcode(input[at]);const size_t n=packetWords(input[at]);
        if(!n || n>count-at){r.reason="packet";return r;}
        if(op==0x70480000U){
            const uint32_t control=input[at+1],lastBits=(control>>8)&0x3fU;
            const uint32_t skipBytes=(control>>16)&3U;
            if(n<3 || (input[at]&0xf000U) || (control&0xfffc0000U) ||
               !lastBits || lastBits>32 || (n-3)*32+lastBits<=skipBytes*8U){
                r.reason="pak-insert-profile";return r;}
        }
        // Authenticate prerequisites, not just end-of-frame packet counts.
        // Gen12 AVC picture state programs mode before its dependent state;
        // walkers consume both MFX/VDENC image, buffer and surface state.
        if((op==0x70020000U || op==0x71000000U) && mfx!=1){
            r.reason="mfx-state-order";return r;}
        if(op>=0x70810000U && op<=0x70870000U && mode!=1){
            r.reason="vdenc-state-order";return r;}
        if(op==0x70870000U &&
           (mfxPipe!=1 || mfxImage!=1 || image!=1 || pipe!=1 ||
            srcSurface!=1 || refSurface!=1 || ds!=1)){
            r.reason="walker-state-order";return r;}
        if(op==0x12000000U && !walkers){r.reason="status-order";return r;}
        size_t emitted=n;
        if(op==0x70000000U){++mfx;emitted+=2;
            if(input[at+2]&0x20U || input[at+4]){r.reason="mfx-mode-options";return r;}}
        if(op==0x70020000U){++mfxPipe;
            // Legacy compression-mode bits are not Gen12 compression types.
            static constexpr size_t controls[]={3,6,9,12,15,18,54,57,60,64,67};
            for(size_t k:controls)
                if(input[at+k]&0x600U){r.reason="mfx-compression";return r;}
            if(input[at+61]){r.reason="reference-compression";return r;}}
        if(op==0x71000000U){++mfxImage;
            // Gen12 MFX_AVC_IMG_STATE DW3 reserved fields only; preserve
            // weighted prediction, rho-control and both chroma QP offsets.
            if(input[at+3]&0xe0e0c0ffU){r.reason="mfx-image-reserved";return r;}
            const uint32_t mbWidth=(width+15U)/16U,mbHeight=(height+15U)/16U;
            const uint32_t mbCount=mbWidth*mbHeight;
            if(input[at+1]!=(mbCount>0xffffU?0xffffU:mbCount) ||
               input[at+2]!=((mbHeight-1U)<<16|(mbWidth-1U))){
                r.reason="mfx-image-geometry";return r;}
            if(input[at+17]&0x3fU){r.reason="fractional-qp";return r;}}
        if(op==0x71030000U){
            const uint32_t mbWidth=(width+15U)/16U,mbHeight=(height+15U)/16U;
            const uint32_t start=input[at+4],next=input[at+5];
            const uint32_t x=(start>>16)&0xffU,y=start>>24;
            const uint32_t nx=next&0x1ffU,ny=(next>>16)&0x1ffU;
            if((start&0x8000U) || (next&0xfe00fe00U) || x>=mbWidth ||
               y>=mbHeight || nx>=mbWidth || ny>mbHeight ||
               (ny==mbHeight && nx) || ny*mbWidth+nx<=y*mbWidth+x ||
               (start&0x7fffU)!=((y*mbWidth+x)&0x7fffU)){
                r.reason="mfx-slice-geometry";return r;}
        }
        if(op==0x70850000U){++image;
            if(input[at+1]&0x20U || input[at+24] || input[at+34]&0x4000U){
                r.reason="image-options";return r;}}
        if(op==0x70800000U){++mode;emitted=6;
            if(input[at+1]!=0x82U){r.reason="mode-profile";return r;}}
        if(op==0x70810000U || op==0x70820000U || op==0x70830000U) {
            const bool downscaled=op==0x70830000U;
            const uint32_t surfaceWidth=downscaled?(width+3)/4:width;
            const uint32_t surfaceHeight=downscaled?((height+15)&~15U)/4:((height+15)&~15U);
            if(!legacyNv12Surface(input+at,surfaceWidth,surfaceHeight)){
                r.reason="surface-profile";return r;}
            if(op==0x70810000U)++srcSurface;
            if(op==0x70820000U)++refSurface;
            if(downscaled){++ds;emitted=10;}
        }
        if(op==0x70840000U){++pipe;emitted=71;
            if(input[at+7] || input[at+8]){r.reason="legacy-bwd-reference";return r;}
            for(size_t k=3;k<37;k+=3){
                if(input[at+k]&(1U<<9)){
                    r.reason="compressed-resource";return r;}
                // Gen12 VDENC address triplets: 64B-aligned lower address,
                // 16-bit upper address, control reserved bit11/bits15..31.
                if((input[at+k-2]&0x3fU) || (input[at+k-1]&0xffff0000U) ||
                   (input[at+k]&0xffff8800U)){
                    r.reason="resource-reserved-bits";return r;}
            }}
        if(op==0x70870000U){++walkers;emitted=27;
            if(input[at+1]!=nextRow || input[at+2]<=nextRow ||
               input[at+2]>(height+15)/16 || input[at+3]!=0){r.reason="walker-profile";return r;}
            nextRow=input[at+2];}
        if(op==0x12000000U){++srm;
            if(!relativeMfc(input[at+1]) || input[at]&(1U<<19) || (input[at+2]&3U)){
                r.reason="srm-profile";return r;}
            const uint32_t bit=input[at+1]==0x128a0?1U:input[at+1]==0x128a4?2U:
                               input[at+1]==0x128b4?4U:8U;
            if(registerMask&bit){r.reason="duplicate-srm";return r;}registerMask|=bit;}
        needed+=emitted;at+=n;
    }
    if(mfx!=1 || mfxPipe!=1 || mfxImage!=1 || image!=1 || mode!=1 ||
       srcSurface!=1 || refSurface!=1 || ds!=1 || pipe!=1 ||
       !walkers || walkers>32 || srm!=4 || registerMask!=15 ||
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
        // Extended PAK is DW1 bit6 in BOTH pinned Gen9 and Gen12 layouts.
        // Gen12 bit5 is UltraMode, not Extended PAK; never enable it here.
        if(op==0x70850000U)output[start+1]|=0x40U;
        if(op==0x70810000U || op==0x70820000U || op==0x70830000U)
            output[start+3]=(input[at+3]&0x000fffffU)|(4U<<27);
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

// Must be preallocated OFF the kernel stack, before the native locked scope.
// Neither a valid byte shape nor these callbacks establish kernel ownership;
// the eventual routed caller must hold independently verified native leases.
struct PublishScratch {
    uint32_t source[1024],produced[1536],undo[1536];
};
struct Publication {
    Result preparation{};
    bool published=false,restored=true,mustStop=false;
};
// A targeted candidate with !published MUST NOT be forwarded to native submit,
// even when restored==true. mustStop additionally marks a post-write failure.
// CPU release fences do not establish device coherency: the kernel integration
// still needs its native IO synchronization before invoking the native submit.
template<class Live,class SamePinnedAllocation>
inline Publication publish(volatile uint32_t* backing,size_t available,size_t reservedWords,
                           volatile uint32_t& nativeWords,uint32_t width,uint32_t height,
                           PublishScratch& scratch,Live live,SamePinnedAllocation canRestore) noexcept {
    Publication r;
    const uint32_t original=nativeWords;
    const uintptr_t start=reinterpret_cast<uintptr_t>(backing);
    const uintptr_t work=reinterpret_cast<uintptr_t>(&scratch);
    const uintptr_t counter=reinterpret_cast<uintptr_t>(&nativeWords);
    // reservedWords is the native-accounted EXCLUSIVE used span, not free
    // allocation capacity. The integration must obtain it from the accepted
    // data-buffer reservation protocol before any expanding replacement.
    if(!backing || (start&3U) || original>1024 || original<2 ||
       reservedWords<original || reservedWords>available ||
       available>SIZE_MAX/4 || start>UINTPTR_MAX-available*4 ||
       work>UINTPTR_MAX-sizeof(scratch) || counter>UINTPTR_MAX-4 ||
       (work<start+available*4 && start<work+sizeof(scratch)) ||
       (counter<start+available*4 && start<counter+4) ||
       (counter<work+sizeof(scratch) && work<counter+4) || !live(original))return r;
    for(size_t i=0;i<original;++i)scratch.source[i]=backing[i];
    r.preparation=produce(scratch.source,original,width,height,scratch.produced,
                          reservedWords<1536?reservedWords:1536);
    if(!r.preparation.ok || nativeWords!=original || !live(original))return r;
    const size_t changed=r.preparation.words;
    // Save the WHOLE overwritten extent, including bytes beyond old count.
    for(size_t i=0;i<changed;++i)scratch.undo[i]=backing[i];
    for(size_t i=0;i<original;++i)if(scratch.undo[i]!=scratch.source[i]){
        r.preparation.reason="source-changed";return r;}
    if(nativeWords!=original || !live(original))return r;
    auto restore=[&]() noexcept {
        r.mustStop=true;r.restored=false;
        // Do not write through an alias whose allocation pin was lost.
        if(!canRestore())return;
        for(size_t i=0;i<changed;++i)backing[i]=scratch.undo[i];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        nativeWords=original;
        bool equal=nativeWords==original;
        for(size_t i=0;i<changed;++i)if(backing[i]!=scratch.undo[i])equal=false;
        r.restored=equal;
    };
    r.restored=false;
    for(size_t i=0;i<changed;++i)backing[i]=scratch.produced[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    bool equal=true;
    for(size_t i=0;i<changed;++i)if(backing[i]!=scratch.produced[i])equal=false;
    if(!equal || nativeWords!=original || !live(original)){restore();return r;}
    // Count is published only after all command bytes and validation complete.
    nativeWords=static_cast<uint32_t>(changed);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if(nativeWords!=changed || !live(static_cast<uint32_t>(changed))){restore();return r;}
    r.published=true;return r;
}
}
