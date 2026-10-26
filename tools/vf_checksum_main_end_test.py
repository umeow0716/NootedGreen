#!/usr/bin/env python3
"""Compile actual checksum MainEnd/qualification blocks against owned mocks.

No VM, MMIO, native resource invocation or GPU execution. Fixed native image
reads are limited to the thunk, vtable slot and FIFO END-mode byte windows.
Readiness derives from qualified installed endpoints; CPU mocks grant no GPU proof.
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
     import re
     for name,offset,count in [('surfaceOutputs',0x1c8,12),('surfaceCall',0x1d4,7)]:
      literal=re.search(r'static constexpr uint8_t '+name+r'\[\]=\{([^}]+)\}',s)
      assert literal, name
      expected=bytes(int(x.strip(),0) for x in literal.group(1).split(','))
      actual=read(0x36602+offset,count)
      assert len(expected)==count and expected==actual, name
     literal=re.search(r'static constexpr uint8_t unmapPrefix\[\]=\{([^}]+)\}',s)
     expected=bytes(int(x.strip(),0) for x in literal.group(1).split(','))
     assert len(expected)==15 and expected==read(0x77e92,15)
     call=read(0x36602+0x243,5)
     assert call[0]==0xe8 and 0x36602+0x248+struct.unpack_from('<i',call,1)[0]==0x77e92
     assert 'gVfChecksumSshUnmapCaller=mediaKernel+0x248;' in s
     assert 'gVfChecksumSurfaceBindCaller=mediaKernel+0x1db;' in s
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

def test_readiness(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    start=source.index('gVfChecksumPublicationRouteReady=checksumEndQualified')
    install=source[start:source.index(';',start)+1].replace('this->','owner.')
    start=source.index('static IOReturn vfReadOwnedChecksumPublication(')
    start=source.index('\tif (gVfIdentity==VfIdentity::Virtual',start)
    token='reply.flags=NGVfChecksumPublication::routeReady;'
    query=source[start:source.index(token,start)+len(token)]
    prefix=r'''#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include "NootedGreen/kern_vf_device_observation.hpp"
using UInt32=uint32_t;
enum class VfIdentity {Virtual,Physical};
bool checksumEndQualified=true,gVfChecksumPublicationRouteReady=true,gVfChecksumNativeBindingsQualified=true;
bool gVfMediaPublicationRouteReady=true,gVfMediaPublicationAbiVerified=true,gVfCaptureStreamResetBound=true;
bool gVfDeviceStopping=false,gVfSubmissionStopped=false,gVfProtocolFault=false,mediaOpt=true,genOpt=true;
uintptr_t mainTable=0x10000000,videoEnd=0x20000000,gVfChecksumMainVtable=mainTable+16;
struct Owner {uintptr_t oVfChecksumEnd=videoEnd;}owner;
VfIdentity gVfIdentity=VfIdentity::Virtual;
alignas(16) std::array<uint8_t,0xdd0> native{},foreign{};
void*gVfAccelerator=native.data();
template<class T>T&getMember(void*p,size_t offset){return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+offset);}
bool checkKernelArgument(const char*text){return !strcmp(text,"-ngreenmediaadapter")?mediaOpt:!strcmp(text,"-ngreenavcgen12")?genOpt:false;}
bool installed(){
'''
    middle='\nreturn gVfChecksumPublicationRouteReady;\n}\nbool advertised(void*accelerator){NGVfChecksumPublication::Reply reply{};\n'
    suffix=r'''
return reply.flags==NGVfChecksumPublication::routeReady;
}
int main(){
 for(unsigned mode=0;mode<8;++mode){
  checksumEndQualified=gVfChecksumNativeBindingsQualified=true;gVfChecksumMainVtable=mainTable+16;owner.oVfChecksumEnd=videoEnd;
  if(mode==1)checksumEndQualified=false;if(mode==2)gVfChecksumNativeBindingsQualified=false;
  if(mode==3)gVfChecksumMainVtable=0;if(mode==4)gVfChecksumMainVtable=mainTable+8;
  if(mode==5)owner.oVfChecksumEnd=0;if(mode==6)owner.oVfChecksumEnd=videoEnd+8;
  if(mode==7)gVfChecksumMainVtable=mainTable;
  gVfChecksumPublicationRouteReady=true;assert(installed()==(mode==0));
 }
 for(unsigned mode=0;mode<15;++mode){
  gVfChecksumPublicationRouteReady=gVfChecksumNativeBindingsQualified=gVfMediaPublicationRouteReady=true;
  gVfMediaPublicationAbiVerified=gVfCaptureStreamResetBound=mediaOpt=genOpt=true;
  gVfChecksumMainVtable=mainTable+16;gVfIdentity=VfIdentity::Virtual;
  gVfDeviceStopping=gVfSubmissionStopped=gVfProtocolFault=false;
  getMember<UInt32>(native.data(),0xdc8)=0;void*accelerator=native.data();
  if(mode==1)gVfChecksumPublicationRouteReady=false;if(mode==2)gVfChecksumNativeBindingsQualified=false;
  if(mode==3)gVfChecksumMainVtable=0;if(mode==4)gVfMediaPublicationRouteReady=false;
  if(mode==5)gVfMediaPublicationAbiVerified=false;if(mode==6)gVfCaptureStreamResetBound=false;
  if(mode==7)gVfIdentity=VfIdentity::Physical;if(mode==8)accelerator=foreign.data();
  if(mode==9)mediaOpt=false;if(mode==10)genOpt=false;if(mode==11)gVfDeviceStopping=true;
  if(mode==12)gVfSubmissionStopped=true;if(mode==13)gVfProtocolFault=true;
  if(mode==14)getMember<UInt32>(native.data(),0xdc8)=1;
  assert(advertised(accelerator)==(mode==0));
 }
 puts("Actual checksum installed/runtime capability gates PASS; CPU-only");
}
'''
    path=root/'checksum_readiness_actual.cpp';path.write_text(prefix+install+middle+query+suffix)
    binary=root/'checksum_readiness_actual'
    subprocess.run([*CXX,'-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_end(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    a=source.index('void Gen11::vfChecksumEnd(');b=source.index('\nvoid Gen11::vfMediaEnd(',a);body=source[a:b].replace('reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0))','fakeCaller')
    a=source.index('\tstatic bool bindLocked(');b=source.index('    static VfMediaSubmission *findCurrent(',a);scope=source[a:b]
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
    bool args=true,checksumCapture=false;bool checkKernelArgument(const char*flag){return !strcmp(flag,"-ngreenchecksumcapture")?checksumCapture:args;}
    void* current_thread(){return reinterpret_cast<void*>(0x4000);}
    enum class VfIdentity {Virtual,Physical};VfIdentity gVfIdentity=VfIdentity::Virtual;
    bool gVfChecksumPublicationRouteReady=true,gVfChecksumNativeBindingsQualified=true,gVfCaptureStreamResetBound=true,gVfMediaPublicationAbiVerified=true;
    uintptr_t gVfChecksumMainVtable=0x9000,gVfMediaEndCaller=123,fakeCaller=123;
    alignas(16) unsigned char context[0x1400],shared[0xa0],descriptor[0x80],accel[16],task[16],gpuTask[16];
    void*gVfAccelerator=accel;
    NgreenAvcGen12::PublishScratch scratch;
    struct VfMediaSubmission {
     void*thread;void*context;void*accelerator;void*task;void*shared;void*gpuTask;NgreenAvcGen12::PublishScratch*scratch;
     NgreenChecksumGen12::NativeHeapBindings checksumHeaps;NgreenChecksumGen12::NativeSurfaceBindings checksumSurfaces;bool translating,failed,captureCancelled;
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
     if(captureCalls==2){
      if(mode==49)entry.checksumSurfaces.gpu[0]+=4096;
      if(mode==50)++entry.checksumSurfaces.identifier[0];
      if(mode==51)++entry.checksumSurfaces.pitch[0];
      if(mode==52)entry.checksumSurfaces.writeUse[0]^=1;
      if(mode==53)entry.checksumSurfaces.resources[0]+=4096;
     }
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
     entry={current_thread(),context,accel,task,shared,gpuTask,&scratch,{},{},false,false,false};entry.checksumHeaps.reset();
     memcpy(commands,captured,sizeof(captured));assert(NgreenChecksumGen12::writeReservationTrailer(commands,78));commands[78]=0x1234;commands[79]=0x5678;
     uint64_t gpu[]={uint64_t(captured[16]&~1U)|(uint64_t(captured[17])<<32),uint64_t(captured[20]&~1U)|(uint64_t(captured[21])<<32),uint64_t(captured[14]&~1U)|(uint64_t(captured[15])<<32)};
     for(unsigned i=0;i<3;++i)assert(entry.checksumHeaps.record(i,true,0x10000+i*4096,gpu[i],4096,false,reinterpret_cast<uintptr_t>(task),reinterpret_cast<uintptr_t>(shared)));
     for(unsigned i=0;i<3;++i)assert(entry.checksumSurfaces.record(true,0x50000+i*4096,0x70000000+i*4096,1920,10+i,false));
     getMember<uintptr_t>(context,0)=gVfChecksumMainVtable;getMember<void*>(context,0x5a8)=accel;
     getMember<void*>(context,0x578)=task;getMember<void*>(context,0x5a0)=shared;
     getMember<void*>(shared,0x78)=accel;getMember<void*>(shared,0x80)=task;getMember<void*>(shared,0x88)=gpuTask;
     getMember<void*>(context,0x618)=descriptor;getMember<uint8_t>(descriptor,0x68)=1;
     getMember<uint32_t>(context,0x660)=4096;getMember<uint32_t>(context,0x6f4)=1024;
     getMember<uint32_t>(context,0x6dc)=1;getMember<void*>(context,0xfe0)=commands;getMember<void*>(context,0x708)=commands;
     void*s=context+0x650;getMember<uint32_t>(s,0x30)=128;getMember<uint32_t>(s,0x34)=80;
     getMember<uint32_t>(s,0x38)=36;getMember<uint8_t>(s,0x3c)=1;
     checksumCapture=false;present=mapping=args=true;gVfChecksumPublicationRouteReady=gVfChecksumNativeBindingsQualified=gVfCaptureStreamResetBound=gVfMediaPublicationAbiVerified=true;
     gVfIdentity=VfIdentity::Virtual;gVfAccelerator=accel;fakeCaller=123;captureCalls=nativeCalls=0;
    }
    int main(){setbuf(stdout,nullptr);for(mode=0;mode<56;++mode){setup();void*s=context+0x650;
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
     case 40:entry.checksumSurfaces.reset();break;
     case 41:entry.checksumSurfaces.gpu[0]+=32;break;
     case 42:entry.checksumSurfaces.gpu[1]+=32;break;
     case 43:entry.checksumSurfaces.gpu[2]+=1;break;
     case 44:entry.checksumSurfaces.failed=true;break;
     case 45:entry.checksumSurfaces.count=2;break;
     case 46:entry.checksumSurfaces.resources[2]=0;break;
     case 47:entry.checksumSurfaces.gpu[0]=UINT64_C(0x1000000000000);break;
     case 54:checksumCapture=true;break;
     case 55:checksumCapture=true;entry.checksumSurfaces.gpu[0]+=32;break;
     case 48:entry.checksumSurfaces.gpu[2]+=4;break; // buffer is element-, not tile-aligned

     }
     std::array<uint32_t,1024> before{};memcpy(before.data(),commands,sizeof(commands));
     bool panic=false;try{Gen11::vfChecksumEnd(context,s);}catch(Panic&){panic=true;}
     bool expectedPanic=mode==5 || mode==6 || mode==20 || mode==21 || mode==25 || mode==26 || mode==27 || (mode>=28 && mode<=38);
     assert(panic==expectedPanic);
     bool forwarded=mode==0 || mode==48 || (mode>=1 && mode<=4) || mode==8 || mode==23;
     if(nativeCalls!=unsigned(forwarded))std::fprintf(stderr,"mode=%d calls=%u expected=%u\n",mode,nativeCalls,unsigned(forwarded));assert(nativeCalls==unsigned(forwarded));
     if(mode==0 || mode==48){assert(commands[0]==0 && commands[10]==0x61010014);assert(commands[78]==0 && commands[79]==0);assert(!entry.failed && !entry.translating && entry.checksumHeaps.seen==0);}
     else if(!panic && !forwarded){assert(entry.failed);assert(!entry.captureCancelled && !entry.translating);assert(getMember<uint32_t>(s,0x34)==0 && getMember<uint8_t>(s,0x3c)==0);assert(memcmp(before.data(),commands,sizeof(commands))==0);}
     else if(mode!=20 && mode!=21)assert(memcmp(before.data(),commands,sizeof(commands))==0);
     }
     puts("Actual MainEnd body: 56 forward/publication/cancel/authority-loss cases PASS; native once/no submission on refusal");
    }
    '''
    p=root/'checksum_end_actual.cpp';p.write_text(prefix+body+post)
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(p),'-o',str(root/'checksum_end_actual')],check=True)
    subprocess.run([str(root/'checksum_end_actual')],check=True)

def test_surface_bind_returns(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    a=source.index('bool Gen11::vfChecksumBindResource(')
    b=source.index('\ntemplate<class Live>\nstatic void vfObserveChecksumPreparedResources(',a)
    body=source[a:b].replace('reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0))','fakeCaller')
    prefix=r'''
void vfObserveChecksumHeapBinding(unsigned,unsigned,bool,unsigned long,unsigned) {}

#include "NootedGreen/kern_checksum_gen12_commands.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
using mach_vm_address_t=uintptr_t;
template<class T>T&getMember(void*p,size_t o){return *reinterpret_cast<T*>(static_cast<unsigned char*>(p)+o);}
#define PANIC_COND(c,t,m) assert(!(c))
template<class...A>void logOnly(const char*,const char*,A...){++logs;}
unsigned logs=0;
#define SYSLOG logOnly
enum class VfIdentity{Virtual,Physical};VfIdentity gVfIdentity=VfIdentity::Virtual;
bool gVfChecksumNativeBindingsQualified=true,present=true,returned=true;
uintptr_t gVfChecksumBindCallers[3]={10,20,30},gVfChecksumSurfaceBindCaller=40,fakeCaller=40;
volatile uint32_t gVfChecksumSurfaceBindLogCount=0;
bool OSCompareAndSwap(uint32_t a,uint32_t b,volatile uint32_t*p){if(*p!=a)return false;*p=b;return true;}
alignas(16) unsigned char context[0x700],accel[8],foreign[8];void*gVfAccelerator=accel;
struct Entry{bool translating=false;void*accelerator=accel;NgreenChecksumGen12::NativeHeapBindings checksumHeaps{};NgreenChecksumGen12::NativeSurfaceBindings checksumSurfaces{};} entry;
struct VfMediaSubmissionScope{static Entry*find(void*p){return present&&p==context?&entry:nullptr;}};
uint64_t nextGpu;uintptr_t nextResource;uint32_t nextPitch=1920;unsigned nativeCalls=0;
bool nativeImpl(void*,void*,uint32_t,void**r,uint64_t*g,uint32_t*p,bool){++nativeCalls;if(r)*r=reinterpret_cast<void*>(nextResource);if(g)*g=nextGpu;if(p)*p=nextPitch;return returned;}
#define FunctionCast(fn,addr) nativeImpl
struct Gen11{static Gen11*callback;uintptr_t oVfChecksumBindResource=1;static bool vfChecksumBindResource(void*,void*,uint32_t,void**,uint64_t*,uint32_t*,bool);};
Gen11 instance;Gen11*Gen11::callback=&instance;
'''
    # Declaration order matters for the real production SYSLOG instantiation.
    prefix=prefix.replace('template<class...A>void logOnly','extern unsigned logs;\ntemplate<class...A>void logOnly')
    suffix=r'''
int main(){
 for(unsigned mode=0;mode<19;++mode){
  entry={};present=returned=gVfChecksumNativeBindingsQualified=true;gVfIdentity=VfIdentity::Virtual;
  fakeCaller=40;logs=nativeCalls=gVfChecksumSurfaceBindLogCount=0;
  getMember<void*>(context,0x5a8)=accel;
  if(mode==1)fakeCaller=41;if(mode==2)entry.translating=true;if(mode==3)entry.accelerator=foreign;
  if(mode==4)present=false;if(mode==5)returned=false;if(mode==6)gVfChecksumNativeBindingsQualified=false;
  if(mode==7)gVfIdentity=VfIdentity::Physical;
  for(unsigned i=0;i<3;++i){
   nextGpu=0x50000000+i*4096;nextResource=0x1000+i*4096;
   if(mode==8&&i==0)nextGpu+=32;if(mode==9&&i==1)nextGpu+=32;
   if(mode==10&&i==2)++nextGpu;if(mode==11&&i==2)nextGpu+=4;
   if(mode==12&&i==1)nextResource=0;if(mode==13&&i==1)++nextResource;
   if(mode==14&&i==1)nextGpu=0;if(mode==15&&i==1)nextGpu=UINT64_C(0x1000000000000);
   void*r=nullptr;uint64_t gpu=0;uint32_t pitch=0;
   const bool native=Gen11::vfChecksumBindResource(context,mode==16?context:context+0x650,10+i,
       &r,mode==17?nullptr:&gpu,&pitch,mode==18);
   assert(native==returned);
  }
  assert(nativeCalls==3);
  assert(entry.checksumSurfaces.admitted()==(mode==0||mode==11||mode==18));
  if(mode==1||mode==4||mode==6||mode==7)assert(entry.checksumSurfaces.count==0);
 }
 entry={};present=returned=gVfChecksumNativeBindingsQualified=true;gVfIdentity=VfIdentity::Virtual;
 fakeCaller=40;gVfChecksumSurfaceBindLogCount=logs=0;getMember<void*>(context,0x5a8)=accel;
 for(unsigned n=0;n<100;++n){entry.checksumSurfaces.reset();nextGpu=0x50000000;nextResource=0x1000;
  void*r;uint64_t gpu;uint32_t pitch;
  assert(Gen11::vfChecksumBindResource(context,context+0x650,10,&r,&gpu,&pitch,false));}
 assert(logs==24 && gVfChecksumSurfaceBindLogCount==24);
 puts("Actual native surface bind callback: 19 cases/native-once/unchanged result/no pointer-follow/24-log bound PASS");
}
'''
    path=root/'checksum_surface_bind_actual.cpp';path.write_text(prefix+body+suffix)
    subprocess.run([*CXX,'-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT),str(path),'-o',str(root/'checksum_surface_bind_actual')],check=True)
    subprocess.run([str(root/'checksum_surface_bind_actual')],check=True)


def test_checksum_capture_arm(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    start=source.index('                if(checkKernelArgument("-ngreenchecksumcapture"))')
    end=source.index(';',source.index('SYSLOG(',start))+1
    body=source[start:end]
    assert len(body.encode())<1024
    code=r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
bool enabled=true,early=false,gVfChecksumPublicationRouteReady=true,gVfCaptureStreamResetBound=true;
uintptr_t gVfChecksumSurfaceBindCaller=1;int observed=-1;
bool checkKernelArgument(const char*s){return !strcmp(s,"-ngreenchecksumcapture")?enabled:early;}
#define SYSLOG(module,format,value) observed=(value)
void check(){observed=-1;
'''.replace('#include <cassert>','#include <cassert>\n#include <cstdint>')+body+r'''
}
int main(){check();assert(observed==1);
 for(bool* gate:{&gVfChecksumPublicationRouteReady,&gVfCaptureStreamResetBound}){
  *gate=false;check();assert(observed==0);*gate=true;}
 gVfChecksumSurfaceBindCaller=0;check();assert(observed==0);gVfChecksumSurfaceBindCaller=1;
 early=true;check();assert(observed==0);early=false;enabled=false;check();assert(observed==-1);
}
'''
    path=root/'checksum_capture_arm_actual.cpp';path.write_text(code)
    subprocess.run([*CXX,'-std=c++17','-fsanitize=address,undefined',str(path),'-o',str(root/'checksum_capture_arm_actual')],check=True)
    subprocess.run([str(root/'checksum_capture_arm_actual')],check=True)


def test_complete_heap_arm(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    marker=source.index('V393: complete checksum CPU observation armed=')
    a=source.rfind('                if(checkKernelArgument(',0,marker);b=source.index(';',marker)+1
    body=source[a:b]
    prefix=r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
bool enabled=true,early=false,gVfChecksumPublicationRouteReady=true,gVfCaptureStreamResetBound=true;
uintptr_t gVfChecksumSurfaceBindCaller=1,gVfChecksumSshUnmapCaller=1,gVfChecksumMapAddressGetter=1,gVfChecksumMapLengthGetter=1,gVfBatchCpuCopy=1;
uintptr_t pmap=0x100,gVfCpuKernelPmapStore=0x100,*gVfCpuKernelPmapSlot=&pmap;int observed=-1;
bool checkKernelArgument(const char*s){return !strcmp(s,"-ngreenchecksumcapture")?enabled:early;}
#define SYSLOG(module,format,value) observed=(value)
void check(){observed=-1;
"""
    suffix=r"""
}
int main(){
 check();assert(observed==1);
 for(bool* value:{&gVfChecksumPublicationRouteReady,&gVfCaptureStreamResetBound}) {
  *value=false;check();assert(observed==0);*value=true;
 }
 for(uintptr_t* value:{&gVfChecksumSurfaceBindCaller,&gVfChecksumSshUnmapCaller,&gVfChecksumMapAddressGetter,&gVfChecksumMapLengthGetter,&gVfBatchCpuCopy}) {
  *value=0;check();assert(observed==0);*value=1;
 }
 gVfCpuKernelPmapSlot=nullptr;check();assert(observed==0);gVfCpuKernelPmapSlot=&pmap;
 pmap=0;check();assert(observed==0);pmap=gVfCpuKernelPmapStore;
 early=true;check();assert(observed==0);early=false;enabled=false;check();assert(observed==-1);
}
"""
    path=root/'checksum_complete_arm.cpp';path.write_text(prefix+body+suffix)
    binary=root/'checksum_complete_arm'
    subprocess.run([*CXX,'-std=c++17','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


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

def test_ssh_unmap(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    a=source.index('void Gen11::vfChecksumUnmap(');b=source.index('\nvoid Gen11::vfChecksumEnd(',a)
    body=source[a:b].replace('reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0))','fakeCaller')
    q=source.index('                    const auto pureGetter=')
    qualifier=source[q:source.index('                    const bool qualifiedMap=',q)]
    prefix=r'''
#include <cassert>
#include <initializer_list>
#include <cstdint>
#include <cstring>
#include "NootedGreen/kern_checksum_gen12_commands.hpp"
using mach_vm_address_t=uintptr_t;
template<class T> T& getMember(void*p,size_t n){return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+n);}
#define PANIC_COND(c,...) do{if(c)throw 1;}while(0)
#define SYSLOG(...) do{}while(0)
#define FunctionCast(f,p) reinterpret_cast<decltype(&Gen11::f)>(p)
bool opt=true,flight=true,surface=true,released=false,present=true;unsigned nativeCalls=0,records=0,copies=0,mode=0;
bool checkKernelArgument(const char*){return opt;}bool vfFlightEnabled(){return flight;}
uintptr_t fakeCaller=0x5550,gVfChecksumSshUnmapCaller=0x5550,gVfChecksumMainVtable=0x8880;
uintptr_t gVfChecksumMapAddressGetter=0x1110,gVfChecksumMapLengthGetter=0x2220,gVfBatchCpuCopy=0;
uintptr_t pmap=0x9990,gVfCpuKernelPmapStore=0x9990;uintptr_t*gVfCpuKernelPmapSlot=&pmap;
volatile uint32_t gVfChecksumSshCaptureCount=0;
bool OSCompareAndSwap(uint32_t a,uint32_t b,volatile uint32_t*p){if(*p!=a)return false;*p=b;return true;}
alignas(16) uint8_t context[0x600]{};int accelerator=0;void*gVfAccelerator=&accelerator;
struct VfMediaSubmission {
 bool failed=false,translating=false;void*accelerator=&::accelerator;void*context=::context;uint32_t flightGroup=4;
 struct{unsigned seen=7;bool failed=false;uint64_t gpu[3]={0,0,0x444000};}checksumHeaps;
 struct{bool admitted(){return surface;}}checksumSurfaces;
}entry;
struct VfMediaSubmissionScope{static VfMediaSubmission*findCurrent(){return present?&entry:nullptr;}};
template<class Live>static void vfObserveChecksumPreparedResources(VfMediaSubmission&,const NgreenChecksumGen12::NativeSshMapPagePair&,uint32_t,Live&&) {}
NgreenChecksumGen12::NativeSshMapPair saved;
NgreenChecksumGen12::NativeSshMapPagePair gVfChecksumSshPagePairs[2];
void vfFlightRecord(uint32_t kind,uint32_t group,uint64_t id,const volatile uint32_t*data,uint32_t count){
 assert(!released && kind==30 && group==4 && id==0x444000 && count==8);++records;
 auto*out=reinterpret_cast<uint32_t*>(&saved);for(unsigned i=0;i<count;++i)out[i]=data[i];
}
void vfFlightChecksumPageChunk(uint32_t group,uint64_t id,uint32_t ticket,uint32_t part,uint32_t flags,const uint32_t*data){
 assert(!released && group==4 && id==0x444000 && ticket==1 && part<2 && (flags&1));++records;
 for(unsigned i=0;i<1024;++i)assert(data[i]==(i^((mode==18 && part==1 && i==143)?1U:0U)));
}
constexpr uintptr_t slot=0xffff800000001000ULL,map=slot+0x1000,table=slot+0x2000,cpu=slot+0x3000;
uint64_t nofault(uint64_t at,uint64_t output,uint64_t bytes){
 assert(!released);auto*out=reinterpret_cast<void*>(output);uint64_t value=0;
 if(at==slot)value=map;else if(at==map)value=table;
 else if(at==table+0x118)value=mode==16?0x3330:0x1110;
 else if(at==table+0x128)value=0x2220;else if(at==map+0x28)value=cpu;else if(at==map+0x30)value=4096;
 else if(at==cpu && bytes==4096){++copies;if(mode==17)return 0;auto*w=static_cast<uint32_t*>(out);
  for(unsigned i=0;i<1024;++i)w[i]=i;if(mode==18 && copies==2)w[143]^=1;return bytes;}
 else {assert(false);return 0;}
 assert(bytes==8);std::memcpy(out,&value,8);return 8;
}
void original(void**p){assert(p==reinterpret_cast<void**>(slot));assert(!released);++nativeCalls;released=true;}
struct Gen11{static Gen11*callback;uintptr_t oVfChecksumUnmap=0;static void vfChecksumUnmap(void**);};
Gen11 owner;Gen11*Gen11::callback=&owner;
'''
    suffix=r'''
int main(){
 for(unsigned offset: {0x28U,0x30U}) {
  const uint8_t expected[]={0x55,0x48,0x89,0xe5,0x48,0x8b,0x47,uint8_t(offset),0x5d,0xc3};
  uint8_t raw[10];std::memcpy(raw,expected,10);
  assert(pureGetter(reinterpret_cast<uintptr_t>(raw),expected,10));
  assert(!pureGetter(0,expected,10) && !pureGetter(reinterpret_cast<uintptr_t>(raw),expected,9));
  for(unsigned i=0;i<10;++i)for(unsigned bit=0;bit<8;++bit) {
   raw[i]^=1U<<bit;assert(!pureGetter(reinterpret_cast<uintptr_t>(raw),expected,10));raw[i]^=1U<<bit;
  }
 }
 for(mode=0;mode<20;++mode){
  opt=flight=surface=present=true;released=false;copies=nativeCalls=records=0;gVfChecksumSshCaptureCount=0;
  fakeCaller=0x5550;pmap=gVfCpuKernelPmapStore;gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(&nofault);
  owner.oVfChecksumUnmap=reinterpret_cast<uintptr_t>(&original);entry=VfMediaSubmission{};
  getMember<uintptr_t>(context,0)=gVfChecksumMainVtable;getMember<void*>(context,0x5a8)=gVfAccelerator;
  if(mode==1)fakeCaller=0;if(mode==2)opt=false;if(mode==3)flight=false;if(mode==4)gVfBatchCpuCopy=0;
  if(mode==5)pmap=0;if(mode==6)present=false;if(mode==7)entry.failed=true;if(mode==8)entry.translating=true;
  if(mode==9)entry.accelerator=nullptr;if(mode==10)entry.checksumHeaps.seen=6;if(mode==11)entry.checksumHeaps.failed=true;
  if(mode==12)surface=false;if(mode==13)getMember<uintptr_t>(context,0)=0;
  if(mode==14)getMember<void*>(context,0x5a8)=nullptr;if(mode==15)gVfChecksumSshCaptureCount=2;
  if(mode==19)owner.oVfChecksumUnmap=0;
  bool panic=false;try{Gen11::vfChecksumUnmap(reinterpret_cast<void**>(slot));}catch(int){panic=true;}
  assert(panic==(mode==19) && nativeCalls==(mode==19?0:1));
  assert(records==((mode==0 || mode==18)?3:(mode==16 || mode==17)?1:0));
  if(mode==0)assert(saved.flags==15 && copies==2);
  if(mode==16)assert(saved.flags==0 && copies==0);
  if(mode==17)assert(saved.flags==1 && copies==1);
  if(mode==18)assert(saved.flags==7 && copies==2);
 }
}
'''
    path=root/'checksum_unmap_actual.cpp';path.write_text(prefix+body+qualifier+suffix)
    executable=root/'checksum_unmap_actual'
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-I'+str(ROOT),str(path),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
    print('Actual native SSH unmap observer:20 cases/native-once/copy-before-release/short-read and changed pair PASS; CPU-only')

def test_observed_surface_ranges(root):
    fixture=r'''
#include "NootedGreen/kern_checksum_gen12_commands.hpp"
#include <cassert>
#include <cstdio>
using namespace NgreenChecksumGen12;
int main(){
 for(unsigned mode=0;mode<17;++mode){
  NativeSshMapPagePair pair{};pair.first=pair.second=4096;pair.flags=15;
  NativeSurfaceBindings bindings{};
  for(unsigned i=0;i<16;++i)pair.before[i]=i<8?64+i*64:0;
  for(unsigned i=0;i<3;++i){auto*s=pair.before+16+16*i;
   s[0]=i==0?0x25017000U:i==1?0x24297000U:0x835d4000U;
   s[1]=(i<2?4U:6U)<<24;s[7]=0x09770000;
   s[2]=i<2?479U|((i?543U:1087U)<<16):127U|(7U<<16);
   s[3]=i<2?1919:3;s[8]=i==0?0x40020000:i==1?0x4021e000:0x40a6f000;
   assert(bindings.record(true,0x1000+i*0x1000,s[8],i<2?1920:4096,i,i==2));
  }
  for(unsigned i=0;i<1024;++i)pair.after[i]=pair.before[i];
  if(mode==1)pair.version=1;if(mode==2)pair.first=4092;if(mode==3)pair.second=4092;
  if(mode==4)pair.flags=7;if(mode==5)pair.atomic=1;if(mode==6)bindings.failed=true;
  if(mode==7)pair.after[1023]=1;if(mode==8)bindings.gpu[0]+=4096;
  if(mode==9)pair.before[17]=pair.after[17]=0x4e000000;
  if(mode==10)bindings.pitch[0]=2048;if(mode==11)bindings.pitch[2]=4;
  if(mode==12)pair.before[18]=pair.after[18]=479|(1085U<<16);
  if(mode==13)pair.before[35]=pair.after[35]=2047;
  if(mode==14)pair.before[51]=pair.after[51]=3|(1U<<21);
  if(mode==15)pair.before[20]=pair.after[20]=1;
  if(mode==16){pair.before[18]=pair.after[18]=479|(1023U<<16);pair.before[34]=pair.after[34]=479|(511U<<16);}
  uint64_t sizes[3]={1,2,3};const bool ok=checksumObservedSurfaceRanges(pair,bindings,sizes);
  assert(ok==(mode==0 || mode==16));
  if(ok){assert(sizes[0]==1920U*(mode==16?1024:1088));assert(sizes[1]==sizes[0]/2 && sizes[2]==4096);}
  else assert(!sizes[0] && !sizes[1] && !sizes[2]);
 }std::puts("Observed final SSH ranges: two geometries and15 refusals PASS; original output bind4096, not BUFFER row pitch4");
}
'''
    path=root/'observed_ranges.cpp';path.write_text(fixture)
    binary=root/'observed_ranges'
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-I'+str(ROOT),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_prepared_capture_arm(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    marker=source.index('V394: checksum prepared CPU backing and task leaves armed=')
    start=source.rfind('                if(checkKernelArgument(',0,marker)
    end=source.index('\n                }',marker)+len('\n                }')
    body=source[start:end]
    fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "NootedGreen/kern_flight_trace.hpp"
enum class VfIdentity {Virtual,Physical};VfIdentity gVfIdentity=VfIdentity::Virtual;
bool enabled=true,early=false,lockAllowed=true,publishLock=true;
bool gVfChecksumPublicationRouteReady=true,gVfChecksumNativeBindingsQualified=true,gVfCaptureStreamResetBound=true;
bool gVfNativeObjectLayoutBound=true,gVfMediaMapOffsetLayoutVerified=true;
uintptr_t gVfChecksumSurfaceBindCaller=1,gVfChecksumSshUnmapCaller=1,gVfChecksumMapAddressGetter=1,gVfChecksumMapLengthGetter=1,gVfBatchCpuCopy=1;
uintptr_t pmap=1,gVfCpuKernelPmapStore=1,*gVfCpuKernelPmapSlot=&pmap;
uintptr_t mapper=0,*gVfCpuSystemMapperSlot=&mapper;
uintptr_t gVfNativeBatchMapVtable=1,gVfMediaSysMemoryVtable=1,gVfLeafObservationReader=1,gVfRootObservationVtable=1;
NGFlightTrace::PreparedPhysicalCacheAuthority gVfPreparedPhysicalCacheAuthority{1,1,true};
int lock=0,*gVfPageTableUpdateLock=nullptr,observed=-1;unsigned initializers=0;
bool vfEnsurePageTableUpdateLock(){++initializers;if(lockAllowed && publishLock)gVfPageTableUpdateLock=&lock;return lockAllowed;}
bool checkKernelArgument(const char*s){return !strcmp(s,"-ngreenchecksumcapture")?enabled:early;}
#define SYSLOG(module,format,value) observed=(value)
void check(){observed=-1;initializers=0;gVfPageTableUpdateLock=nullptr;__ACTUAL__}
int main(){check();assert(observed==1 && initializers==1);
 for(bool*value:{&gVfChecksumPublicationRouteReady,&gVfChecksumNativeBindingsQualified,&gVfCaptureStreamResetBound,
 &gVfNativeObjectLayoutBound,&gVfMediaMapOffsetLayoutVerified,&gVfPreparedPhysicalCacheAuthority.exactLayout}){
  *value=false;check();assert(observed==0 && !initializers);*value=true;}
 for(uintptr_t*value:{&gVfChecksumSurfaceBindCaller,&gVfChecksumSshUnmapCaller,&gVfChecksumMapAddressGetter,
 &gVfChecksumMapLengthGetter,&gVfBatchCpuCopy,&gVfNativeBatchMapVtable,&gVfMediaSysMemoryVtable,
 &gVfLeafObservationReader,&gVfRootObservationVtable,&gVfPreparedPhysicalCacheAuthority.physicalMethod,&gVfPreparedPhysicalCacheAuthority.dmaMethod}){
  *value=0;check();assert(observed==0 && !initializers);*value=1;}
 gVfCpuKernelPmapSlot=nullptr;check();assert(observed==0 && !initializers);gVfCpuKernelPmapSlot=&pmap;
 pmap=0;check();assert(observed==0 && !initializers);pmap=1;
 gVfCpuSystemMapperSlot=nullptr;check();assert(observed==0 && !initializers);gVfCpuSystemMapperSlot=&mapper;
 early=true;check();assert(observed==0 && !initializers);early=false;
 gVfIdentity=VfIdentity::Physical;check();assert(observed==0 && !initializers);gVfIdentity=VfIdentity::Virtual;
 lockAllowed=false;check();assert(observed==0 && initializers==1);lockAllowed=true;
 publishLock=false;check();assert(observed==0 && initializers==1);publishLock=true;
 enabled=false;check();assert(observed==-1 && !initializers);
}
'''.replace('__ACTUAL__',body)
    path=root/'prepared_capture_arm.cpp';path.write_text(fixture)
    binary=root/'prepared_capture_arm'
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-I'+str(ROOT),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    print('Actual V394 arm: all binding/identity/lock/opt-in refusals PASS; lock initializer only after complete bindings')

def test_prepared_resource_runtime(root):
    source=(ROOT/'NootedGreen/kern_gen11.cpp').read_text()
    a=source.index('template<class Live>\nstatic void vfObserveChecksumPreparedResources(')
    b=source.index('\nvoid Gen11::vfChecksumUnmap(',a)
    body=source[a:b]
    fixture=r'''
#include "NootedGreen/kern_checksum_gen12_commands.hpp"
#include "NootedGreen/kern_flight_trace.hpp"
#include <cassert>
#include <cstring>
#include <vector>
#include <array>
#include <cstdio>
using mach_vm_address_t=uintptr_t;
template<class T>T getMember(void*p,size_t n){T v;std::memcpy(&v,static_cast<uint8_t*>(p)+n,sizeof(v));return v;}
#define SYSLOG(...) do{}while(0)
constexpr uint64_t base=0xffff800000000000ULL;
std::vector<uint8_t> memory(1024*1024);
const uint64_t shared=base+0x90000,task=base+0xa0000,gpuTask=base+0xc0000;
uintptr_t gVfNativeBatchMapVtable=base+0xe0000,gVfMediaSysMemoryVtable=base+0xe1000;
NGFlightTrace::PreparedPhysicalCacheAuthority gVfPreparedPhysicalCacheAuthority{base+0xd0000,base+0xd0100,true};
bool gVfNativeObjectLayoutBound=true,gVfMediaMapOffsetLayoutVerified=true;
void* mapper=nullptr;void**gVfCpuSystemMapperSlot=&mapper;
uintptr_t gVfBatchCpuCopy;
uint64_t copy(uint64_t from,uint64_t to,uint64_t n){
 if(from<base || n>memory.size() || from-base>memory.size()-n)return 0;
 std::memcpy(reinterpret_cast<void*>(to),memory.data()+from-base,n);return n;
}
template<class T>void set(uint64_t at,T value){assert(at>=base && at-base<=memory.size()-sizeof(T));std::memcpy(memory.data()+at-base,&value,sizeof(T));}
struct VfMediaSubmission {void*accelerator=reinterpret_cast<void*>(base+0xb0000);void*context;
 uint32_t flightGroup=4;NgreenChecksumGen12::NativeHeapBindings checksumHeaps{};NgreenChecksumGen12::NativeSurfaceBindings checksumSurfaces{};};
std::vector<std::array<uint32_t,16>> records,pteRecords;
std::vector<std::array<uint32_t,660>> snapshots;
uint32_t gVfChecksumResourceSnapshots[2][6][660];
int lockObject=0,lockDepth=0;int*gVfPageTableUpdateLock=&lockObject;
void IORecursiveLockLock(int*){assert(!lockDepth);++lockDepth;}
void IORecursiveLockUnlock(int*){assert(lockDepth==1);--lockDepth;}
uintptr_t gVfRootObservationVtable=base+0xf1000,gVfLeafObservationReader=0;
bool nativeRead=true,changeTable=false;
bool readLeaf(const void*,uint64_t address,uint64_t&physical,uint64_t&flags){
 assert(lockDepth==1);if(!nativeRead)return false;
 unsigned role=unsigned((address-0x40000000)/0x400000);
 physical=0x10000000+role*0x1000000+(address-(0x40000000+role*0x400000));flags=0x8b;
 if(changeTable){uint64_t changed=base+0xf0008;std::memcpy(memory.data()+gpuTask-base+0x260,&changed,8);}
 return true;
}
void vfFlightRecord(uint32_t kind,uint32_t group,uint64_t,const volatile uint32_t*p,uint32_t n){
 if(kind==34){assert(!lockDepth && group==4 && n==660);std::array<uint32_t,660> r{};for(unsigned i=0;i<660;++i)r[i]=p[i];snapshots.push_back(r);return;}
 assert(!lockDepth && (kind==32 || kind==33) && group==4 && n==16);std::array<uint32_t,16> r{};for(unsigned i=0;i<16;++i)r[i]=p[i];(kind==32?records:pteRecords).push_back(r);
}
template<class T,unsigned long N>constexpr unsigned arrsize(const T(&)[N]){return N;}
__ACTUAL__
int main(){for(unsigned mode=0;mode<25;++mode){
 std::fill(memory.begin(),memory.end(),0);records.clear();pteRecords.clear();snapshots.clear();
 gVfPageTableUpdateLock=mode==20?nullptr:&lockObject;nativeRead=mode!=23;changeTable=mode==24;
 gVfLeafObservationReader=reinterpret_cast<uintptr_t>(&readLeaf);
 set(gpuTask+0x260,base+0xf0000);set(base+0xf0000,gVfRootObservationVtable+(mode==21?8:0));
 set(gVfRootObservationVtable+0x140,gVfLeafObservationReader+(mode==22?8:0));
 alignas(16) uint8_t context[0x600]{};VfMediaSubmission entry{};entry.context=context;
 std::memcpy(context+0x578,&task,8);std::memcpy(context+0x5a0,&shared,8);
 set(shared+0x78,reinterpret_cast<uint64_t>(entry.accelerator));set(shared+0x80,task);set(shared+0x88,gpuTask);
 const uint64_t descriptorTable=base+0xe2000;
 set(descriptorTable+0x130,gVfPreparedPhysicalCacheAuthority.dmaMethod);set(descriptorTable+0x138,gVfPreparedPhysicalCacheAuthority.physicalMethod);
 NgreenChecksumGen12::NativeSshMapPagePair pair{};pair.first=pair.second=4096;pair.flags=15;
 for(unsigned i=0;i<16;++i)pair.before[i]=i<8?64+i*64:0;
 const uint64_t sizes[]={4096,4096,4096,2088960,1044480,4096};
 for(unsigned role=0;role<6;++role){
  const uint64_t r=base+role*0x10000,m=r+0x1000,sys=r+0x2000,d=r+0x3000,e=r+0x4000,p=r+0x5000,u=r+0x9000;
  const uint64_t gpu=0x40000000+role*0x400000;
  set(r+0x40,m);set(r+0x88,sys);set(m,gVfNativeBatchMapVtable);set(m+0x88,reinterpret_cast<uint64_t>(entry.accelerator));set(m+0x90,gpuTask);set(m+0x18,sys);set(m+0x98,gpu);
  set(sys,gVfMediaSysMemoryVtable);set(sys+0x28,reinterpret_cast<uint64_t>(entry.accelerator));set(sys+0x40,sizes[role]);set(sys+0xd0,d);
  set(d,descriptorTable);set(d+0x20,uint32_t(mode==3?0x20:0x10));set(d+0x50,sizes[role]);set(d+0x88,uint32_t(1));set(d+0x90,e);
  const uint32_t pages=uint32_t((sizes[role]+4095)/4096);set(e+0x10,p);set(e+0x18,uint64_t(48+8*pages+40));set(p+0x28,pages);
  for(unsigned i=0;i<pages;++i)set(p+48+8*i,uint64_t(0x10000+role*0x1000+i));
  NGFlightTrace::PreparedPhysicalCacheBlock block{};block.upl=u;set(p+48+8*pages,block);
  if(role<3)assert(entry.checksumHeaps.record(role,true,r,gpu,4096,false,task,shared));
  else {unsigned i=role-3;assert(entry.checksumSurfaces.record(true,r,gpu,i<2?1920:4096,i,i==2));auto*s=pair.before+16+16*i;
   s[0]=i==0?0x25017000U:i==1?0x24297000U:0x835d4000U;s[1]=(i<2?4U:6U)<<24;s[7]=0x09770000;
   s[2]=i<2?479U|((i?543U:1087U)<<16):127U|(7U<<16);s[3]=i<2?1919:3;s[8]=gpu;
  }
  if(mode==1 && role==3)set(m+0x10,uint32_t(0x40));
  if(role==3)switch(mode){
   case 5:set(m,gVfNativeBatchMapVtable+8);break;
   case 6:set(sys,gVfMediaSysMemoryVtable+8);break;
   case 7:set(r+0x88,uint64_t(0));break;
   case 8:set(m+0x18,sys+8);break;
   case 9:set(m+0x88,reinterpret_cast<uint64_t>(entry.accelerator)+8);break;
   case 10:set(m+0x90,gpuTask+8);break;
   case 11:set(m+0x11c,uint8_t(1));break;
   case 12:set(sys+0x28,reinterpret_cast<uint64_t>(entry.accelerator)+8);break;
   case 13:set(sys+0xd0,uint64_t(0x1000));break;
   case 14:set(sys+0x40,uint64_t(4*1024*1024+1));break;
   case 15:set(m+0x98,gpu+4096);break;
   case 16:set(m+0x20,sizes[role]+1);break;
   case 17:set(d,descriptorTable+8);break;
   case 18:set(d+0x88,uint32_t(0));break;
   case 19:set(p+48,uint64_t(0));break;
  }
 }
 for(unsigned i=0;i<1024;++i)pair.after[i]=pair.before[i];
 gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(&copy);if(mode==4)set(shared+0x80,task+8);
 vfObserveChecksumPreparedResources(entry,pair,0,[&](){return mode!=2;});
 if(mode==2 || mode==4){assert(records.empty() && snapshots.empty());continue;}
 unsigned begins=0,ends=0,pages=0;uint64_t covered[6]{};
 for(const auto&r:records){if(r[3]==0)++begins;else if(r[3]==2){++ends;assert((r[15]==0)==(mode==0 || mode>=20 || mode!=3 && r[2]!=3));}else{++pages;covered[r[2]]+=r[11];}
  std::printf("CPU32 %u",mode);for(auto w:r)std::printf(" %u",w);std::puts("");}
 assert(begins==6 && ends==6);if(mode==0){assert(pages==769);for(unsigned i=0;i<6;++i)assert(covered[i]==sizes[i]);}
 if(mode==1)assert(!covered[3]);if(mode==3)assert(!pages);
 assert(!lockDepth && pteRecords.size()==pages);
 assert(snapshots.size()==6);
 const unsigned expectedStage[]={0,8,0,0,0,4,10,3,7,5,6,9,11,12,13,14,15,0,0,0,0,0,0,0,0};
 for(const auto&r:snapshots){assert(r[13]==(r[2]==3?expectedStage[mode]:0));std::printf("SNAP34 %u",mode);for(auto w:r)std::printf(" %u",w);std::puts("");}
 for(const auto&r:pteRecords){std::printf("PTE33 %u",mode);for(auto w:r)std::printf(" %u",w);std::puts("");}
 }std::puts("Actual six-resource prepared CPU runtime: full coverage/foreign mode/missing descriptor/owner roots PASS; no native invocation");}
'''.replace('__ACTUAL__',body)
    path=root/'prepared_resources.cpp';path.write_text(fixture)
    binary=root/'prepared_resources'
    subprocess.run([*CXX,'-std=c++14','-fsanitize=address,undefined','-I'+str(ROOT),str(path),'-o',str(binary)],check=True)
    output=subprocess.run([str(binary)],check=True,capture_output=True,text=True).stdout
    import importlib.util
    spec=importlib.util.spec_from_file_location('ngreen_prepared_decode',ROOT/'tools/media_adapter/decode_kdebug_recorder.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    rows_by_mode={};leaves_by_mode={};snapshots_by_mode={}
    for line in output.splitlines():
        if line.startswith('CPU32 '):
            mode,*words=[int(n) for n in line.split()[1:]]
            module.validate_leaf(32,words)
            rows_by_mode.setdefault(mode,[]).append((4,{'thread':77,'identity':f'{0x40000000+words[2]*0x400000:016x}'},struct.pack('<16I',*words)))
        elif line.startswith('SNAP34 '):
            mode,*words=[int(n) for n in line.split()[1:]]
            module.validate_leaf(34,words)
            snapshots_by_mode.setdefault(mode,[]).append((4,{'thread':77,'identity':f'{0x40000000+words[2]*0x400000:016x}'},struct.pack('<660I',*words)))
        elif line.startswith('PTE33 '):
            mode,*words=[int(n) for n in line.split()[1:]]
            module.validate_leaf(33,words)
            leaves_by_mode.setdefault(mode,[]).append((4,{'thread':77,'identity':f'{0x40000000+words[2]*0x400000:016x}'},struct.pack('<16I',*words)))
    for mode,rows in rows_by_mode.items():
        report=module.prepared_resource_coverage(rows,leaves_by_mode.get(mode,()))
        if mode>=20:assert report['stable_native_task_leaf_reads']==0
        else:assert report['stable_native_task_leaf_reads']==report['task_leaf_pages']
        assert report['resources']==6 and report['complete_cpu_page_coverage']==(6 if mode==0 or mode>=20 else 0 if mode==3 else 5)
        assert not report['iova_mapping_proven'] and not report['gpu_execution_proof']
    for mode,snapshots in snapshots_by_mode.items():
        admission=module.prepared_resource_admission(snapshots)
        assert admission['captures']==6 and not admission['gpu_execution_proof']
        for item in admission['resources']:
            if mode==0:assert item['admitted'] and item['pair_flags']==63 and all(item['expected_matches_before'].values())
        if mode==10:assert admission['resources'][3]['stage']=='map-task' and admission['resources'][3]['expected_matches_before']['map_task'] is False
        if mode==8:assert admission['resources'][3]['stage']=='map-memory-association' and admission['resources'][3]['before']['map_memory_descriptor'] is None
    good=snapshots_by_mode[0][0];bad_snapshots=[]
    for field,value in ((0,2),(2,6),(3,2636),(4,145),(12,3),(13,18),(14,1),(17,0),(30,0),(31,1)):
        words=list(struct.unpack('<660I',good[2]));words[field]=value
        bad_snapshots.append((good[0],good[1],struct.pack('<660I',*words)))
    partial=snapshots_by_mode[5][3];words=list(struct.unpack('<660I',partial[2]));words[106]=1
    bad_snapshots.append((partial[0],partial[1],struct.pack('<660I',*words)))
    bad_snapshots.append((good[0],good[1],good[2][:-4]))
    bad_snapshots.append((good[0],{'thread':77,'identity':'0000000000000001'},good[2]))
    for bad in bad_snapshots:
        try:module.prepared_resource_admission([bad])
        except ValueError:pass
        else:raise AssertionError('corrupt admission prefix accepted')
    rows=rows_by_mode[0]
    negatives=[rows[1:],rows[:-1],rows[:2]+[rows[1]]+rows[2:],[rows[1],rows[0]]+rows[2:],
        [row for row in rows if struct.unpack('<16I',row[2])[2]!=5],
        [rows[0],(4,{'thread':78,'identity':rows[1][1]['identity']},rows[1][2])]+rows[2:]]
    for index,field,value in ((1,4,7),(1,7,1),(1,11,4097),(2,11,4095),(2,15,0),(0,0,2)):
        bad=list(rows);words=list(struct.unpack('<16I',bad[index][2]));words[field]=value
        if bad[index][2]==struct.pack('<16I',*words):continue
        bad[index]=(bad[index][0],bad[index][1],struct.pack('<16I',*words));negatives.append(bad)
    for bad in negatives:
        try:module.prepared_resource_coverage(bad,leaves_by_mode[0])
        except ValueError:pass
        else:raise AssertionError('prepared resource corrupt scope accepted')
    leaves=leaves_by_mode[0]
    bad_leaf_sets=[leaves[1:],leaves+[leaves[0]],[(4,{'thread':78,'identity':leaves[0][1]['identity']},leaves[0][2])]+leaves[1:]]
    for field,value in ((3,1024),(4,1),(6,0),(12,4),(13,0),(15,1)):
        bad=list(leaves);words=list(struct.unpack('<16I',bad[0][2]));words[field]=value
        bad[0]=(bad[0][0],bad[0][1],struct.pack('<16I',*words));bad_leaf_sets.append(bad)
    for bad in bad_leaf_sets:
        try:module.prepared_resource_coverage(rows_by_mode[0],bad)
        except ValueError:pass
        else:raise AssertionError('prepared task leaf corruption accepted')
    print(output.splitlines()[-1]);print(f'Strict actual CPU resource scope decoder: full/refused coverage and {len(negatives)} corrupt scopes rejected')

if __name__=="__main__":
    with tempfile.TemporaryDirectory(prefix="ngreen-checksum-main-end-") as directory:
        destination=Path(directory)
        test_prepared_capture_arm(destination)
        test_prepared_resource_runtime(destination)
        test_observed_surface_ranges(destination)
        test_ssh_unmap(destination)
        test_qualification(destination)
        test_readiness(destination)
        test_end(destination)
        test_surface_bind_returns(destination)
        test_checksum_capture_arm(destination)
        test_complete_heap_arm(destination)
        test_user_adoption(destination)
