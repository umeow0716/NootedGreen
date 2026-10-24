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
constexpr uint32_t profile=1,nativeBodyWords=72,producedBodyWords=78,nativeEndReserveWords=2;
constexpr uint32_t legacyStreamWords=nativeBodyWords+nativeEndReserveWords;
constexpr uint32_t reservedStreamWords=producedBodyWords+nativeEndReserveWords;
constexpr uint32_t shaderBytes=1576,reservationWords=6;
// Body sizes above exclude the native END/NOOP pair. Native Video end
// subtracts two before passing count to FIFO/ring; those are separate words.
// The CPU reservation marker is consumed before native submission, never an
// instruction or an ACK. Caller proves writable extent/lifetime separately.
inline bool writeReservationTrailer(volatile uint32_t* commands,size_t words) noexcept {
    if(!commands || words!=producedBodyWords ||
        (reinterpret_cast<uintptr_t>(commands)&3U) ||
        reinterpret_cast<uintptr_t>(commands)>UINTPTR_MAX-producedBodyWords*4)return false;
    static constexpr uint32_t marker[reservationWords]={0x4e474352U,profile,nativeBodyWords,producedBodyWords,1,shaderBytes};
    for(unsigned i=0;i<reservationWords;++i)commands[nativeBodyWords+i]=marker[i];
    // Only six words inside the caller's reserved BODY are touched. The
    // separate native END reserve is not this userspace builder's allocation.
    return true;
}
inline bool reservationMatches(const volatile uint32_t* commands,size_t words) noexcept {
    if(!commands || words!=reservedStreamWords || (reinterpret_cast<uintptr_t>(commands)&3U) ||
        reinterpret_cast<uintptr_t>(commands)>UINTPTR_MAX-reservedStreamWords*4)return false;
    static constexpr uint32_t marker[reservationWords]={0x4e474352U,profile,nativeBodyWords,producedBodyWords,1,shaderBytes};
    for(unsigned i=0;i<reservationWords;++i)if(commands[nativeBodyWords+i]!=marker[i])return false;
    // Native reserve contents may be stale. The authenticated end path must
    // independently prove FIFO will overwrite its END/NOOP pair.
    return true;
}
struct HeapAddresses {
    uint64_t surface=0,dynamic=0,instruction=0;
    uint32_t kernelSlot=0;
};
// Opaque scalars copied from the THREE original bind_resource returns.
// This is not a resource retain, lock, epoch proof or permission to dereference
// resource/task/shared. The authenticated native end caller supplies those
// independent lifetime/lock guarantees before consuming this observation.
struct NativeHeapBindings {
    volatile uintptr_t resources[3];
    volatile uint64_t gpu[3];
    volatile uint32_t pitch[3];
    uintptr_t task,shared;
    uint32_t seen;
    bool failed;
    void reset() noexcept {
        for(unsigned i=0;i<3;++i){resources[i]=0;gpu[i]=0;pitch[i]=0;}
        task=shared=0;seen=0;failed=false;
    }
    bool record(unsigned slot,bool nativeReturned,uintptr_t resource,uint64_t address,
        uint32_t rowPitch,bool write,uintptr_t sourceTask,uintptr_t sourceShared) noexcept {
        if(failed)return false;
        if(slot>=3 || seen!=((1U<<slot)-1U) || !nativeReturned || write ||
            !resource || (resource&7U) || !(address&~UINT64_C(4095)) ||
            address>UINT64_C(0x0000ffffffffffff) || !rowPitch || !sourceTask || !sourceShared ||
            (seen && (task!=sourceTask || shared!=sourceShared))) {failed=true;return false;}
        for(unsigned i=0;i<slot;++i)if(resources[i]==resource || (gpu[i]&~UINT64_C(4095))==(address&~UINT64_C(4095))){failed=true;return false;}
        resources[slot]=resource;gpu[slot]=address;pitch[slot]=rowPitch;
        task=sourceTask;shared=sourceShared;seen|=1U<<slot;return true;
    }
    bool copyObservedHeaps(uintptr_t currentTask,uintptr_t currentShared,HeapAddresses& output) const noexcept {
        if(failed || seen!=7 || !task || task!=currentTask || !shared || shared!=currentShared)return false;
        for(unsigned i=0;i<3;++i) {
            if(!resources[i] || (resources[i]&7U) || !(gpu[i]&~UINT64_C(4095)) ||
                gpu[i]>UINT64_C(0x0000ffffffffffff) || !pitch[i])return false;
            for(unsigned j=0;j<i;++j)if(resources[j]==resources[i] || (gpu[j]&~UINT64_C(4095))==(gpu[i]&~UINT64_C(4095)))return false;
        }
        // Native token order is DYNAMIC, INSTRUCTION, SURFACE. It is not the
        // order of their DWORD pairs in STATE_BASE_ADDRESS.
        // Exact native3687a..368b1 floors each returned address to a page
        // before setting the SBA modify bit. Preserve raw scalars above;
        // reproduce that established alignment, never invent another mapping.
        const HeapAddresses candidate{gpu[2]&~UINT64_C(4095),gpu[0]&~UINT64_C(4095),gpu[1]&~UINT64_C(4095),1};
        output=candidate;return true;
    }
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
// CPU publication only; never a GPU completion or native submit ACK. The
// caller independently holds the mapped span/task/physical/native-IOLock
// authority, plus the observed heap identities AND native append-END mode.
// This function writes the two separately admitted kernel reserve words,
// while writeReservationTrailer above touches only the user BODY allocation.
// It must CANCEL every refused
// candidate, including one successfully restored, rather than submit legacy
// body/marker bytes. Lost authority forbids any rollback write.
struct Publication {bool published=false,restored=true,mustStop=false;const char* reason="extent";};
template<class Scratch,class Live,class Same>
inline Publication publish(volatile uint32_t* commands,size_t capacity,size_t reserved,
    volatile uint32_t& count,const HeapAddresses& heaps,Scratch& scratch,Live live,Same same) noexcept {
    static_assert(sizeof(scratch.source)/sizeof(scratch.source[0])>=reservedStreamWords &&
        sizeof(scratch.produced)/sizeof(scratch.produced[0])>=reservedStreamWords &&
        sizeof(scratch.undo)/sizeof(scratch.undo[0])>=reservedStreamWords,"checksum scratch extent");
    Publication result;
    if(!commands || capacity>16384 || reserved>capacity || reserved<reservedStreamWords ||
        count!=reservedStreamWords)return result;
    const uintptr_t begin=reinterpret_cast<uintptr_t>(commands),counter=reinterpret_cast<uintptr_t>(&count);
    const uintptr_t work=reinterpret_cast<uintptr_t>(&scratch);
    const auto disjoint=[](uintptr_t a,size_t an,uintptr_t b,size_t bn) noexcept {
        return a<=UINTPTR_MAX-an && b<=UINTPTR_MAX-bn && (a+an<=b || b+bn<=a);
    };
    if((begin&3U) || (counter&3U) || !disjoint(begin,reservedStreamWords*4,counter,4) ||
        !disjoint(begin,reservedStreamWords*4,work,sizeof(scratch)) || !disjoint(counter,4,work,sizeof(scratch))) {
        result.reason="alias";return result;
    }
    if(!same() || !live(reservedStreamWords)){result.reason="owner";return result;}
    for(unsigned i=0;i<reservedStreamWords;++i)scratch.undo[i]=scratch.source[i]=commands[i];
    if(!reservationMatches(scratch.source,reservedStreamWords)){result.reason="reservation";return result;}
    const auto prepared=build1080Batch(scratch.source,nativeBodyWords,heaps,scratch.produced,producedBodyWords);
    if(!prepared.ok){result.reason=prepared.reason;return result;}
    scratch.produced[producedBodyWords]=scratch.produced[producedBodyWords+1]=0;
    if(!same() || !live(reservedStreamWords)){result.reason="owner-before-write";return result;}
    for(unsigned i=0;i<reservedStreamWords;++i)if(commands[i]!=scratch.undo[i]) {
        result.reason="source-changed";return result;
    }
    const auto restore=[&]() noexcept {
        if(!same() || !live(reservedStreamWords))return false;
        for(unsigned i=0;i<reservedStreamWords;++i)commands[i]=scratch.undo[i];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if(!same() || !live(reservedStreamWords))return false;
        for(unsigned i=0;i<reservedStreamWords;++i)if(commands[i]!=scratch.undo[i])return false;
        return true;
    };
    for(unsigned i=0;i<reservedStreamWords;++i)commands[i]=scratch.produced[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    bool verified=same() && live(reservedStreamWords);
    if(verified)for(unsigned i=0;i<reservedStreamWords;++i)if(commands[i]!=scratch.produced[i]){verified=false;break;}
    if(verified)verified=same() && live(reservedStreamWords);
    if(!verified) {
        result.reason="write-readback";result.restored=restore();result.mustStop=!result.restored;return result;
    }
    count=reservedStreamWords; // count-last: still no native descriptor/stamp/queue operation.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if(count!=reservedStreamWords || !same() || !live(reservedStreamWords)) {
        result.reason="count-readback";result.restored=restore();result.mustStop=!result.restored;return result;
    }
    result.published=true;result.reason="published";return result;
}
}
