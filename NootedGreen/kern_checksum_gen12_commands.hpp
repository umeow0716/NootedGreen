#pragma once
#include <stddef.h>
#include <stdint.h>
#include "kern_avc_gen12_commands.hpp"

// Shared pure CPU producer. It grants no writable mapping, native IOLock,
// task/physical epoch, relocation completion, heap lifetime or submission.
// Native KMD overwrites the SBA addresses, so kernel use is POST relocation.
// Exact captured 1080p FrameChecksum shape only; other auxiliary kernels and
// walker geometries are not silently accepted.
namespace NgreenChecksumGen12 {
struct HeapAddresses {
    uint64_t surface=0,dynamic=0,instruction=0;
    uint32_t kernelSlot=0;
};
struct Result {bool ok=false;const char* reason="shape";};
inline Result build1080Batch(const uint32_t* source,size_t words,const HeapAddresses& heap,
    volatile uint32_t* output,size_t capacity) noexcept {
    if(!source || !output || words!=72 || capacity<78)return {false,"extent"};
    const uintptr_t inputAddress=reinterpret_cast<uintptr_t>(source);
    const uintptr_t outputAddress=reinterpret_cast<uintptr_t>(output);
    if((inputAddress&3U) || (outputAddress&3U) || inputAddress>UINTPTR_MAX-288 ||
        outputAddress>UINTPTR_MAX-312 ||
        !(inputAddress+288<=outputAddress || outputAddress+312<=inputAddress))return {false,"alias"};
    // Exact 1080p native FrameChecksum schema. Other dispatch shapes are not
    // silently generalized from a single observed media-walker grid.
    static constexpr uint32_t shape[72]={
        0x11000001,0,0x80000040,0x7a000004,0x001010a0,0,0,0,0,0x69041301,
        0x6101000e,1,0,0,1,0,1,0,1,0,1,0,1,0x1001,1,0x1001,
        0x70000007,0,0,0x01bf1000,0,0x007c0002,0,0,0,
        0x70010002,0,0x40,0x400,0x70020002,0,0x40,0,
        0x7103000f,0,0,0,0,0,0,0x07ff07ff,0x00020044,0,0,
        0x00010000,1,0x00020044,0,0x44,0x00020000,
        0x7a000004,0x001010a0,0,0,0,0,0x7a000004,0x0000089c,0,0,0,0
    };
    const uint64_t addresses[]={heap.surface,heap.dynamic,heap.instruction};
    if(heap.kernelSlot!=1)return {false,"kernel-binding"};
    for(unsigned i=0;i<3;++i) {
        if(!addresses[i] || addresses[i]>UINT64_C(0x0000fffffffff000) ||
           (addresses[i]&4095))return {false,"heap-address"};
        for(unsigned j=0;j<i;++j)if(addresses[i]==addresses[j])return {false,"heap-alias"};
    }
    for(size_t i=0;i<size_t(72);++i) {
        uint32_t expected=shape[i];
        if(i==14)expected=uint32_t(heap.surface)|1U;
        if(i==15)expected=uint32_t(heap.surface>>32);
        if(i==16)expected=uint32_t(heap.dynamic)|1U;
        if(i==17)expected=uint32_t(heap.dynamic>>32);
        if(i==20)expected=uint32_t(heap.instruction)|1U;
        if(i==21)expected=uint32_t(heap.instruction>>32);
        if(source[i]!=expected)return {false,"native-batch-shape"};
    }
    uint32_t candidate[78];
    for(size_t i=0;i<26;++i)candidate[i]=source[i];
    for(size_t i=26;i<32;++i)candidate[i]=0;
    for(size_t i=26;i<size_t(72);++i)candidate[i+6]=source[i];
    // 1c1b emits an unbound register-zero LRI. Gen12's qualified media
    // PIPELINE_SELECT follows the PIPE_CONTROL; no legacy LRI is retained.
    candidate[0]=candidate[1]=candidate[2]=0; // MI_NOOP, preserves SBA word offset 10.
    candidate[10]=0x61010014; // Gen12 22-DW SBA; bindless fields remain disabled.
    uint32_t cache=0;
    if(!NgreenAvcGen12::linuxGen12CacheControl(0,cache))return {false,"cache-profile"};
    candidate[11]|=(cache&0x7fU)<<4;
    for(unsigned i=14;i<=20;i+=2)candidate[i]|=(cache&0x7fU)<<4;
    // Intel Gen12 constructor's two single-GPU restrictions, plus translated
    // stateless MOCS. Source addresses/lengths and all walker/curbe data survive.
    candidate[13]=0xc000U|((cache&0x7fU)<<16);
    // Gen9 reserved bit 6 becomes Gen12 FusedEuDispatch. Intel's Gen12
    // legacy/non-fused mode is ONE, not the source packet's reserved zero.
    candidate[35]|=0x40U;
    // Native2c566 appends code and descriptors, rather than replacing code
    // that a GPU could already have fetched. Its descriptor stride is 64B.
    // Load the appended slot into hardware ID0; the walker stays at ID0.
    candidate[48]=0x40U;
    for(size_t i=0;i<78;++i)output[i]=candidate[i];
    return {true,"produced"};
}
}
