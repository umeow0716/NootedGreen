#pragma once
#include <array>
#include <cstdint>
#include "../../NootedGreen/kern_avc_gen12_commands.hpp"
// Generated from frame_checksum_gen12.asm with IGA c36a504, Gen12 + auto-deps.
// Runtime loader/command/heap admission is deliberately separate. These bytes
// are not a native Apple function or permission to submit a GPU command.
namespace NgreenFrameChecksumGen12 {
inline constexpr unsigned byteSize=1576;
inline constexpr char sha256[]="94e83be475bcf49225f49933fad3f76bc6f541e6fde1c17a51da61fba6beaeea";
alignas(64) inline constexpr std::array<uint32_t,394> isa{{
    0x80000068U,0x01868210U,0x01000024U,0x000c000cU,0x80000061U,0x01854330U,0x00040004U,0x00000000U,
    0x00400165U,0x00018110U,0x11000184U,0x0fff0fffU,0x80000065U,0x01a68210U,0x01000024U,0x0fff0fffU,
    0x80000061U,0x05454330U,0x00010001U,0x00000000U,0x01400062U,0x01c58560U,0x050001b4U,0x00030003U,
    0x11400262U,0x02258520U,0x05000544U,0x00000000U,0x00000269U,0x01e50160U,0x060001a4U,0x000001c4U,
    0x00000268U,0x02250220U,0x02000164U,0x00000224U,0x00000240U,0x02058660U,0x050001e4U,0x00070007U,
    0x00000170U,0x00010220U,0x52000204U,0x00000224U,0x91000120U,0x00004000U,0x00000000U,0x00000560U,
    0x00000168U,0x02458220U,0x11000144U,0x00030003U,0x00000061U,0x02454770U,0x00000000U,0x00000000U,
    0x00000061U,0x02854770U,0x00000000U,0x00000000U,0x00010061U,0x02854770U,0x00000000U,0x00000000U,
    0x81000020U,0x00004000U,0x00000000U,0x00000390U,0x81400020U,0x00004000U,0x00000000U,0x000000b0U,
    0x640b0061U,0x00100000U,0x80000040U,0x10018220U,0x02000104U,0x02190001U,0x80000061U,0x0b454770U,
    0x0000001fU,0x00000000U,0x620b0761U,0x00110278U,0x80000061U,0x0b050770U,0x00000204U,0x00000000U,
    0x80039031U,0x03050000U,0xc0000b04U,0x00000000U,0x00002040U,0x0b058660U,0x05000264U,0x00200020U,
    0x80039031U,0x04050000U,0xc0000b04U,0x00000000U,0x80000001U,0x00010000U,0x20000000U,0x00000000U,
    0x80000001U,0x00010000U,0x30000000U,0x00000000U,0x80000020U,0x00004000U,0x00000000U,0x000000a0U,
    0x640c0061U,0x00100000U,0x80000040U,0x10018220U,0x02000104U,0x02190000U,0x80000061U,0x0c454770U,
    0x0000001fU,0x00000000U,0x620c0061U,0x00110278U,0x80000061U,0x0c050770U,0x00000204U,0x00000000U,
    0x80039031U,0x03050000U,0xc0000c04U,0x00000000U,0x00002040U,0x0c058660U,0x05000264U,0x00200020U,
    0x80039031U,0x04050000U,0xc0000c04U,0x00000000U,0x80000001U,0x00010000U,0x20000000U,0x00000000U,
    0x80000001U,0x00010000U,0x30000000U,0x00000000U,0x00020061U,0x05850450U,0x002002a4U,0x00000000U,
    0x00020061U,0x0d050450U,0x00200344U,0x00000000U,0x00020267U,0x02a60440U,0x05200304U,0x00340585U,
    0x00020061U,0x0d450450U,0x0020034cU,0x00000000U,0x00020061U,0x05c50450U,0x002002acU,0x00000000U,
    0x00020061U,0x0d850450U,0x00200384U,0x00000000U,0x00020267U,0x02ae0440U,0x0520030cU,0x003405c5U,
    0x00020061U,0x0dc50450U,0x0020038cU,0x00000000U,0x00020667U,0x02a60440U,0x052002a4U,0x00340d05U,
    0x00020061U,0x0e050450U,0x002003c4U,0x00000000U,0x00020467U,0x02ae0440U,0x052002acU,0x00340d45U,
    0x00020061U,0x0e450450U,0x002003ccU,0x00000000U,0x00020467U,0x02a60440U,0x052002a4U,0x00340d85U,
    0x00020061U,0x0e850450U,0x00200404U,0x00000000U,0x00020467U,0x02ae0440U,0x052002acU,0x00340dc5U,
    0x00020061U,0x0ec50450U,0x0020040cU,0x00000000U,0x00020467U,0x02a60440U,0x052002a4U,0x00340e05U,
    0x00020061U,0x0f050450U,0x00200444U,0x00000000U,0x00020467U,0x02ae0440U,0x052002acU,0x00340e45U,
    0x00020061U,0x0f450450U,0x0020044cU,0x00000000U,0x00020467U,0x02a60440U,0x052002a4U,0x00340e85U,
    0x00020061U,0x0f850450U,0x00200484U,0x00000000U,0x00020467U,0x02ae0440U,0x052002acU,0x00340ec5U,
    0x00020061U,0x0fc50450U,0x0020048cU,0x00000000U,0x00020467U,0x02a60440U,0x052002a4U,0x00340f05U,
    0x00020061U,0x10050450U,0x002004c4U,0x00000000U,0x00020467U,0x02ae0440U,0x052002acU,0x00340f45U,
    0x00020061U,0x10450450U,0x002004ccU,0x00000000U,0x00020467U,0x02a60440U,0x052002a4U,0x00340f85U,
    0x00000040U,0x02858660U,0x05000284U,0x00010001U,0x00020467U,0x02ae0440U,0x052002acU,0x00340fc5U,
    0x00000170U,0x00010660U,0x16000284U,0x00000244U,0x00020767U,0x02a60440U,0x052002a4U,0x00341005U,
    0x00000040U,0x02658660U,0x05000264U,0x00400040U,0x00020767U,0x02ae0440U,0x052002acU,0x00341045U,
    0x91000420U,0x00004000U,0x00000000U,0xfffffc90U,0x00000267U,0x02e50660U,0x060002c4U,0x000002a4U,
    0x00000167U,0x05050550U,0x050002f4U,0x000002e4U,0x80000161U,0x05550450U,0x00000504U,0x00000000U,
    0x00000167U,0x01960440U,0x0500050cU,0x00000554U,0x81400020U,0x00004000U,0x00000000U,0x00000040U,
    0x00030061U,0x06054770U,0x00000000U,0x00000000U,0x00000161U,0x060e0000U,0x00000194U,0x00000000U,
    0x80000020U,0x00004000U,0x00000000U,0x00000030U,0x00030061U,0x06054770U,0x00000000U,0x00000000U,
    0x00000161U,0x06060000U,0x00000194U,0x00000000U,0x80000061U,0x05454330U,0x00080008U,0x00000000U,
    0x00030061U,0x10854850U,0x00000000U,0x76543210U,0x01400262U,0x05258560U,0x05000564U,0x00100010U,
    0x80000040U,0x10018220U,0x02000124U,0x02009300U,0x00030341U,0x07058560U,0x05461085U,0x00040004U,
    0x00030340U,0x09050660U,0x05000524U,0x00461085U,0x00039031U,0x00010000U,0xc0000704U,0x0000060cU,
    0x00033061U,0x08054770U,0x00000000U,0x00000000U,0x00030169U,0x0a058660U,0x05460905U,0x00020002U,
    0x80000040U,0x10018220U,0x02000124U,0x02009300U,0x00010061U,0x08050770U,0x00220285U,0x00000000U,
    0x00039131U,0x00010000U,0xc0000a04U,0x0000080cU,0x80000001U,0x00010000U,0x20000000U,0x00000000U,
    0x80000001U,0x00010000U,0x30000000U,0x00000000U,0x647f0061U,0x00100000U,0x80039031U,0x00000004U,
    0x70207f0cU,0x00000000U,
}};
static_assert(sizeof(isa)==byteSize,"complete Gen12 checksum kernel");

// Pure CPU producer, NOT a writable-span/owner/relocation authorization.
// The runtime must separately hold the native checksum builder/heap leases,
// qualify the PF cache ABI, reserve 78 words before the native builder runs,
// and normalize SBA cache bits AGAIN after native KMD address relocation.
struct HeapAddresses {
    uint64_t surface=0,dynamic=0,instruction=0;
    uint32_t kernelSlot=0;
};
struct BatchResult {bool ok=false;const char* reason="shape";};
inline BatchResult build1080Batch(const std::array<uint32_t,72>& source,
                                 const HeapAddresses& heap,
                                 std::array<uint32_t,78>& output) noexcept {
    // Exact 1080p native FrameChecksum schema. Other dispatch shapes are not
    // silently generalized from a single observed media-walker grid.
    constexpr std::array<uint32_t,72> shape{{
        0x11000001,0,0x80000040,0x7a000004,0x001010a0,0,0,0,0,0x69041301,
        0x6101000e,1,0,0,1,0,1,0,1,0,1,0,1,0x1001,1,0x1001,
        0x70000007,0,0,0x01bf1000,0,0x007c0002,0,0,0,
        0x70010002,0,0x40,0x400,0x70020002,0,0x40,0,
        0x7103000f,0,0,0,0,0,0,0x07ff07ff,0x00020044,0,0,
        0x00010000,1,0x00020044,0,0x44,0x00020000,
        0x7a000004,0x001010a0,0,0,0,0,0x7a000004,0x0000089c,0,0,0,0
    }};
    const uint64_t addresses[]={heap.surface,heap.dynamic,heap.instruction};
    if(heap.kernelSlot!=1)return {false,"kernel-binding"};
    for(unsigned i=0;i<3;++i) {
        if(!addresses[i] || addresses[i]>UINT64_C(0x0000fffffffff000) ||
           (addresses[i]&4095))return {false,"heap-address"};
        for(unsigned j=0;j<i;++j)if(addresses[i]==addresses[j])return {false,"heap-alias"};
    }
    for(size_t i=0;i<source.size();++i) {
        uint32_t expected=shape[i];
        if(i==14)expected=uint32_t(heap.surface)|1U;
        if(i==15)expected=uint32_t(heap.surface>>32);
        if(i==16)expected=uint32_t(heap.dynamic)|1U;
        if(i==17)expected=uint32_t(heap.dynamic>>32);
        if(i==20)expected=uint32_t(heap.instruction)|1U;
        if(i==21)expected=uint32_t(heap.instruction>>32);
        if(source[i]!=expected)return {false,"native-batch-shape"};
    }
    std::array<uint32_t,78> candidate{};
    for(size_t i=0;i<26;++i)candidate[i]=source[i];
    for(size_t i=26;i<source.size();++i)candidate[i+6]=source[i];
    // 1c1b emits an unbound register-zero LRI. Gen12's qualified media
    // PIPELINE_SELECT follows the PIPE_CONTROL; no legacy LRI is retained.
    candidate[0]=candidate[1]=candidate[2]=0; // MI_NOOP, preserves SBA word offset 10.
    candidate[10]=0x61010014; // Gen12 22-DW SBA; bindless fields remain disabled.
    uint32_t cache=0;
    if(!NgreenAvcGen12::linuxGen12CacheControl(0,cache))return {false,"cache-profile"};
    for(unsigned i:{11U,14U,16U,18U,20U})candidate[i]|=(cache&0x7fU)<<4;
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
    output=candidate;return {true,"produced"};
}
}
