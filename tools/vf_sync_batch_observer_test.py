#!/usr/bin/env python3
"""Execute the actual synchronous observer body with owned CPU-only callbacks.

Only the CPU return-address intrinsic is injected; this does not execute the
native kernel copier, configure tracing, route a function, or access a GPU.
"""
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'NootedGreen/kern_gen11.cpp').read_text()
if sys.argv[1:2] == ['--resource-layout']:
    if len(sys.argv) != 3:
        raise SystemExit('usage: vf_sync_batch_observer_test.py --resource-layout PINNED_GEN12_VDBOX_DIR')
    fixture = r'''
    #include "mhw_vdbox_mfx_hwcmd_g12_X.h"
    #include "mhw_vdbox_vdenc_hwcmd_g12_X.h"
    using M=mhw_vdbox_mfx_g12_X;using V=mhw_vdbox_vdenc_g12_X;
    using P=M::MFX_PIPE_BUF_ADDR_STATE_CMD;using I=M::MFX_IND_OBJ_BASE_ADDR_STATE_CMD;
    using B=M::MFX_BSP_BUF_BASE_ADDR_STATE_CMD;using D=V::VDENC_PIPE_BUF_ADDR_STATE_CMD;
    static_assert(sizeof(P)==68*4,"MFX pipe extent");
    static_assert(offsetof(P,DW7)==7*4,"MFX source");
    static_assert(offsetof(P,Refpicbaseaddr)==19*4,"MFX references");
    static_assert(sizeof(P::Refpicbaseaddr)==16*8,"MFX 16 packed references");
    static_assert(offsetof(P,DW51)==51*4,"MFX shared reference attributes");
    static_assert(offsetof(P,DW61)==61*4,"MFX compression is not an address");
    static_assert(offsetof(P,DW65)==65*4,"MFX slice-size streamout");
    static_assert(sizeof(I)==26*4 && offsetof(I,DW21)==21*4 && offsetof(I,DW24)==24*4,"MFX indirect pair");
    static_assert(sizeof(B)==10*4 && offsetof(B,DW7)==7*4,"MFX BSP bitplane");
    static_assert(sizeof(D)==71*4,"VDENC pipe extent");
    static_assert(offsetof(D,OriginalUncompressedPicture)==10*4,"VDENC source");
    static_assert(offsetof(D,StreaminDataPicture)==13*4,"VDENC streamin");
    static_assert(offsetof(D,RowStoreScratchBuffer)==16*4,"VDENC rowstore");
    static_assert(offsetof(D,DW61)==61*4,"VDENC histogram is not an address");
    static_assert(offsetof(D,VdencTileRowStoreBuffer)==62*4,"VDENC final triples");
    '''
    with tempfile.TemporaryDirectory(prefix='ngreen-resource-layout-') as directory:
        cpp = Path(directory)/'offsets.cpp'; cpp.write_text(fixture)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++14', '-Werror',
                        '-fsyntax-only', '-I', sys.argv[2], str(cpp)], check=True)
    print('PASS independent compiled Gen12 MFX/VDENC resource sizes/offsets; no resource or GPU call')
elif sys.argv[1:]:
    if len(sys.argv) != 4 or sys.argv[1] != '--native':
        raise SystemExit('usage: vf_sync_batch_observer_test.py [--native TGL KERNEL]')
    def bounded(path, offset, size):
        assert 0 < size <= 1024
        with Path(path).open('rb') as file:
            file.seek(offset)
            data = file.read(size)
        assert len(data) == size
        return data
    def fnv(data):
        value = 14695981039346656037
        for byte in data:
            value = ((value ^ byte)*1099511628211) & ((1 << 64)-1)
        return value
    mutations = 0
    for path, offset, size, digest in (
            (sys.argv[2], 0x42f7a, 242, 0x6c30a64a7547b329),
            (sys.argv[3], 0x1e4dc0, 362, 0x648d5b7fd7277f0e),
            (sys.argv[3], 0x1e0b10, 208, 0xf9172f81775ea555),
            (sys.argv[3], 0x1c9880, 816, 0xddd9ec77c57b4d13)):
        data = bounded(path, offset, size)
        assert f'0x{digest:016x}' in source and fnv(data) == digest
        for index in range(size):
            bad = bytearray(data); bad[index] ^= 1
            assert fnv(bad) != digest
            mutations += 1
    assert bounded(sys.argv[2], 0x42f7a, 14) == bytes.fromhex('554889e54156534883ec104989f6')
    caller = bounded(sys.argv[2], 0x4c828, 47)
    assert caller == bytes.fromhex('498bbc2430010000498b7618498b4e20458b4630450fb64e6a410fb64669488b1f4883ec084489fa50ff9318010000')
    copier = bounded(sys.argv[3], 0x1e4dc0, 362)
    for displacement, next_instruction, target in (
            (0x31, 0x35, 0xffffff80003c9880),
            (0x7b, 0x7f, 0xffffff80003c9880),
            (0x12d, 0x131, 0xffffff80003e0b10)):
        assert 0xffffff80003e4dc0 + next_instruction + struct.unpack_from('<i', copier, displacement)[0] == target
    print(f'PASS bounded native batch/copy/pmap spans and {mutations} byte mutations; exact caller and relocation-free entry; static only')
start = source.index('bool Gen11::vfObserveRingBatch(')
stop = source.index('\nvoid Gen11::vfMediaEnd(', start)
body = source[start:stop]
assert body.count('__builtin_return_address(0)') == 1
body = body.replace('__builtin_return_address(0)', 'testCaller')
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "NootedGreen/kern_flight_trace.hpp"
using mach_vm_address_t=uintptr_t;using UInt32=uint32_t;
#define PANIC_COND(c,...) assert(!(c))
#define FunctionCast(f,p) reinterpret_cast<decltype(&f)>(p)
#define arrsize(a) (sizeof(a)/sizeof((a)[0]))
static bool enabled=true,operationEnabled=true,nativeResult=true;
static uint32_t group=3,nativeCalls=0,sourceCopies=0,ownerCopies=0,metadata=0,traceEvents=0;
static uint32_t lastFlags=0,lastCopied=0;
static uint64_t shortCopy=UINT64_MAX;
static unsigned changeAtOwnerRead=0;
static void *gVfAccelerator=reinterpret_cast<void *>(0xffffff8040200000ULL);
static void *owner=gVfAccelerator,*pmap=reinterpret_cast<void *>(0x1234);
static void **gVfCpuKernelPmapSlot=&pmap;static void *gVfCpuKernelPmapStore=pmap;
static uintptr_t testCaller=9,gVfBatchCaller=9;
static uint32_t gVfBatchCpuCopies[8][16384]{};
static volatile UInt32 gVfBatchCpuTickets=0;
static volatile int gVfFlightRecordSerial=0;
struct VfContextOperationGuard {explicit operator bool()const{return operationEnabled;}};
static bool vfFlightEnabled(){return enabled;}
static uint32_t vfFlightCurrentGroup(){return group;}
static bool OSCompareAndSwap(uint32_t a,uint32_t b,volatile uint32_t *p){if(*p!=a)return false;*p=b;return true;}
static int OSIncrementAtomic(volatile int *p){return ++*p;}
static void trace(uint32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t){++traceEvents;}
static NGFlightTrace::Callback gVfFlightTrace=trace;
static void vfFlightRecord(uint32_t kind,uint32_t g,uint64_t id,const volatile uint32_t *w,uint32_t n){
    assert(kind==19 && g==3 && id>=1 && id<=8 && n==13 && w[12]==0);
    ++metadata;lastFlags=w[7];lastCopied=w[6];
}
static void *ring=reinterpret_cast<void *>(0xffffff8040100000ULL);
static uint32_t *cpu=reinterpret_cast<uint32_t *>(0xffffff8040000000ULL);
static uint64_t nativeGpu=0x41d34000;
static uint32_t expectedCount=16;
static uint64_t copy(uint64_t src,uint64_t dst,uint64_t bytes){
    if(src==reinterpret_cast<uintptr_t>(ring)+0x10){
        assert(bytes==8);++ownerCopies;
        void *value=changeAtOwnerRead && ownerCopies>=changeAtOwnerRead?nullptr:owner;
        std::memcpy(reinterpret_cast<void *>(dst),&value,8);return 8;
    }
    assert(src==reinterpret_cast<uintptr_t>(cpu) && bytes==uint64_t(expectedCount)*4);
    ++sourceCopies;uint64_t done=shortCopy==UINT64_MAX?bytes:shortCopy;
    for(uint64_t i=0;i<(done<=bytes?done:0);++i)reinterpret_cast<uint8_t *>(dst)[i]=uint8_t(i);
    return done;
}
static mach_vm_address_t gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(&copy);
class Gen11 {public:
    static bool vfObserveRingBatch(void *,uint64_t,uint32_t,uint32_t *,uint32_t,bool,bool);
    mach_vm_address_t oVfObserveRingBatch=0;
};
static Gen11 instance,*callback=&instance;
static bool native(void *r,uint64_t gpu,uint32_t stamp,uint32_t *p,uint32_t count,bool a,bool b){
    assert(r==ring && gpu==nativeGpu && stamp==7 && p==cpu && count==expectedCount && a && !b);
    ++nativeCalls;return nativeResult;
}
static void reset(){
    enabled=operationEnabled=nativeResult=true;group=3;testCaller=gVfBatchCaller=9;
    nativeCalls=sourceCopies=ownerCopies=metadata=traceEvents=0;lastFlags=lastCopied=0;
    shortCopy=UINT64_MAX;changeAtOwnerRead=0;owner=gVfAccelerator;gVfBatchCpuTickets=0;
    expectedCount=16;pmap=gVfCpuKernelPmapStore;
    callback->oVfObserveRingBatch=reinterpret_cast<uintptr_t>(&native);
}
static void invoke(){assert(Gen11::vfObserveRingBatch(ring,nativeGpu,7,cpu,expectedCount,true,false)==nativeResult);}
'''
tests = r'''
int main(){
    reset();invoke();assert(nativeCalls==1 && sourceCopies==1 && metadata==1 && lastFlags==7 && lastCopied==64 && traceEvents);
    reset();nativeResult=false;invoke();assert(nativeCalls==1 && metadata==1);
    for(unsigned reason=0;reason<6;++reason){
        reset();if(reason==0)enabled=false;if(reason==1)operationEnabled=false;
        if(reason==2)testCaller=10;if(reason==3)group=0;if(reason==4)owner=nullptr;
        if(reason==5)pmap=nullptr;
        invoke();assert(nativeCalls==1 && !sourceCopies && !metadata && !traceEvents);
    }
    reset();expectedCount=16384;invoke();assert(nativeCalls==1 && sourceCopies==1 && lastCopied==65536 && lastFlags==7);
    for(uint32_t count:{0U,16385U,UINT32_MAX}){
        reset();expectedCount=count;invoke();assert(nativeCalls==1 && !sourceCopies && !metadata);
    }
    reset();auto *savedCpu=cpu;cpu=reinterpret_cast<uint32_t *>(0xffff000040000000ULL);
    invoke();assert(nativeCalls==1 && !sourceCopies && !metadata);cpu=savedCpu;
    for(uint64_t amount:{0ULL,3ULL,60ULL,65ULL}){
        reset();shortCopy=amount;invoke();assert(nativeCalls==1 && sourceCopies==1 && metadata==1 && lastFlags==5);
        assert(lastCopied==(amount<=64?amount:0));if(amount<4 || amount>64)assert(!traceEvents);
    }
    reset();changeAtOwnerRead=2;invoke();assert(nativeCalls==1 && !sourceCopies && metadata==1 && !lastFlags);
    reset();changeAtOwnerRead=3;invoke();assert(nativeCalls==1 && sourceCopies==1 && metadata==1 && lastFlags==3 && !traceEvents);
    reset();for(unsigned i=0;i<10;++i)invoke();assert(nativeCalls==10 && sourceCopies==8 && metadata==8 && gVfBatchCpuTickets==8);
}
'''
with tempfile.TemporaryDirectory(prefix='ngreen-sync-batch-') as directory:
    cpp = Path(directory)/'actual.cpp'
    cpp.write_text(prefix+body+tests)
    binary = Path(directory)/'test'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++14', '-O1', '-g',
                    '-fsanitize=address,undefined', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS actual synchronous observer ASan/UBSan: native once/false/owner/caller/gates/short-copy/stability/64KiB/budget; owned CPU callbacks only')
