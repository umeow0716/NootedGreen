#!/usr/bin/env python3
"""Actual rect observer and binding on owned CPU buffers, no GPU/native calls."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'NootedGreen/kern_gen11.cpp').read_text()
body=source[source.index('uintptr_t Gen11::vfObserveRectList('):source.index('uintptr_t Gen11::vfObservePoolSubmit(')]
body=body.replace('NGVfCpuRange::kernelRange','ownedRange')
start=source.index('static constexpr uint8_t rectEntry[]=')
end=source.index('KernelPatcher::RouteRequest rectRoute[]=',start)
block=source[start:end]
test_start=block.index('if(exact &&')
arrays=block[:test_start]
condition=block[test_start+3:block.index(') {',test_start)]
binding=r'''
static bool actualBinding(uint8_t *image,uint64_t size,bool exact=true){
 const uintptr_t rect=reinterpret_cast<uintptr_t>(image);
 const auto contains=[&](uintptr_t entry,uint32_t offset,uint32_t bytes){
  return entry>=rect&&entry-rect<=size&&offset<=size-(entry-rect)&&bytes<=size-(entry-rect)-offset;};
 const auto matches=[&](uintptr_t entry,const uint8_t *expected,uint32_t bytes){
  return contains(entry,0,bytes)&&memcmp(reinterpret_cast<const void*>(entry),expected,bytes)==0;};
'''+arrays+'return '+condition+';\n}\n'
anchors={0:bytes.fromhex('554889e54157415641554154534881ecc8010000'),
         0x140:bytes.fromhex('498b8708010000'),
         0x2dc0:bytes.fromhex('48397708737c4989f64989ff4889f748c1e705'),
         0x2e36:bytes.fromhex('49895f104d897708')}
with (root/'sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics').open('rb') as binary:
    for offset,expected in anchors.items():
        binary.seek(0x334b0+offset);assert binary.read(len(expected))==expected
binding_tests='uint8_t image[0x3000]{};\n'
for offset,expected in anchors.items():
    for index,byte in enumerate(expected):binding_tests+=f'image[{offset+index}]={byte};\n'
binding_tests+='assert(actualBinding(image,sizeof(image)));assert(!actualBinding(image,sizeof(image),false));\n'
for offset,expected in anchors.items():
    binding_tests+=f'for(unsigned i={offset};i<{offset+len(expected)};++i){{image[i]^=1;assert(!actualBinding(image,sizeof(image)));image[i]^=1;}}\n'
binding_tests+='assert(!actualBinding(image,0x2e3d));\n'
prefix=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
using UInt32=uint32_t;
#define PANIC_COND(c,...) assert(!(c))
#define arrsize(x) (sizeof(x)/sizeof((x)[0]))
static uint32_t parameters[68],data[32768],gVfRectCpuCopies[4][32768],context;
static uint64_t header[3];static volatile UInt32 gVfRectCpuTickets=0;
static int gVfFlightTrace=1;static uint32_t gVfFlightRecordSerial=0;
enum class VfIdentity{Virtual,Physical};static VfIdentity gVfIdentity=VfIdentity::Virtual;
static bool enabled=true,flight=true,operation=true,range=true,casDenied=false;
static uint32_t group=7,calls=0,copyCalls=0,shortAt=0,mutation=0;
struct Record{uint32_t kind;uint64_t id;std::vector<uint32_t> words;};
static std::vector<Record> records;
static bool checkKernelArgument(const char*){return enabled;}
static bool vfFlightEnabled(){return flight;}
static uint32_t vfFlightCurrentGroup(){return group;}
struct VfContextOperationGuard{explicit operator bool()const{return operation;}};
static bool ownedRange(uint64_t p,uint64_t n){return range&&
 ((p==reinterpret_cast<uintptr_t>(parameters)&&n==272)||(p==reinterpret_cast<uintptr_t>(header)&&n==24)||
  (p==reinterpret_cast<uintptr_t>(data)&&n<=sizeof(data)));}
static bool OSCompareAndSwap(uint32_t a,uint32_t b,volatile UInt32 *p){
 if(casDenied||*p!=a)return false;*p=b;return true;}
static uint32_t OSIncrementAtomic(uint32_t *p){return ++*p;}
static uint64_t ownedCopy(uint64_t from,uint64_t to,uint64_t bytes){
 assert(calls==0);++copyCalls;if(copyCalls==shortAt)--bytes;
 assert((from==reinterpret_cast<uintptr_t>(parameters)&&bytes<=272)||
        (from==reinterpret_cast<uintptr_t>(header)&&bytes<=24)||
        (from==reinterpret_cast<uintptr_t>(data)&&bytes<=sizeof(data)));
 memcpy(reinterpret_cast<void*>(to),reinterpret_cast<const void*>(from),bytes);
 if(from==reinterpret_cast<uintptr_t>(data)){
  if(mutation==1)parameters[0]^=1;if(mutation==2)header[0]^=1;
  if(mutation==3)return bytes+1;
 }return bytes;
}
static uintptr_t gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(ownedCopy);
namespace NGFlightTrace{
template<class Serial>void emitCpuWordsPrefixed(int,Serial serial,uint32_t kind,uint32_t g,uint64_t id,
 const uint32_t *prefix,uint32_t n,const uint32_t *words,uint32_t count){
 assert(calls==0&&g==7&&id==1);serial();Record record{kind,id,{}};
 record.words.assign(prefix,prefix+n);if(count)record.words.insert(record.words.end(),words,words+count);records.push_back(record);
}}
struct Gen11{static Gen11 *callback;uintptr_t oVfObserveRectList=0;static uintptr_t vfObserveRectList(void*,void*,const void*);};
Gen11 *Gen11::callback=nullptr;
static uintptr_t native(void *c,void *p,const void *h){assert(c==&context&&p==parameters&&h==header);
 ++calls;memset(parameters,0xdd,sizeof(parameters));memset(data,0xdd,sizeof(data));return UINT64_C(0xfedcba9876543210);}
'''
suffix=r'''
int main(){
 @BINDING_TESTS@
 Gen11 instance;Gen11::callback=&instance;instance.oVfObserveRectList=reinterpret_cast<uintptr_t>(native);
 for(uint32_t test=0;test<24;++test){
  enabled=flight=operation=range=true;casDenied=false;gVfIdentity=VfIdentity::Virtual;
  group=7;calls=copyCalls=shortAt=mutation=0;gVfRectCpuTickets=0;records.clear();
  gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(ownedCopy);
  for(uint32_t i=0;i<68;++i)parameters[i]=0x23450000+i;
  for(uint32_t i=0;i<32768;++i)data[i]=0x13570000+i;
  header[0]=2;header[1]=4;header[2]=reinterpret_cast<uintptr_t>(data);
  if(test==1)enabled=false;if(test==2)gVfIdentity=VfIdentity::Physical;if(test==3)gVfBatchCpuCopy=0;
  if(test==4)flight=false;if(test==5)operation=false;if(test==6)group=0;
  if(test==7)range=false;if(test==8)gVfRectCpuTickets=4;if(test==9)casDenied=true;
  if(test==10)header[0]=5;if(test==11)header[0]=header[1]=4097;if(test==12)header[2]=0x1234;
  if(test==13)header[0]=header[1]=header[2]=0;if(test==14)header[0]=header[1]=4096;
  if(test>=15&&test<=19)shortAt=test-14;
  if(test>=20&&test<=22)mutation=test-19;
  if(test==23)header[0]=header[1]=UINT64_MAX;
  const uint64_t count=header[0];
  assert(Gen11::vfObserveRectList(&context,parameters,header)==UINT64_C(0xfedcba9876543210)&&calls==1);
  if(test>=1&&test<=9){assert(records.empty());continue;}
  assert(!records.empty()&&records[0].kind==28&&records[0].words.size()==160);
  const auto &meta=records[0].words;
  if(test==0||test==13||test==14)assert(meta[6]==31&&meta[9]==1);
  if(test==10||test==11||test==12||test==23){assert(meta[9]==0&&meta[5]==0&&records.size()==1);continue;}
  if(test==20)assert(!(meta[6]&8));if(test==21)assert(!(meta[6]&16));
  if(test!=15)for(uint32_t i=0;i<68;++i)assert(meta[12+i]==0x23450000+i);
  uint32_t seen=0;
  for(uint32_t i=1;i<records.size();++i){const auto &r=records[i];assert(r.kind==29&&r.words[1]==seen);
   for(uint32_t j=0;j<r.words[4];++j)assert(r.words[6+j]==0x13570000+seen+j);
   seen+=r.words[4];}
  assert(seen==meta[5]/4);
  if(test==13)assert(!seen);if(test==14)assert(seen==count*8);
 }
 puts("PASS actual rect input wrapper 24 cases: pre-native only, raw return/arguments/native once, limit/short/shape changes");
}
'''.replace('@BINDING_TESTS@',binding_tests)
with tempfile.TemporaryDirectory(prefix='ngreen-rect-owned-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(prefix+binding+body+suffix)
    subprocess.run([os.environ.get('CXX','clang++'),'-std=c++14','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

spec=importlib.util.spec_from_file_location('rect_decoder',root/'tools/media_adapter/decode_kdebug_recorder.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
head=[2,0,4,0,0x10000,0xffff8000]
good=[1,272,272,24,24,64,31,4096,32,1,0,0]+[0]*136+head+head
decoder.validate_leaf(28,good)
for index,value in ((0,2),(1,273),(2,273),(3,25),(4,25),(5,65),(6,63),(7,4097),(8,16),(9,0),(10,1),(12,1),(148,5)):
    bad=list(good);bad[index]=value
    try:decoder.validate_leaf(28,bad)
    except ValueError:pass
    else:raise AssertionError(f'rect mutation admitted at {index}')
with tempfile.TemporaryDirectory(prefix='ngreen-rect-sql-') as directory:
    sink=decoder.DiskSink(Path(directory)/'records')
    meta=dict(record_id=1,kind=28,group=7,identity='0000000000000001',words=good);sink(meta)
    assert sink.rect_input_report()['incomplete']==1
    chunk=dict(meta,record_id=2,kind=29,words=[1,0,16,64,16,31]+[0]*16);sink(chunk)
    assert sink.rect_input_report()['complete_control_stable_cpu_copies']==1
    try:sink(dict(chunk,record_id=3))
    except ValueError:pass
    else:raise AssertionError('duplicate rect chunk admitted')
    assert sink.db.execute('SELECT captured FROM rect_inputs').fetchone()[0]==64
    assert not sink.rect_input_report()['full_parameter_extent_proven']
    sink.close()
print('PASS rect metadata/flags/stability/SQL rollback; not full parameter or GPU proof')
