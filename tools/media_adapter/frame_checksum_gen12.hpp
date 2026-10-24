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
// Caller must hold the original context's exclusive owner/physical epoch
// from BEFORE these nested allocation reads through native return. Readable
// mappings alone are never a lifetime lease. Use only before publication and
// before any frame. A post-call failure is ambiguous and must be quarantined.
struct AppendResult {bool ok=false,mutated=false;const char* reason="owner";};
inline constexpr std::array<uint8_t,259> nativeLoaderBytes{{
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x41,0x89,
    0xd6,0x49,0x89,0xfd,0x8b,0x87,0x6c,0x01,0x00,0x00,0x8b,0xbf,0x90,0x01,0x00,0x00,
    0x42,0x8d,0x0c,0x30,0x39,0xcf,0x0f,0x82,0xd2,0x00,0x00,0x00,0x45,0x8b,0xbd,0x68,
    0x01,0x00,0x00,0x4d,0x8b,0xa5,0x98,0x01,0x00,0x00,0x49,0x01,0xc4,0x41,0x83,0xbd,
    0x74,0x01,0x00,0x00,0x00,0x74,0x74,0x4d,0x8d,0x85,0xa8,0x11,0x00,0x00,0x41,0xb9,
    0x01,0x00,0x00,0x00,0x4c,0x89,0xff,0x48,0xc1,0xe7,0x06,0x49,0x8b,0x18,0x41,0x8b,
    0x8d,0x60,0x01,0x00,0x00,0xc7,0x44,0x3b,0x04,0x00,0x00,0x00,0x00,0x0f,0xb7,0x54,
    0x3b,0x12,0xc1,0xe2,0x10,0x89,0x54,0x3b,0x10,0x83,0xe0,0xc0,0x8b,0x14,0x3b,0x83,
    0xe2,0x3f,0x09,0xc2,0x89,0x14,0x3b,0x83,0x64,0x3b,0x0c,0x03,0xc1,0xe1,0x0b,0x81,
    0xc1,0x00,0xf8,0x00,0x00,0x81,0xe1,0x00,0x00,0xff,0xff,0x89,0x4c,0x3b,0x14,0x41,
    0x8b,0x85,0x74,0x01,0x00,0x00,0x49,0x39,0xc1,0x73,0x10,0x41,0x8b,0x85,0x6c,0x01,
    0x00,0x00,0x49,0x83,0xc0,0x20,0x49,0xff,0xc1,0xeb,0xa0,0x44,0x89,0xf2,0x4c,0x89,
    0xe7,0xe8,0xf0,0x1d,0x21,0x00,0x4f,0x89,0xa4,0xfd,0x90,0x43,0x00,0x00,0x47,0x89,
    0xb4,0xbd,0x90,0x45,0x00,0x00,0x41,0x83,0xc6,0x3f,0x41,0x83,0xe6,0xc0,0x45,0x01,
    0xb5,0x6c,0x01,0x00,0x00,0x41,0xff,0x85,0x68,0x01,0x00,0x00,0x44,0x89,0xf8,0x48,
    0x83,0xc4,0x08,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3,0xe8,0xa4,
    0x19,0x21,0x00,
}};
template<class Authority,class Read,class Writable,class Executable,class Invoke>
inline AppendResult appendProgram(uintptr_t image,uintptr_t context,Authority authority,
    Read read,Writable writable,Executable executable,Invoke invoke) noexcept {
    if(!image || image>UINTPTR_MAX-0x76d5b8 || !context || context>UINTPTR_MAX-0x4748 ||
        !authority(image,context))return {};
    uintptr_t originalTable=0;
    if(!read(context,&originalTable,sizeof(originalTable)) || originalTable!=image+0x754408)
        return {false,false,"checksum-family"};
    std::array<uint8_t,259> body{};
    if(!executable(image+0x2c566,body.size()) ||
        !read(image+0x2c566,body.data(),body.size()) || body!=nativeLoaderBytes)
        return {false,false,"loader-identity"};
    std::array<uint32_t,18> head{};
    std::array<uint32_t,8> descriptorPool{};
    uint8_t debug=1;
    if(!read(context+0x160,head.data(),sizeof(head)) ||
        !read(context+0x1198,descriptorPool.data(),sizeof(descriptorPool)) ||
        !read(context+0x4690,&debug,1))return {false,false,"context-read"};
    // Native2c158/9710 initialized one private page, one existing kernel and
    // no frames. Check the rounded allocation requirement independently of
    // native2c566, whose capacity comparison only checks unrounded bytes.
    if(head[0]!=64 || head[1]!=1536 || head[2]!=1 || head[3]!=1536 || head[4]!=3 ||
        head[5]!=1 || head[6] || head[7] || debug || head[10] || head[11]!=1 ||
        head[12]!=4096 || head[13]!=1 || (!head[16] && !head[17]) ||
        descriptorPool[0] || descriptorPool[1]!=1 || descriptorPool[2]!=4096 ||
        descriptorPool[3]!=1 || (!descriptorPool[6] && !descriptorPool[7]))
        return {false,false,"fresh-context"};
    const uintptr_t heap=uint64_t(head[14])|(uint64_t(head[15])<<32);
    const uintptr_t descriptors=uint64_t(descriptorPool[4])|(uint64_t(descriptorPool[5])<<32);
    const auto disjoint=[](uintptr_t a,size_t an,uintptr_t b,size_t bn) noexcept {
        return a && b && a<=UINTPTR_MAX-an && b<=UINTPTR_MAX-bn && (a+an<=b || b+bn<=a);
    };
    if((heap&4095) || (descriptors&4095) ||
        !disjoint(heap,4096,descriptors,4096) || !disjoint(heap,4096,context,0x4748) ||
        !disjoint(descriptors,4096,context,0x4748) ||
        !writable(context,0x4748) || !writable(heap,4096) || !writable(descriptors,4096))
        return {false,false,"allocation-domain"};
    std::array<uint8_t,1536> original{},oldCode{};
    std::array<uint32_t,32> oldDescriptors{};
    std::array<uint64_t,2> oldPointers{};std::array<uint32_t,2> oldSizes{};
    std::array<uint32_t,32> expectedDescriptors{};expectedDescriptors[5]=0x20000;
    if(!read(image+0x76cff0,original.data(),1480) ||
        !read(heap,oldCode.data(),oldCode.size()) || oldCode!=original ||
        !read(descriptors,oldDescriptors.data(),sizeof(oldDescriptors)) || oldDescriptors!=expectedDescriptors ||
        !read(context+0x4390,oldPointers.data(),sizeof(oldPointers)) || oldPointers[0]!=heap || oldPointers[1] ||
        !read(context+0x4590,oldSizes.data(),sizeof(oldSizes)) || oldSizes[0]!=1480 || oldSizes[1])
        return {false,false,"original-program"};
    if(!authority(image,context))return {false,false,"owner-before-call"};
    // The qualified original native helper is invoked exactly once. It only
    // appends to native CPU allocations here; it does not submit GPU work.
    const uint64_t returned=invoke(image+0x2c566,context,isa.data(),byteSize);
    std::array<uint32_t,18> after{};std::array<uint32_t,8> poolAfter{};
    std::array<uint64_t,2> pointers{};std::array<uint32_t,2> sizes{};
    std::array<uint8_t,1576> appended{};
    head[2]=2;head[3]=3136;expectedDescriptors[16]=1536;expectedDescriptors[21]=0x20000;
    uintptr_t finalTable=0;
    if(returned!=1 || !authority(image,context) ||
        !read(context,&finalTable,sizeof(finalTable)) || finalTable!=originalTable ||
        !read(context+0x160,after.data(),sizeof(after)) || after!=head ||
        !read(context+0x1198,poolAfter.data(),sizeof(poolAfter)) || poolAfter!=descriptorPool ||
        !read(heap,oldCode.data(),oldCode.size()) || oldCode!=original ||
        !read(heap+1536,appended.data(),appended.size()) ||
        !read(descriptors,oldDescriptors.data(),sizeof(oldDescriptors)) || oldDescriptors!=expectedDescriptors ||
        !read(context+0x4390,pointers.data(),sizeof(pointers)) || pointers[0]!=heap || pointers[1]!=heap+1536 ||
        !read(context+0x4590,sizes.data(),sizeof(sizes)) || sizes[0]!=1480 || sizes[1]!=byteSize)
        return {false,true,"native-postcondition"};
    for(size_t i=0;i<isa.size();++i)for(unsigned byte=0;byte<4;++byte)
        if(appended[4*i+byte]!=uint8_t(isa[i]>>(8*byte)))return {false,true,"program-readback"};
    return {true,true,"appended"};
}
}
