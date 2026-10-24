#!/usr/bin/env python3
"""Compile actual checksum MainEnd/qualification blocks against owned mocks.

No VM, MMIO, native resource invocation or GPU execution. Fixed native image
reads are limited to the thunk, vtable slot and FIFO END-mode byte windows.
The hook's production readiness stays false; mocks exercise its future branch.
"""
from pathlib import Path
import os
import shlex
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
CXX=shlex.split(os.environ.get("CXX","clang++"))
assert CXX, "empty compiler"
# Sanitizer findings are failures, including those in the owned mock helpers.
os.environ["UBSAN_OPTIONS"]="halt_on_error=1"

def test_qualification(root):
    s=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    a=s.index('                const auto mainEnd=patcher.solveSymbol');b=s.index('                if(checksumEndQualified)',a);body=s[a:b]
    image=(ROOT/'sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics')
    with image.open('rb') as f:
     h=f.read(32);n,sz=struct.unpack_from('<II',h,16);assert sz<4096;c=f.read(sz);pos=0;segs=[]
     for _ in range(n):
      cmd,num=struct.unpack_from('<II',c,pos)
      if cmd==0x19:segs.append(struct.unpack_from('<4Q',c,pos+24))
      pos+=num
     def read(va,num):
      assert num<=4096
      for start,vs,off,fs in segs:
       if start<=va and va+num<=start+fs:
        f.seek(off+va-start);return f.read(num)
      raise AssertionError('fixed range absent')
     thunk=read(0x36a6c,18);fifo=read(0x4c710,53)
     assert struct.unpack('<q',read(0xd2b70+0xa48,8))[0]==0x36a6c
    prefix=r'''
    #include <cstdint>
    #include <cstring>
    #include <cassert>
    #include <cstdio>
    #include <vector>
    using mach_vm_address_t=uintptr_t;
    template<class T>T& getMember(void*p,size_t o){return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+o);}
    std::vector<uint8_t> image(0x100000);
    bool gVfCaptureStreamResetBound=true,bindQualified=true;
    uintptr_t address=reinterpret_cast<uintptr_t>(image.data()),size=image.size();
    uintptr_t mediaEnd=address+0x40d62,videoEnd=address+0x775f2,videoTable=address+0xda430,mainTable=address+0xd2b70;
    struct Patcher {uintptr_t solveSymbol(int,const char*s,uintptr_t,uintptr_t){return strstr(s,"ContextMain16")?address+0x36a6c:address+0x4c6c6;}} patcher;
    auto contains=[](uintptr_t p,size_t o,size_t n){return p>=address && p-address<=size && o<=size-(p-address) && n<=size-(p-address)-o;};
    auto matches=[](uintptr_t p,const uint8_t*e,size_t n){return memcmp(reinterpret_cast<void*>(p),e,n)==0;};
    bool qualified(){int index=0;
    '''
    suffix='return checksumEndQualified;\n}\n'
    suffix+='static const uint8_t thunk[]={'+','.join(str(v) for v in thunk)+'};\n'
    suffix+='static const uint8_t fifo[]={'+','.join(str(v) for v in fifo)+'};\n'
    suffix+=r'''
    int main(){
     memcpy(image.data()+0x36a6c,thunk,sizeof(thunk));memcpy(image.data()+0x4c710,fifo,sizeof(fifo));
     getMember<uintptr_t>(reinterpret_cast<void*>(mainTable),0xa48)=address+0x36a6c;
     assert(qualified());unsigned count=0;
     for(auto range:{std::pair<size_t,size_t>{0x36a6c,sizeof(thunk)}, {0x4c710,sizeof(fifo)}, {0xd2b70+0xa48,8}})
      for(size_t i=0;i<range.second;++i)for(unsigned bit=0;bit<8;++bit){
       auto&p=image[range.first+i];p^=1U<<bit;assert(!qualified());p^=1U<<bit;++count;
      }
     bindQualified=false;assert(!qualified());bindQualified=true;
     gVfCaptureStreamResetBound=false;assert(!qualified());gVfCaptureStreamResetBound=true;
     auto old=mainTable;mainTable=0;assert(!qualified());mainTable=old;
     assert(qualified());printf("Main thunk/FIFO mode actual qualification: %u byte-bit mutations and independent gates PASS\n",count);
    }
    '''
    p=root/'main_end_qualification_actual.cpp';p.write_text(prefix+body+suffix)
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',str(root/'main_end_qualification_actual')],check=True)
    subprocess.run([str(root/'main_end_qualification_actual')],check=True)

def test_end(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    a=source.index('void Gen11::vfChecksumEnd(');b=source.index('\nvoid Gen11::vfMediaEnd(',a);body=source[a:b].replace('reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0))','fakeCaller')
    a=source.index('\tstatic bool bindLocked(');b=source.index('\tstatic VfMediaSubmission *find(',a);scope=source[a:b]
    # Complete 72-DW captured checksum CPU body; not a GPU execution fixture.
    words=(0x11000001,0x0,0x80000040,0x7a000004,0x1010a0,0x0,0x0,0x0,0x0,0x69041301,0x6101000e,0x1,0x0,0x0,0x40a6e001,0x0,0x40a6c001,0x0,0x1,0x0,0x40a6d001,0x0,0x1,0x1001,0x1,0x1001,0x70000007,0x0,0x0,0x1bf1000,0x0,0x7c0002,0x0,0x0,0x0,0x70010002,0x0,0x40,0x400,0x70020002,0x0,0x40,0x0,0x7103000f,0x0,0x0,0x0,0x0,0x0,0x0,0x7ff07ff,0x20044,0x0,0x0,0x10000,0x1,0x20044,0x0,0x44,0x20000,0x7a000004,0x1010a0,0x0,0x0,0x0,0x0,0x7a000004,0x89c,0x0,0x0,0x0,0x0,)
    prefix=r'''
    #include "NootedGreen/kern_checksum_gen12_commands.hpp"
    #include "NootedGreen/kern_vf_submission_gate.hpp"
    #include <array>
    #include <cstdint>
    #include <cassert>
    #include <cstdio>
    #include <cstring>
    using mach_vm_address_t=uintptr_t;using UInt32=uint32_t;
    template<class T>T& getMember(void*p,size_t o){return *reinterpret_cast<T*>(static_cast<unsigned char*>(p)+o);}
    struct Panic{};
    #define PANIC_COND(c,t,m) do{if(c)throw Panic{};}while(0)
    template<class...A>void logOnly(const char*,const char*f,A...args){std::printf(f,args...);std::puts("");}
    #define SYSLOG logOnly
    bool args=true;bool checkKernelArgument(const char*){return args;}
    void* current_thread(){return reinterpret_cast<void*>(0x4000);}
    enum class VfIdentity {Virtual,Physical};VfIdentity gVfIdentity=VfIdentity::Virtual;
    bool gVfChecksumPublicationRouteReady=true,gVfChecksumNativeBindingsQualified=true,gVfCaptureStreamResetBound=true,gVfMediaPublicationAbiVerified=true;
    uintptr_t gVfChecksumMainVtable=0x9000,gVfMediaEndCaller=123,fakeCaller=123;
    alignas(16) unsigned char context[0x1400],shared[0xa0],descriptor[0x80],accel[16],task[16],gpuTask[16];
    void*gVfAccelerator=accel;
    NgreenAvcGen12::PublishScratch scratch;
    struct VfMediaSubmission {
     void*thread;void*context;void*accelerator;void*task;void*shared;void*gpuTask;NgreenAvcGen12::PublishScratch*scratch;
     NgreenChecksumGen12::NativeHeapBindings checksumHeaps;bool translating,failed,captureCancelled;
    } entry;
    bool present=true;
    struct VfMediaSubmissionScope {
    '''
    prefix+=scope+r'''
     static VfMediaSubmission* find(void*p){return present && p==entry.context?&entry:nullptr;}
    };
    struct VfMediaMappedSpan {
     void*resource,*mapped,*map,*descriptor,*gpuMap;uint64_t gpuBase,backingBytes;volatile uint32_t*commands;
     uint32_t bytes,offset,used;size_t availableWords,reservedWords;const char*reason;uint64_t mapLength;
     uint32_t classes,recordFlags,type,mapFlags,resourceBytes,count;
    };
    alignas(16) uint32_t commands[1024];bool mapping=true;unsigned captureCalls=0;int mode=0;
    bool vfCaptureMediaMappedSpan(const VfMediaSubmission&,void*s,VfMediaMappedSpan&out){
     ++captureCalls;
     if(!mapping || getMember<uint8_t>(s,0x3c)!=1)return false;
     if(mode==18 && captureCalls==2)entry.checksumHeaps.pitch[0]++;
     if(mode==20 && commands[0]==0)return false; // identity lost AFTER actual CPU writes
     out={reinterpret_cast<void*>(0x1000),reinterpret_cast<void*>(0x2000),reinterpret_cast<void*>(0x3000),
     reinterpret_cast<void*>(0x4000),reinterpret_cast<void*>(0x5000),0x60000000,4096,commands,4096,128,1024,992,224,"admitted",4096,1,0,10,0,4096,getMember<uint32_t>(s,0x34)};
     if(mode==19 && captureCalls>=2)out.resource=reinterpret_cast<void*>(0x8000);
     return true;
    }
    unsigned nativeCalls=0;
    void nativeImpl(void*that,void*stream){assert(that==context && stream==context+0x650);++nativeCalls;}
    #define FunctionCast(fn,addr) nativeImpl
    void OSSynchronizeIO(){if(mode==21)commands[0]^=1;}
    struct Gen11 {static Gen11*callback;uintptr_t oVfChecksumEnd=1;static void vfChecksumEnd(void*,void*);};
    Gen11 instance;Gen11*Gen11::callback=&instance;
    '''
    post='static const uint32_t captured[]={'+','.join(hex(w) for w in words)+'};\n'
    post+=r'''
    void setup(){
     memset(context,0,sizeof(context));memset(shared,0,sizeof(shared));memset(descriptor,0,sizeof(descriptor));
     entry={current_thread(),context,accel,task,shared,gpuTask,&scratch,{},false,false,false};entry.checksumHeaps.reset();
     memcpy(commands,captured,sizeof(captured));assert(NgreenChecksumGen12::writeReservationTrailer(commands,78));commands[78]=0x1234;commands[79]=0x5678;
     uint64_t gpu[]={uint64_t(captured[16]&~1U)|(uint64_t(captured[17])<<32),uint64_t(captured[20]&~1U)|(uint64_t(captured[21])<<32),uint64_t(captured[14]&~1U)|(uint64_t(captured[15])<<32)};
     for(unsigned i=0;i<3;++i)assert(entry.checksumHeaps.record(i,true,0x10000+i*4096,gpu[i],4096,false,reinterpret_cast<uintptr_t>(task),reinterpret_cast<uintptr_t>(shared)));
     getMember<uintptr_t>(context,0)=gVfChecksumMainVtable;getMember<void*>(context,0x5a8)=accel;
     getMember<void*>(context,0x578)=task;getMember<void*>(context,0x5a0)=shared;
     getMember<void*>(shared,0x78)=accel;getMember<void*>(shared,0x80)=task;getMember<void*>(shared,0x88)=gpuTask;
     getMember<void*>(context,0x618)=descriptor;getMember<uint8_t>(descriptor,0x68)=1;
     getMember<uint32_t>(context,0x660)=4096;getMember<uint32_t>(context,0x6f4)=1024;
     getMember<uint32_t>(context,0x6dc)=1;getMember<void*>(context,0xfe0)=commands;getMember<void*>(context,0x708)=commands;
     void*s=context+0x650;getMember<uint32_t>(s,0x30)=128;getMember<uint32_t>(s,0x34)=80;
     getMember<uint32_t>(s,0x38)=36;getMember<uint8_t>(s,0x3c)=1;
     present=mapping=args=true;gVfChecksumPublicationRouteReady=gVfChecksumNativeBindingsQualified=gVfCaptureStreamResetBound=gVfMediaPublicationAbiVerified=true;
     gVfIdentity=VfIdentity::Virtual;gVfAccelerator=accel;fakeCaller=123;captureCalls=nativeCalls=0;
    }
    int main(){setbuf(stdout,nullptr);for(mode=0;mode<40;++mode){setup();void*s=context+0x650;
     switch(mode){
     case 1:gVfChecksumPublicationRouteReady=false;break;
     case 2:gVfIdentity=VfIdentity::Physical;break;
     case 3:gVfAccelerator=task;break;
     case 4:args=false;break;
     case 5:present=false;break;
     case 6:fakeCaller=124;break;
     case 7:entry.failed=true;break;
     case 8:entry.checksumHeaps.reset();break;
     case 9:getMember<uintptr_t>(context,0)=0x9100;break;
     case 10:mapping=false;break;
     case 11:entry.checksumHeaps.failed=true;break;
     case 12:getMember<void*>(context,0x618)=nullptr;break;
     case 13:getMember<uint8_t>(descriptor,0x68)=0;break;
     case 14:getMember<uint32_t>(context,0x1304)=1;break;
     case 15:getMember<uint32_t>(s,0x34)=74;break;
     case 16:commands[72]^=1;break;
     case 17:commands[10]^=1;break;
     case 22:entry.failed=true;getMember<uint32_t>(s,0x34)=0;break;
     case 23:entry.checksumHeaps.reset();getMember<uint32_t>(s,0x34)=0;break;
     case 24:getMember<uint8_t>(s,0x3c)=0;break;
     case 25:mapping=false;getMember<uint32_t>(context,0x6f4)=100;break;
     case 26:getMember<uint32_t>(s,0x34)=2000;break;
     case 27:entry.translating=true;break;
     case 28:mapping=false;getMember<uint32_t>(context,0x660)=0;break;
     case 29:mapping=false;getMember<uint32_t>(context,0x660)=65540;break;
     case 30:mapping=false;getMember<uint32_t>(context,0x6f4)=4100;break;
     case 31:mapping=false;getMember<uint32_t>(s,0x30)=129;break;
     case 32:mapping=false;getMember<uint32_t>(s,0x30)=1028;break;
     case 33:mapping=false;getMember<uint32_t>(context,0x6f4)=132;break;
     case 34:mapping=false;getMember<uint8_t>(s,0x3c)=2;break;
     case 35:mapping=false;getMember<void*>(context,0xfe0)=nullptr;break;
     case 36:mapping=false;getMember<void*>(context,0x708)=context;break;
     case 37:mapping=false;getMember<uint32_t>(context,0x6dc)=2;break;
     case 38:mapping=false;gVfCaptureStreamResetBound=false;break;
     case 39:gVfChecksumNativeBindingsQualified=false;break;
     }
     std::array<uint32_t,1024> before{};memcpy(before.data(),commands,sizeof(commands));
     bool panic=false;try{Gen11::vfChecksumEnd(context,s);}catch(Panic&){panic=true;}
     bool expectedPanic=mode==5 || mode==6 || mode==20 || mode==21 || mode==25 || mode==26 || mode==27 || (mode>=28 && mode<=38);
     assert(panic==expectedPanic);
     bool forwarded=mode==0 || (mode>=1 && mode<=4) || mode==8 || mode==23;
     if(nativeCalls!=unsigned(forwarded))std::fprintf(stderr,"mode=%d calls=%u expected=%u\n",mode,nativeCalls,unsigned(forwarded));assert(nativeCalls==unsigned(forwarded));
     if(mode==0){assert(commands[0]==0 && commands[10]==0x61010014);assert(commands[78]==0 && commands[79]==0);assert(!entry.failed && !entry.translating && entry.checksumHeaps.seen==0);}
     else if(!panic && !forwarded){assert(entry.failed);assert(!entry.captureCancelled && !entry.translating);assert(getMember<uint32_t>(s,0x34)==0 && getMember<uint8_t>(s,0x3c)==0);assert(memcmp(before.data(),commands,sizeof(commands))==0);}
     else if(mode!=20 && mode!=21)assert(memcmp(before.data(),commands,sizeof(commands))==0);
     }
     puts("Actual MainEnd body: 40 forward/publication/cancel/authority-loss cases PASS; native once/no submission on refusal");
    }
    '''
    p=root/'checksum_end_actual.cpp';p.write_text(prefix+body+post)
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(p),'-o',str(root/'checksum_end_actual')],check=True)
    subprocess.run([str(root/'checksum_end_actual')],check=True)

def test_user_adoption(root):
    source=(ROOT/'tools/media_adapter/adapter.cpp').read_text()
    start=source.index('            bool published=false;')
    finish=source.index('            if(!published)return 5;',start)+len('            if(!published)return 5;')
    body=source[start:finish]
    prefix=r'''
#include "tools/media_adapter/frame_checksum_gen12.hpp"
#include "NootedGreen/kern_vf_device_observation.hpp"
#include <vector>
#include <cassert>
#include <cstdio>
std::vector<unsigned> calls;bool ready=true,prepared=true,published=true;
bool writableDataMemory(uintptr_t,size_t){return true;}
bool ownedExecutableMemory(uintptr_t,size_t){return true;}
uint64_t dispatchOwnedChecksumSize(void*,const void*,uint32_t*,uint32_t*){return 0;}
uint64_t dispatchOwnedChecksumBuilder(void*,const void*,void*){return 0;}
uint64_t dispatchOwnedAvcSize(void*,const void*,uint32_t*,uint32_t*){return 0;}
uint64_t dispatchOwnedAvcBuilder(void*,const void*,void*){return 0;}
struct Owner {
 template<class L,class S,class P>bool prepareChecksumProgram(L&,S,P){calls.push_back(2);return prepared;}
 template<class L,class S,class X>bool publishChecksumInputCallbacks(L&,uintptr_t,uintptr_t,uintptr_t,S,X){calls.push_back(3);return published;}
 template<class L,class S,class X>bool publishAvcInputCallbacks(L&,uintptr_t,uintptr_t,uintptr_t,S,X){calls.push_back(4);return published;}
 template<class L,class S,class X>bool publishFrame(L&,uintptr_t,S,X){calls.push_back(5);return published;}
} owner;
struct Lease{Owner*get(){return &owner;}} lease;
struct Scope{uintptr_t image(){return 0x1000000;}} scope;
struct Binding{uintptr_t vtable,frame;} binding;
bool queryChecksumPublication(Lease&,Scope&,uintptr_t,NGVfChecksumPublication::Reply&){calls.push_back(1);return ready;}
int adopt(){uintptr_t hook=1,physical=2;auto sameOwner=[](uintptr_t,uintptr_t,uint64_t,uintptr_t){return true;};
'''
    suffix=r'''
return 0;}
int main(){
 for(unsigned mode=0;mode<6;++mode){
  calls.clear();ready=prepared=published=true;
  binding={scope.image()+0x754408,scope.image()+0x2190a};
  if(mode==1)ready=false;if(mode==2)prepared=false;if(mode==3)published=false;
  if(mode==4)binding.vtable=scope.image()+0x754350; // Shared frame entry is NOT the checksum class.
  if(mode==5)binding={scope.image()+0x755000,scope.image()+0x27724};
  const auto result=adopt();
  assert(result==((mode>=1 && mode<=3)?5:0));
  const std::vector<unsigned> expected=mode==1?std::vector<unsigned>{1}:mode==2?std::vector<unsigned>{1,2}:
   mode==4?std::vector<unsigned>{5}:mode==5?std::vector<unsigned>{4}:std::vector<unsigned>{1,2,3};
  assert(calls==expected);
 }
 puts("Actual checksum adoption branch: independent capability before program before publication, 6 cases PASS; native invocation stubbed");
}
'''
    path=root/'checksum_adoption_actual.cpp';path.write_text(prefix+body+suffix)
    subprocess.run([*CXX,'-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(path),'-o',str(root/'checksum_adoption_actual')],check=True)
    subprocess.run([str(root/'checksum_adoption_actual')],check=True)

if __name__=="__main__":
    with tempfile.TemporaryDirectory(prefix="ngreen-checksum-main-end-") as directory:
        destination=Path(directory)
        test_qualification(destination)
        test_end(destination)
        test_user_adoption(destination)
