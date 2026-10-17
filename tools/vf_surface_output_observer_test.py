#!/usr/bin/env python3
"""Execute the actual wrapper with owned buffers; no kernel/GPU execution."""
from pathlib import Path
import importlib.util
import os
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'NootedGreen/kern_gen11.cpp').read_text()
body = source[source.index('void *Gen11::vfObserveSurfaceState('):source.index('bool Gen11::vfObserveRingBatch(')]
caller = 'reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0))'
assert body.count(caller) == 1
body = body.replace(caller, 'ownedCaller').replace('NGVfCpuRange::kernelRange', 'ownedRange')
# Fixed byte extents only, never scan a binary. Verify each complete instruction
# prefix, native sret return and all three signed call edges independently.
image = root/'sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics'
anchors = {
    0x359e4: bytes.fromhex('554889e54157415641554154534881ec88000000'),
    0x3616d: bytes.fromhex('4889d84881c4880000005b415c415d415e415f5dc3'),
    0x348ea: bytes.fromhex('e8f5100000'),
    0x34adc: bytes.fromhex('e8030f0000'),
    0x34bef: bytes.fromhex('e8f00d0000'),
}
with image.open('rb') as binary:
    for offset, expected in anchors.items():
        binary.seek(offset)
        actual = binary.read(len(expected))
        assert actual == expected
        for index in range(len(expected)):
            changed = bytearray(actual)
            changed[index] ^= 1
            assert changed != expected
        if actual[0] == 0xe8:
            assert offset+5+struct.unpack_from('<i', actual, 1)[0] == 0x359e4
assert sum((1,3,2,2,2,2,1,7)) == 20  # 14-byte jump must span full SUB.
binding_start = source.index('static constexpr uint8_t surfaceEntry[]=')
binding_end = source.index('if(exact && gVfBatchCpuCopy)', binding_start)
binding = source[binding_start:binding_end]
assert 'if(checkKernelArgument("-ngreencaptureonly"))' in source[binding_start-650:binding_start]
binding_function = r'''
static bool actualBinding(uint8_t *image,uint64_t size,int64_t shift=0) {
 const uintptr_t address=reinterpret_cast<uintptr_t>(image);
 const uintptr_t surface=address+0x2534+shift,rect=address;
 const auto contains=[&](uintptr_t entry,uint32_t offset,uint32_t bytes) {
  return entry>=address&&entry-address<=size&&offset<=size-(entry-address)&&bytes<=size-(entry-address)-offset;
 };
 const auto matches=[&](uintptr_t entry,const uint8_t *expected,uint32_t bytes) {
  return contains(entry,0,bytes)&&memcmp(reinterpret_cast<const void*>(entry),expected,bytes)==0;
 };
''' + binding + '\nreturn exact;\n}\n'
binding_test = 'uint8_t image[0x3000]{};\n'
for offset, expected in anchors.items():
    relative = offset-0x334b0
    for index, byte in enumerate(expected):
        binding_test += f'image[{relative+index}]={byte};\n'
binding_test += 'assert(actualBinding(image,sizeof(image)));\n'
for offset, expected in anchors.items():
    relative = offset-0x334b0
    binding_test += f'for(unsigned i={relative};i<{relative+len(expected)};++i){{image[i]^=1;assert(!actualBinding(image,sizeof(image)));image[i]^=1;}}\n'
binding_test += 'assert(!actualBinding(image,0x2cd1));assert(!actualBinding(image,sizeof(image),1));\n'

prefix = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
using mach_vm_address_t=uintptr_t; using UInt32=uint32_t;
#define PANIC_COND(c,...) assert(!(c))
#define arrsize(x) (sizeof(x)/sizeof((x)[0]))
static bool enabled=true,operation=true,flight=true,range=true,casDenied=false,mutate=false,foreign=false;
static uint32_t group=7,calls=0,copyCalls=0,shortAt=0;
static uint32_t params[12]{},output[16]{};
static uintptr_t ownedCaller=11,gVfSurfaceCallers[3]={11,12,13};
static volatile UInt32 gVfSurfaceTickets=0;
static uint32_t gVfFlightRecordSerial=0;
static int gVfFlightTrace=1;
static std::vector<uint32_t> record;
enum class VfIdentity {Virtual,Physical};
static VfIdentity identity=VfIdentity::Virtual;
static VfIdentity vfIdentifyDevice(){return identity;}
static bool checkKernelArgument(const char*){return enabled;}
static bool vfFlightEnabled(){return flight;}
static uint32_t vfFlightCurrentGroup(){return group;}
struct VfContextOperationGuard {explicit operator bool() const{return operation;}};
static bool ownedRange(uint64_t p,uint64_t n){return range &&
 ((p==reinterpret_cast<uintptr_t>(params)&&n==48)||(p==reinterpret_cast<uintptr_t>(output)&&n==64));}
static bool OSCompareAndSwap(uint32_t a,uint32_t b,volatile UInt32 *p){
 if(casDenied||*p!=a)return false;*p=b;return true;}
static uint32_t OSIncrementAtomic(uint32_t *p){return ++*p;}
static uint64_t ownedCopy(uint64_t from,uint64_t to,uint64_t bytes){
 ++copyCalls;assert(calls==(copyCalls==1?0U:1U));if(copyCalls==shortAt)--bytes;
 assert((from==reinterpret_cast<uintptr_t>(params)&&bytes<=48)||
        (from==reinterpret_cast<uintptr_t>(output)&&bytes<=64));
 memcpy(reinterpret_cast<void*>(to),reinterpret_cast<void*>(from),bytes);return bytes;}
static uintptr_t gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(ownedCopy);
namespace NGFlightTrace {
template<class Serial> void emitCpuWordsPrefixed(int,Serial serial,uint32_t kind,uint32_t g,uint64_t id,
 const uint32_t *prefix,uint32_t n,const uint32_t *words,uint32_t count){
 assert(kind==25&&g==7&&id==0&&n==12&&count==40);serial();
 record.assign(prefix,prefix+n);record.insert(record.end(),words,words+count);}
}
struct Gen11 {
 static Gen11 *callback; uintptr_t oVfObserveSurfaceState=0;
 static void *vfObserveSurfaceState(void*,const void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
};
Gen11 *Gen11::callback=nullptr;
static void *native(void *out,const void *in,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){
 ++calls;assert(out==output&&in==params&&a==101&&b==202&&c==303&&d==404&&e==505);
 for(uint32_t i=0;i<16;++i)output[i]=0xfeed0000+i;
 if(mutate)params[0]^=1;
 return foreign?reinterpret_cast<void*>(uintptr_t(0x1234)):out;
}
'''
suffix = r'''
int main(){
 Gen11 instance;Gen11::callback=&instance;instance.oVfObserveSurfaceState=reinterpret_cast<uintptr_t>(native);
 for(unsigned test=0;test<18;++test){
  enabled=operation=flight=range=true;casDenied=mutate=foreign=false;identity=VfIdentity::Virtual;
  group=7;ownedCaller=11;gVfSurfaceTickets=0;gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(ownedCopy);
  calls=copyCalls=shortAt=0;record.clear();for(uint32_t i=0;i<12;++i)params[i]=0x13570000+i;
  if(test==1)enabled=false;if(test==2)identity=VfIdentity::Physical;if(test==3)ownedCaller=99;
  if(test==4)operation=false;if(test==5)group=0;if(test==6)flight=false;if(test==7)range=false;
  if(test==8)gVfBatchCpuCopy=0;if(test==9)gVfSurfaceTickets=64;if(test==10)casDenied=true;
  if(test>=11&&test<=13)shortAt=test-10;
  if(test==14)mutate=true;if(test==15)foreign=true;if(test==16)ownedCaller=12;if(test==17)ownedCaller=13;
  auto *result=Gen11::vfObserveSurfaceState(output,params,101,202,303,404,505);
  assert(calls==1&&result==(foreign?reinterpret_cast<void*>(uintptr_t(0x1234)):output));
  for(uint32_t i=0;i<16;++i)assert(output[i]==0xfeed0000+i);
  if(test>=1&&test<=10){assert(record.empty()&&copyCalls==0);continue;}
  assert(record.size()==52&&copyCalls==3);
  if(shortAt!=3)for(uint32_t i=0;i<16;++i)assert(record[36+i]==0xfeed0000+i);
  assert(record[1]==(test>=16?test-15:0));
  assert(record[6]==101&&record[7]==202&&record[8]==303&&record[9]==404&&record[10]==505);
  assert(bool(record[2]&8)==!(shortAt==1||shortAt==2||mutate));
  assert(bool(record[2]&16)==!foreign);
  if(!shortAt&&!mutate&&!foreign)assert(record[2]==31);
 }
 puts("PASS actual native surface wrapper 18 cases: same sret/arguments/native once/RAX/short-copy/quota/site/scope refusal");
}
'''
suffix = suffix.replace('int main(){', 'int main(){\n'+binding_test, 1)
with tempfile.TemporaryDirectory(prefix='ngreen-surface-owned-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(prefix+binding_function+body+suffix)
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++14', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)

spec = importlib.util.spec_from_file_location('surface_decoder', root/'tools/media_adapter/decode_kdebug_recorder.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
good = [1, 0, 31, 48, 48, 64, 101, 202, 303, 404, 505, 0]+[0]*40
decoder.validate_leaf(25, good)
for offset, value in ((0,2),(1,3),(2,63),(3,49),(4,49),(5,65),(11,1),(12,1),(36,1)):
    bad = list(good)
    bad[offset] = value
    if offset == 36:
        bad[5] = 63; bad[2] &= ~4; bad[51] = 0xff000000
    try:
        decoder.validate_leaf(25, bad)
    except ValueError:
        pass
    else:
        raise AssertionError(f'surface decoder mutation accepted at {offset}')
print('PASS exact surface producer byte/call edges and strict decoder negatives')
with tempfile.TemporaryDirectory(prefix='ngreen-surface-sql-') as directory:
    sink = decoder.DiskSink(Path(directory)/'records')
    saved = dict(record_id=1, kind=25, group=7, identity='0000000000000000', words=good)
    sink(saved)
    assert sink.db.execute('SELECT kind,length(payload) FROM records').fetchall() == [(25,208)]
    for change in (dict(group=0), dict(identity='0000000000000001'), dict(words=good[:-1])):
        bad = dict(saved, record_id=2)
        bad.update(change)
        try:
            sink(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid surface parent/extent entered SQL')
        assert sink.db.execute('SELECT COUNT(*) FROM records').fetchone()[0] == 1
    sink.close()
print('PASS native surface SQL parent/extent rejection and rollback')
