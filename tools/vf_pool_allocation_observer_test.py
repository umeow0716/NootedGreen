#!/usr/bin/env python3
"""Actual pre-submit allocation observer on owned CPU data; never queue work."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'NootedGreen/kern_gen11.cpp').read_text()
body=source[source.index('uintptr_t Gen11::vfObservePoolSubmit('):source.index('void *Gen11::vfObserveSurfaceState(')]
body=body.replace('reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0))','ownedCaller')
body=body.replace('NGVfCpuRange::kernelRange','ownedRange')
bind_start=source.index('const auto submit=patcher.solveSymbol(index,"__ZN25IOAccelCommandBufferPool212submitBufferEv"')
bind_end=source.index('KernelPatcher::RouteRequest poolRoute[]=',bind_start)
block=source[bind_start:bind_end]
array_start=block.index('static constexpr uint8_t entry[]=')
array=block[array_start:block.index(';',array_start)+1]
guard_start=block.index('bool exactPool=')
guard=block[guard_start:block.index('if(exactPool) {',block.index('if(exactPool) {')+1)]
assert 'if(checkKernelArgument("-ngreencaptureonly"))' in source[bind_start-80:bind_start]
assert 'static constexpr uint8_t poolCall[]={0x48,0x89,0xdf,0xe8};' in source
assert 'gVfPoolSubmitCaller=rect+0x2122' in source
binding_function='static bool actualPoolBinding(uint8_t *image,uint64_t size,uintptr_t submit){\nconst uintptr_t address=reinterpret_cast<uintptr_t>(image);\n'+array+'\n'+guard+'\nreturn exactPool;\n}\n'
prefix=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "@HEADER@"
using mach_vm_address_t=uintptr_t;using UInt32=uint32_t;
#define PANIC_COND(c,...) assert(!(c))
#define arrsize(x) (sizeof(x)/sizeof((x)[0]))
alignas(8) static uint8_t pool[0x1878];
static uint32_t backing[16384],gVfPoolCpuCopies[4][16384];
static uintptr_t gVfPoolSubmitCaller=11,ownedCaller=11;
static volatile UInt32 gVfPoolCpuTickets=0;
static uint32_t gVfFlightRecordSerial=0;
static int gVfFlightTrace=1,accelerator;
static void *gVfAccelerator=&accelerator;
enum class VfIdentity{Virtual,Physical};static VfIdentity gVfIdentity=VfIdentity::Virtual;
static bool enabled=true,flight=true,operation=true,range=true,casDenied=false;
static uint32_t group=7,calls=0,mutation=0,copyFault=0;
struct Record{uint32_t kind;uint64_t id;std::vector<uint32_t> words;};
static std::vector<Record> records;
template<class T> static void put(uint32_t offset,T value){memcpy(pool+offset,&value,sizeof(value));}
static bool checkKernelArgument(const char*){return enabled;}
static bool vfFlightEnabled(){return flight;}
static uint32_t vfFlightCurrentGroup(){return group;}
struct VfContextOperationGuard{explicit operator bool()const{return operation;}};
static bool ownedRange(uint64_t p,uint64_t n){return range&&
 ((p==reinterpret_cast<uintptr_t>(pool)&&n==0x1874)||
  (p==reinterpret_cast<uintptr_t>(backing)&&n<=sizeof(backing)));}
static bool OSCompareAndSwap(uint32_t a,uint32_t b,volatile UInt32 *p){
 if(casDenied||*p!=a)return false;*p=b;return true;}
static uint32_t OSIncrementAtomic(uint32_t *p){return ++*p;}
static uint64_t ownedCopy(uint64_t from,uint64_t to,uint64_t bytes){
 assert(calls==0);
 if((copyFault==1&&from==reinterpret_cast<uintptr_t>(pool+0x1830))||
    (copyFault==2&&from==reinterpret_cast<uintptr_t>(pool+0x10))){--bytes;}
 if(from==reinterpret_cast<uintptr_t>(backing)){
  if(mutation==4)bytes-=4;
  memcpy(reinterpret_cast<void*>(to),reinterpret_cast<void*>(from),bytes);
  if(mutation==1)put<uintptr_t>(0x10,0x1234);
  if(mutation==2)put<uint32_t>(0x1838,0x8888);
  if(mutation==3)put<uintptr_t>(0x40,reinterpret_cast<uintptr_t>(backing)+4096);
  return mutation==5?bytes+4:bytes;
 }
 assert(from>=reinterpret_cast<uintptr_t>(pool)&&from+bytes<=reinterpret_cast<uintptr_t>(pool)+sizeof(pool));
 memcpy(reinterpret_cast<void*>(to),reinterpret_cast<void*>(from),bytes);return bytes;
}
static uintptr_t gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(ownedCopy);
namespace NGFlightTrace{
template<class Serial> void emitCpuWordsPrefixed(int,Serial serial,uint32_t kind,uint32_t g,uint64_t id,
 const uint32_t *prefix,uint32_t n,const uint32_t *data,uint32_t count){
 assert(calls==0&&g==7&&id==1);serial();Record out{kind,id,{}};
 out.words.assign(prefix,prefix+n);if(count)out.words.insert(out.words.end(),data,data+count);records.push_back(out);
}}
struct Gen11{static Gen11 *callback;uintptr_t oVfObservePoolSubmit=0;static uintptr_t vfObservePoolSubmit(void*);};
Gen11 *Gen11::callback=nullptr;
static uintptr_t native(void *p){assert(p==pool);++calls;memset(backing,0xdd,sizeof(backing));return 0xabcdef12;}
'''.replace('@HEADER@',str(root/'NootedGreen/kern_ioaccel_command_pool.hpp'))
suffix=r'''
int main(){
 uint8_t nativeEntry[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x4c,0x8b,0xb7,0x48,0x18,0,0};
 auto *entry=nativeEntry;const uintptr_t address=reinterpret_cast<uintptr_t>(entry);
 assert(actualPoolBinding(entry,sizeof(nativeEntry),address));
 for(unsigned i=0;i<sizeof(nativeEntry);++i){entry[i]^=1;assert(!actualPoolBinding(entry,sizeof(nativeEntry),address));entry[i]^=1;}
 assert(!actualPoolBinding(entry,sizeof(nativeEntry)-1,address));
 assert(!actualPoolBinding(entry,sizeof(nativeEntry),0));assert(!actualPoolBinding(entry,sizeof(nativeEntry),address-1));
 Gen11 instance;Gen11::callback=&instance;instance.oVfObservePoolSubmit=reinterpret_cast<uintptr_t>(native);
 for(uint32_t test=0;test<28;++test){
  memset(pool,0,sizeof(pool));for(uint32_t i=0;i<16384;++i)backing[i]=0x13570000+i;
  enabled=flight=operation=range=true;casDenied=false;group=7;ownedCaller=11;
  gVfIdentity=VfIdentity::Virtual;gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(ownedCopy);
  gVfPoolCpuTickets=0;calls=mutation=copyFault=0;records.clear();
  const uintptr_t base=reinterpret_cast<uintptr_t>(backing);
  const uint32_t bytes=test==24?4096:65536;
  put<uintptr_t>(0x10,reinterpret_cast<uintptr_t>(gVfAccelerator));
  put<uintptr_t>(0x30,1);put<uintptr_t>(0x38,2);put<uintptr_t>(0x40,base);
  put<uint32_t>(0x1830,0x00010001);put<uint32_t>(0x1834,bytes);
  put<uint16_t>(0x1842,0);put<uintptr_t>(0x1848,base+64);
  put<uintptr_t>(0x1850,base+bytes-8);put<uintptr_t>(0x1858,base+3488);put<uint32_t>(0x1870,8);
  if(test==1)enabled=false;if(test==2)gVfIdentity=VfIdentity::Physical;if(test==3)ownedCaller=99;
  if(test==4)gVfBatchCpuCopy=0;if(test==5)flight=false;if(test==6)operation=false;if(test==7)group=0;
  if(test==8)put<uintptr_t>(0x10,0x1234);if(test==9)put<uint16_t>(0x1830,257);
  if(test==10)put<uint16_t>(0x1842,1);if(test==11)put<uintptr_t>(0x30,0);
  if(test==12)put<uintptr_t>(0x38,0);if(test==13)put<uintptr_t>(0x40,0x1234);
  if(test==14)put<uintptr_t>(0x1850,base+bytes);if(test==15)put<uint32_t>(0x1870,bytes+1);
  if(test==16)put<uint32_t>(0x1834,65540);if(test==17)gVfPoolCpuTickets=4;if(test==18)casDenied=true;
  if(test>=19&&test<=23)mutation=test-18;if(test==25)range=false;
  if(test==26)copyFault=1;if(test==27)copyFault=2;
  assert(Gen11::vfObservePoolSubmit(pool)==0xabcdef12&&calls==1);
  if((test>=1&&test<=18)||test>=25){assert(records.empty());continue;}
  assert(!records.empty()&&records[0].kind==26&&records[0].words.size()==12);
  const auto &meta=records[0].words;assert(meta[1]==bytes&&meta[3]==64&&meta[4]==3488);
  const uint32_t expectedFlags=(test>=19&&test<=21)?3:(test==22||test==23)?5:7;
  assert(meta[6]==expectedFlags);
  uint32_t seen=0;
  for(uint32_t i=1;i<records.size();++i){const auto &r=records[i];assert(r.kind==27&&r.words[1]==seen);
   for(uint32_t j=0;j<r.words[4];++j)assert(r.words[6+j]==0x13570000+seen+j);
   seen+=r.words[4];}
  assert(seen==(test==23?0:(test==22?bytes-4:bytes)/4));
 }
 puts("PASS actual pre-native pool allocation observer 28 cases; native once/RAX preserved and post-submit rotation not read");
}
'''
with tempfile.TemporaryDirectory(prefix='ngreen-pool-owned-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(prefix+binding_function+body+suffix)
    subprocess.run([os.environ.get('CXX','clang++'),'-std=c++14','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

spec=importlib.util.spec_from_file_location('pool_decoder',root/'tools/media_adapter/decode_kdebug_recorder.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
good=[1,4096,8,64,3488,4096,7,0,1,1,0,0]
decoder.validate_leaf(26,good)
for index,value in ((0,2),(1,65540),(2,4097),(3,3500),(4,4096),(5,4097),(6,5),(7,1),(8,257),(10,1),(11,1)):
    bad=list(good);bad[index]=value
    try:decoder.validate_leaf(26,bad)
    except ValueError:pass
    else:raise AssertionError(f'pool metadata mutation admitted: {index}')
with tempfile.TemporaryDirectory(prefix='ngreen-pool-sql-') as directory:
    sink=decoder.DiskSink(Path(directory)/'records')
    meta=dict(record_id=1,kind=26,group=7,identity='0000000000000001',words=good)
    sink(meta)
    chunk=dict(meta,record_id=2,kind=27,words=[1,512,1024,4096,512,7]+[0]*512)
    try:sink(chunk)
    except ValueError:pass
    else:raise AssertionError('out-of-order pool chunk admitted')
    chunk['words'][1]=0;sink(chunk)
    try:sink(dict(chunk,record_id=3))
    except ValueError:pass
    else:raise AssertionError('duplicate pool chunk admitted')
    assert sink.pool_allocation_report()['incomplete']==1
    chunk['record_id']=3;chunk['words'][1]=512;sink(chunk)
    assert sink.pool_allocation_report()['complete_control_stable_cpu_copies']==1
    assert not sink.pool_allocation_report()['gpu_execution_proof']
    sink.close()
print('PASS pool allocation strict metadata/order/duplicate/SQL rollback and incomplete coverage')
