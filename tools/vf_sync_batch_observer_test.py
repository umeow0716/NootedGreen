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
import sqlite3
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'NootedGreen/kern_gen11.cpp').read_text()
def native_snapshot_binding_contract(text):
    binding=text[text.index('const auto mapMeta='):text.index('"V385: native object snapshot sizes')]
    for token in ('__ZN16IGAccelMemoryMap9MetaClassC1Ev',
                  '__ZN16IGAccelSysMemory9MetaClassC1Ev',
                  '{0xb9,0x28,0x01,0,0}', '{0xb9,0x98,0x01,0,0}',
                  'matches(mapMeta+0x17U,mapClassSize,sizeof(mapClassSize))',
                  'matches(sysMeta+0x17U,sysClassSize,sizeof(sysClassSize))'):
        assert token in binding
    assert 'if(checkKernelArgument("-ngreencaptureonly") && !gVfNativeObjectLayoutBound)return;' in text
native_snapshot_binding_contract(source)
for token in ('matches(mapMeta+0x17U,mapClassSize,sizeof(mapClassSize))',
              'matches(sysMeta+0x17U,sysClassSize,sizeof(sysClassSize))'):
    try:native_snapshot_binding_contract(source.replace(token,'true',1))
    except AssertionError:pass
    else:raise AssertionError('native class extent gate mutation escaped')
with (root/'sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics').open('rb') as stream:
    for offset,expected in ((0x10bcf,bytes.fromhex('b928010000')),(0x126f5,bytes.fromhex('b998010000'))):
        stream.seek(offset);actual=stream.read(5)
        assert actual==expected
        for i in range(5):
            bad=bytearray(actual);bad[i]^=1
            assert bad!=expected
def resource_scope_contract(text):
    try:
        submit = text[text.index('bool Gen11::vfSubmitWorkItem('):]
        assert submit.index('IORecursiveLockLock(gVfPageTableUpdateLock)') < submit.index('vfObserveBatchLeafMapping(') < \
               submit.index('IORecursiveLockUnlock(gVfPageTableUpdateLock)') < submit.index('VfContextQueueGuard queue(that)')
        assert text.count('vfObserveMediaResourceBases(') == 3
        assert text.count('vfObserveNativeBatchProvenance(') == 2
        assert text.count('vfObserveBatchLeafMapping(') == 2
        signature='static void vfObservePreEndCpuState('
        assert text.count(signature)==2
        pre_end=text[text.rindex(signature):text.index('static void vfObserveBatchLeafMapping(')]
        assert pre_end.index('IORecursiveLockLock(gVfPageTableUpdateLock)') < pre_end.index('if(stable())vfObserveMediaResourceBases(') < \
               pre_end.index('IORecursiveLockUnlock(gVfPageTableUpdateLock)')
        for field in ('resource','mapped','map','descriptor','commands','gpuMap','gpuBase','backingBytes',
                      'availableWords','reservedWords','offset','used','bytes'):
            assert f'after.{field}==span.{field}' in pre_end
        assert 'after.count==words' in pre_end and 'native-descriptor=NOT_PREPARED' in pre_end
        assert all(forbidden not in pre_end for forbidden in ('readReg','writeReg','IOMalloc','IOFree'))
        native = text[text.index('static void vfObserveNativeBatchProvenance('):text.index('static void vfObserveBatchLeafMapping(')]
        assert native.index('const bool stable=') < native.index('vfObserveMediaResourceBases(')
        resource = text[text.index('static void vfObserveMediaResourceBases('):text.index('static void vfObserveNativeBatchProvenance(')]
        for required in ('collectPublishedResourceBases(commands,words,bases)',
                         'if(!parsed || !after || !gVfLeafObservationReader',
                         'NGFlightTrace::observeResourceLeaves(bases',
                         '0x140)==gVfLeafObservationReader', '},stable,'):
            assert required in resource
        assert all(forbidden not in resource for forbidden in ('IOMalloc', 'IOFree', 'writeReg', 'readReg', 'getPhysicalSegment'))
        return True
    except (ValueError, AssertionError):
        return False
assert resource_scope_contract(source), 'resource capture left qualified PT-before-CTB CPU scope'
for token in ('IORecursiveLockLock(gVfPageTableUpdateLock)',
              'IORecursiveLockUnlock(gVfPageTableUpdateLock)',
              'collectPublishedResourceBases(commands,words,bases)',
              'if(!parsed || !after || !gVfLeafObservationReader'):
    # Scope the lock mutation to the submit path, not an older unrelated query.
    if token.startswith('IORecursiveLock'):
        at = source.index('bool Gen11::vfSubmitWorkItem(')
        changed = source[:at]+source[at:].replace(token, 'MUTATED', 1)
    else:
        changed = source.replace(token, 'MUTATED', 1)
    assert not resource_scope_contract(changed)
if sys.argv[1:2] == ['--resource-layout']:
    if len(sys.argv) not in (3, 4):
        raise SystemExit('usage: vf_sync_batch_observer_test.py --resource-layout PINNED_GEN12_VDBOX_DIR [READONLY_CAPTURE_DB]')
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
    if len(sys.argv) == 4:
        sys.path.insert(0, str(root/'tools/media_adapter'))
        from audit_vdenc_batch import mode_template_fields
        connection = sqlite3.connect(Path(sys.argv[3]).resolve().as_uri()+'?mode=ro', uri=True)
        rows = connection.execute('SELECT kind,payload FROM records WHERE group_id=3 AND kind IN (3,4) LIMIT 3').fetchall()
        assert len(rows) == 2 and {kind for kind, _ in rows} == {3, 4}
        checks = ['#include <cassert>\n#include "NootedGreen/kern_avc_gen12_commands.hpp"\nint main(){']
        for kind, raw in rows:
            assert 0 < len(raw) <= 6144 and len(raw) % 4 == 0
            words = struct.unpack(f'<{len(raw)//4}I', raw)
            fields = mode_template_fields(words)
            assert fields['complete_packet_walk']
            expected = []
            for family, collection in enumerate(('vdenc_pipe_resources', 'mfx_pipe_resources',
                                                  'mfx_indirect_resources', 'mfx_bsp_resources'), 1):
                for item in fields[collection]:
                    expected.append((int(item['address_hex'], 16), item['address_word'], family,
                                     item['address_word']-item['packet_word'], int(item['cache_select'] == 1)))
            checks.append('{const uint32_t words[]={'+','.join(hex(word)+'U' for word in words)+'};')
            checks.append('const NgreenAvcGen12::ResourceBase expected[]={'+','.join(
                '{'+f'{gpu}ULL,{word},{family},{slot},{internal}'+'}' for gpu, word, family, slot, internal in expected)+'};')
            checks.append('NgreenAvcGen12::ResourceBases out{};assert(NgreenAvcGen12::collectPublishedResourceBases(words,sizeof(words)/4,out));')
            checks.append('assert(out.complete && out.count==sizeof(expected)/sizeof(expected[0]));')
            checks.append('for(unsigned i=0;i<out.count;++i){unsigned hits=0;for(const auto &e:expected){const auto &a=out.entries[i];hits+=a.gpu==e.gpu&&a.word==e.word&&a.family==e.family&&a.slot==e.slot&&a.internal==e.internal;}assert(hits==1);}}')
        checks.append('}')
        with tempfile.TemporaryDirectory(prefix='ngreen-recorded-resource-') as directory:
            cpp = Path(directory)/'actual.cpp'; cpp.write_text('\n'.join(checks))
            binary = Path(directory)/'test'
            subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++14', '-O1', '-g',
                            '-fsanitize=address,undefined', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        print('PASS production CPU resource parser matches independent offline inventory for saved publication and native batch; no GPU call')
elif sys.argv[1:]:
    if len(sys.argv) not in (4, 5) or sys.argv[1] != '--native':
        raise SystemExit('usage: vf_sync_batch_observer_test.py [--native TGL BOOTKC [FAMILY]]')
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
            (sys.argv[3], 0x2b1bc0, 57, 0x51b84c628134c4ea),
            (sys.argv[3], 0x2ccdc0, 362, 0xd8a37de28d61bb88),
            (sys.argv[3], 0x2c8b10, 208, 0x292f758de9058cc4),
            (sys.argv[3], 0x2b1880, 816, 0x88e1cd84195161db),
            (sys.argv[3], 0x8dd0a0, 720, 0x1310a28d6256735c),
            (sys.argv[3], 0x8dc800, 1024, 0xf383ff4366dd7555),
            (sys.argv[3], 0x8dcc00, 1024, 0x0e005644b664144d),
            (sys.argv[3], 0x8dd000, 160, 0x25a17a997245dbb9)):
        data = bounded(path, offset, size)
        assert f'0x{digest:016x}' in source and fnv(data) == digest
        for index in range(size):
            bad = bytearray(data); bad[index] ^= 1
            assert fnv(bad) != digest
            mutations += 1
    mapper_test = bounded(sys.argv[3], 0x8dca4c, 8)
    assert mapper_test[:3] == bytes.fromhex('48833d') and mapper_test[7] == 0
    assert 0xffffff8000adca54 + struct.unpack_from('<i', mapper_test, 3)[0] == 0xffffff80011e4ba8
    assert bounded(sys.argv[2], 0x42f7a, 14) == bytes.fromhex('554889e54156534883ec104989f6')
    caller = bounded(sys.argv[2], 0x4c828, 47)
    assert caller == bytes.fromhex('498bbc2430010000498b7618498b4e20458b4630450fb64e6a410fb64669488b1f4883ec084489fa50ff9318010000')
    # Paired embedded Mach-O identity, not an identically named standalone file.
    outer=struct.unpack('<8I',bounded(sys.argv[3],0,32))
    assert outer[0]==0xfeedfacf and outer[1]==0x01000007 and outer[3]==0xc
    header=bounded(sys.argv[3],0xe8000,32)
    fields=struct.unpack('<8I',header)
    assert fields[0]==0xfeedfacf and fields[3]==2 and fields[4]==21 and fields[5]<=8192
    commands=b''.join(bounded(sys.argv[3],0xe8020+off,min(1024,fields[5]-off))
                      for off in range(0,fields[5],1024))
    at=0;uuids=[]
    for _ in range(fields[4]):
        kind,length=struct.unpack_from('<II',commands,at)
        assert 8<=length<=4096 and at+length<=len(commands)
        if kind==0x1b:uuids.append(commands[at+8:at+24].hex())
        at+=length
    assert at==len(commands) and uuids==['1291d44ac0cd3ac7a9e4ca2749b33959']
    lookup=bounded(sys.argv[3],0x2b1bc0,57)
    assert 0xffffff80004b1bc0+7+struct.unpack_from('<i',lookup,3)[0]==0xffffff8001223d40
    assert 0xffffff80004b1bc0+0x33+struct.unpack_from('<i',lookup,0x2f)[0]==0xffffff80004b1880
    assert struct.unpack('<Q',bounded(sys.argv[3],0x6a6f0,8))[0]==0xffffff8001223d40
    copier = bounded(sys.argv[3], 0x2ccdc0, 362)
    for displacement, next_instruction, target in (
            (0x31, 0x35, 0xffffff80004b1880),
            (0x7b, 0x7f, 0xffffff80004b1880),
            (0x12d, 0x131, 0xffffff80004c8b10)):
        assert 0xffffff80004ccdc0 + next_instruction + struct.unpack_from('<i', copier, displacement)[0] == target
    if len(sys.argv) == 5:
        family = sys.argv[4]
        for offset, size, digest in ((0x1ce9e, 22, 0x8af5292e747d0d15),
                                     (0x1cebe, 42, 0x34662c7e271165d2)):
            original = bounded(family, offset, size)
            assert f'0x{digest:016x}' in source and fnv(original) == digest
            for index in range(size):
                bad = bytearray(original); bad[index] ^= 1
                assert fnv(bad) != digest; mutations += 1
        assert bounded(family, 0x525be, 18) == bytes.fromhex('4889b798000000488997c8000000804f1140')
        assert bounded(family, 0x39c20, 7) == bytes.fromhex('488db700020000')
        assert bounded(family, 0x39c3e, 7) == bytes.fromhex('4981c7e8010000')
    print(f'PASS bounded native batch/copy/pmap spans and {mutations} byte mutations; exact caller and relocation-free entry; static only')
# Execute the actual binding admission statements with owned symbol addresses.
# Hash results are injected here; --native independently checks the exact code
# spans. No target helper is called, and every individual refusal is preserved.
ram_begin = source.index('const auto cpuLookup=patcher.solveSymbol(')
ram_end = source.index('gVfBatchCpuCopy=0;', ram_begin)
copy_begin = source.index('if(gVfCpuBackingLookup && nofaultCopy && physicalCopy &&', ram_end)
copy_end = source.index('constexpr uint8_t parseCall[]', copy_begin)
binding_fixture = r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
using mach_vm_address_t=uintptr_t;
alignas(16) static unsigned char arena[32768];
static unsigned scenario,logs;
static uintptr_t gVfCpuBackingLookup,gVfBatchCpuCopy;
static void **gVfCpuKernelPmapSlot,**gVfCpuSystemMapperSlot;
static void *gVfCpuKernelPmapStore;
static bool gVfMapSnapshotLayoutBound=false;
static void logCall(const char *,const char *,...){++logs;}
#define SYSLOG(...) logCall(__VA_ARGS__)
struct KernelPatcher {enum {KernelID=0};};
struct MockPatcher {
 uintptr_t solveSymbol(int,const char *name) {
  const char *names[]={"_pmap_find_phys_nofault","_pmap_find_pa","_kernel_pmap",
    "__ZN8IOMapper7gSystemE","_ml_nofault_copy","_bcopy_phys"};
  for(unsigned i=0;i<6;++i)if(!std::strcmp(name,names[i]))
   return scenario==i+1?0:reinterpret_cast<uintptr_t>(arena+(i+1)*4096);
  assert(false);return 0;
 }
} patcher;
template<class T>T getMember(void *p,unsigned offset){T v;std::memcpy(&v,static_cast<unsigned char *>(p)+offset,sizeof(v));return v;}
namespace NGFlightTrace {
 uint64_t boundedCodeHash(const volatile uint8_t *p,unsigned length){
  const unsigned offsets[]={4096,8192,20480,24576},sizes[]={57,816,362,208};
  const uint64_t hashes[]={UINT64_C(0x51b84c628134c4ea),UINT64_C(0x88e1cd84195161db),
    UINT64_C(0xd8a37de28d61bb88),UINT64_C(0x292f758de9058cc4)};
  const uint64_t standalone[]={UINT64_C(0x8d33f3e48fddb6bd),UINT64_C(0xddd9ec77c57b4d13),
    UINT64_C(0x648d5b7fd7277f0e),UINT64_C(0xf9172f81775ea555)};
  for(unsigned i=0;i<4;++i)if(p==arena+offsets[i]){assert(length==sizes[i]);
    return scenario==i+16?standalone[i]:hashes[i]^(scenario==i+7?1:0);}
  assert(false);return 0;
 }
}
static void displacement(unsigned entry,unsigned field,unsigned next,unsigned target){
 int32_t delta=static_cast<int32_t>(target-entry-next);std::memcpy(arena+entry+field,&delta,4);
}
static void actualBinding(){
__ACTUAL_RAM__
 gVfBatchCpuCopy=0;
__ACTUAL_COPY__
}
int main(){
 for(scenario=0;scenario<=19;++scenario){
  std::memset(arena,0,sizeof(arena));logs=0;
  gVfCpuBackingLookup=gVfBatchCpuCopy=0;gVfCpuKernelPmapSlot=gVfCpuSystemMapperSlot=nullptr;gVfCpuKernelPmapStore=nullptr;
  displacement(4096,3,7,28672);displacement(4096,0x2f,0x33,8192+(scenario==11?1:0));
  displacement(20480,0x31,0x35,8192+(scenario==12?1:0));
  displacement(20480,0x7b,0x7f,8192+(scenario==13?1:0));
  displacement(20480,0x12d,0x131,24576+(scenario==14?1:0));
  *reinterpret_cast<void **>(arena+12288)=arena+28672+(scenario==15?8:0);
  actualBinding();assert(logs==3);
  const bool ramOk=scenario==0 || scenario==5 || scenario==6 || scenario==9 || scenario==10 || scenario==12 || scenario==13 || scenario==14 || scenario==18 || scenario==19;
  assert(bool(gVfCpuBackingLookup)==ramOk);assert(bool(gVfBatchCpuCopy)==(scenario==0));
  assert(bool(gVfCpuKernelPmapSlot)==ramOk && bool(gVfCpuSystemMapperSlot)==ramOk && bool(gVfCpuKernelPmapStore)==ramOk);
 }
 std::puts("PASS actual RAM/copy binding admission: positive and 19 independent refusals including all four standalone hashes; no native helper invocation");
}
'''.replace('__ACTUAL_RAM__',source[ram_begin:ram_end]).replace('__ACTUAL_COPY__',source[copy_begin:copy_end])
with tempfile.TemporaryDirectory(prefix='ngreen-bindings-') as directory:
    cpp=Path(directory)/'binding.cpp';cpp.write_text(binding_fixture)
    binary=Path(directory)/'binding'
    subprocess.run([os.environ.get('CXX','clang++'),'-std=c++14','-O1','-g','-fsanitize=address,undefined',
                    str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
# Compile the production layout-binding statements, independently refusing each
# symbol, each code chunk, and the actual RIP-relative mapper target.
prepared_begin=source.index('const auto preparedPhysical=patcher.solveSymbol(')
prepared_end=source.index('const auto preparedMulti=patcher.solveSymbol(',prepared_begin)
prepared_fixture=r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
alignas(16) static uint8_t arena[8192];
static unsigned scenario;
struct Authority {uint64_t physicalMethod=0,dmaMethod=0;bool exactLayout=false;} gVfPreparedPhysicalCacheAuthority;
struct KernelPatcher {enum {KernelID=0};};
struct MockPatcher {uintptr_t solveSymbol(int,const char *name){
 const char *names[]={"__ZN25IOGeneralMemoryDescriptor18getPhysicalSegmentEyPyj",
 "__ZNK25IOGeneralMemoryDescriptor19dmaCommandOperationEjPvj","__ZN8IOMapper7gSystemE"};
 for(unsigned i=0;i<3;++i)if(!std::strcmp(name,names[i]))return scenario==i+1?0:reinterpret_cast<uintptr_t>(arena+(i+1)*1024);
 assert(false);return 0;
}} patcher;
template<class T>T getMember(void *p,unsigned n){T v;std::memcpy(&v,static_cast<uint8_t *>(p)+n,sizeof(v));return v;}
namespace NGFlightTrace {uint64_t boundedCodeHash(const volatile uint8_t *p,unsigned bytes){
 const unsigned offsets[]={1024,2048,3072,4096},sizes[]={720,1024,1024,160};
 const uint64_t hashes[]={UINT64_C(0x1310a28d6256735c),UINT64_C(0xf383ff4366dd7555),UINT64_C(0x0e005644b664144d),UINT64_C(0x25a17a997245dbb9)};
 for(unsigned i=0;i<4;++i)if(p==arena+offsets[i]){assert(bytes==sizes[i]);return hashes[i]^(scenario==i+4?1:0);}
 assert(false);return 0;
}}
#define SYSLOG(...) do{}while(0)
static void bind(){__ACTUAL_PREPARED__}
int main(){for(scenario=0;scenario<=8;++scenario){
 gVfPreparedPhysicalCacheAuthority={};
 const int32_t displacement=static_cast<int32_t>(3072-2048-0x254+(scenario==8?8:0));
 std::memcpy(arena+2048+0x24f,&displacement,4);bind();
 assert(gVfPreparedPhysicalCacheAuthority.exactLayout==(scenario==0));
 assert(bool(gVfPreparedPhysicalCacheAuthority.physicalMethod)==(scenario==0));
 assert(bool(gVfPreparedPhysicalCacheAuthority.dmaMethod)==(scenario==0));
 }std::puts("PASS actual prepared-cache binding: exact admission and eight independent refusals; no native call");}
'''.replace('__ACTUAL_PREPARED__',source[prepared_begin:prepared_end])
with tempfile.TemporaryDirectory(prefix='ngreen-prepared-binding-') as directory:
    cpp=Path(directory)/'binding.cpp';cpp.write_text(prepared_fixture)
    binary=Path(directory)/'binding'
    subprocess.run([os.environ.get('CXX','clang++'),'-std=c++14','-O1','-g','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
start = source.index('bool Gen11::vfObserveRingBatch(')
stop = source.index('\n}', source.index('return FunctionCast(vfObserveRingBatch,', start)) + 2
body = source[start:stop]
assert body.count('__builtin_return_address(0)') == 1
body = body.replace('__builtin_return_address(0)', 'testCaller')
map_start = source.index('template<class Stable>\nstatic void vfObserveTaskMapHeaders(')
map_body = source[map_start:source.index('\ntemplate<class Stable>\nstatic void vfObserveMediaResourceBases(', map_start)]
assert all(token not in map_body for token in ('readReg', 'writeReg', 'getPhysicalSegment', 'searchMapForGPU', 'retain(', 'IOMalloc'))
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <cstdio>
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
static uint32_t gVfNativeObjectCopies[4][352]{};
static volatile UInt32 gVfNativeObjectCopyBusy[4]{};
static bool captureMode=false;
static bool checkKernelArgument(const char *name){assert(!std::strcmp(name,"-ngreencaptureonly"));return captureMode;}
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
static bool gVfMapSnapshotLayoutBound=true;
static bool gVfNativeObjectLayoutBound=true;
static mach_vm_address_t gVfNativeBatchMapVtable=0xffffff804010a000ULL;
static mach_vm_address_t gVfMediaSysMemoryVtable=0xffffff804010b000ULL;
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
map_harness = r'''
static uint32_t taskRaw[134],nodeRaw[74],memoryRaw[102];
static unsigned mapScenario=0,mapReads=0,rawRecords=0,currentRecord=0;
static unsigned memoryReads=0;
static uint32_t rawKinds[32],rawCounts[32],rawRecordsData[32][366];
static constexpr uint64_t taskAt=0xffffff8041000000ULL,nodeAt=0xffffff8041002000ULL,memoryAt=0xffffff8041004000ULL;
static void put64(uint32_t *words,unsigned offset,uint64_t value){words[offset/4]=uint32_t(value);words[offset/4+1]=uint32_t(value>>32);}
static void mapTrace(uint32_t event,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t){
    if(event==NGFlightTrace::begin){
        assert(rawRecords<32);currentRecord=rawRecords++;rawKinds[currentRecord]=uint32_t(c>>32);
        rawCounts[currentRecord]=uint32_t(c);assert(rawCounts[currentRecord]<=366);
    }else if(event==NGFlightTrace::data){
        const uint32_t offset=uint32_t(a);uint64_t packed[]={b,c,d};
        for(unsigned i=0;i<6 && offset+i<rawCounts[currentRecord];++i)
            rawRecordsData[currentRecord][offset+i]=uint32_t(packed[i/2]>>((i&1)*32));
    }
}
static uint64_t mapCopy(uint64_t source,uint64_t destination,uint64_t bytes){
    const void *from=nullptr;uint64_t done=bytes;
    if(source==taskAt+0x1e8 || source==taskAt+0x200){assert(bytes==20);from=taskRaw+(source-taskAt)/4;}
    else if(source==nodeAt){
        assert(bytes==208 || (captureMode && bytes==296));++mapReads;
        if(mapScenario==14 && bytes==296)done=292;
        if(mapScenario==15 && mapReads==4)++nodeRaw[0x114/4];
        if(mapScenario==2)done=204;
        if(mapScenario==3 && mapReads==2)nodeRaw[0x98/4]+=4096;
        from=nodeRaw;
    }else if(source==memoryAt){assert(bytes==8 || bytes==216 || (captureMode && bytes==408));from=memoryRaw;
        if(bytes==408 && ++memoryReads==2 && mapScenario==16)++memoryRaw[0xd8/4];}
    else assert(false && "unexpected getter/DMA/foreign CPU read");
    std::memcpy(reinterpret_cast<void *>(destination),from,done);return done;
}
static void testMapSnapshotBackend(){
    for(mapScenario=0;mapScenario<17;++mapScenario){
        reset();mapReads=memoryReads=rawRecords=currentRecord=0;captureMode=mapScenario>=10;
        for(unsigned i=0;i<4;++i)gVfNativeObjectCopyBusy[i]=mapScenario==13?1:0;
        std::memset(taskRaw,0,sizeof(taskRaw));std::memset(nodeRaw,0,sizeof(nodeRaw));std::memset(memoryRaw,0,sizeof(memoryRaw));
        std::memset(rawRecordsData,0,sizeof(rawRecordsData));
        put64(taskRaw,0x1e8,nodeAt);put64(taskRaw,0x1f0,nodeAt);taskRaw[0x1f8/4]=1;
        put64(nodeRaw,0,gVfNativeBatchMapVtable);put64(nodeRaw,0x18,memoryAt);
        put64(nodeRaw,0x88,reinterpret_cast<uintptr_t>(gVfAccelerator));put64(nodeRaw,0x90,taskAt);
        put64(nodeRaw,0x98,0x40010000);put64(nodeRaw,0xc8,131072);nodeRaw[4]=0x4001;
        put64(memoryRaw,0,gVfMediaSysMemoryVtable);put64(memoryRaw,0x28,reinterpret_cast<uintptr_t>(gVfAccelerator));
        put64(memoryRaw,0x40,65536);put64(memoryRaw,0xd0,0xffffff8041006000ULL);
        if(mapScenario==1)nodeRaw[4]|=0x40; // Physical mode must not call a physical getter.
        if(mapScenario==4)put64(nodeRaw,0x90,taskAt+4096);
        if(mapScenario==5)put64(nodeRaw,0,gVfNativeBatchMapVtable+4096);
        if(mapScenario==7)put64(nodeRaw,0x30,nodeAt);
        if(mapScenario==9)put64(memoryRaw,0,gVfMediaSysMemoryVtable+4096);
        if(mapScenario==11)put64(nodeRaw,0,gVfNativeBatchMapVtable+4096);
        if(mapScenario==12)put64(memoryRaw,0,gVfMediaSysMemoryVtable+4096);
        nodeRaw[0x114/4]=1;memoryRaw[0xd8/4]=0x300;
        gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(&mapCopy);gVfFlightTrace=mapTrace;
        vfObserveTaskMapHeaders(reinterpret_cast<void *>(mapScenario==8?0x40010000ULL:taskAt),0,3,0x40010000,
                               [](){return mapScenario!=6;});
        const unsigned expected[]={0,0,1,5,4,0,5,3,3,0,0,1,0,1,1,5,5};
        unsigned end0=99,nodeCount=0;
        for(unsigned i=0;i<rawRecords;++i){
            const auto *r=rawRecordsData[i];assert(r[0]==(captureMode && rawKinds[i]==23?2U:1U));
            if(rawKinds[i]==24){assert(rawCounts[i]==24 && !r[10] && !r[11]);if(r[3]==1 && r[2]==0)end0=r[4];}
            else {assert(rawKinds[i]==23 && rawCounts[i]==(captureMode?366U:226U) && !r[11] && !r[12]);++nodeCount;
                if(mapScenario==10){assert(r[6]==296 && r[8]==408 && r[10]==255);
                    assert(r[14+0x114/4]==1 && r[14+148+0xd8/4]==0x300);}
                if(mapScenario==0 || mapScenario==1){assert(r[6]==208 && r[7]==208 && r[8]==216 && r[9]==216 && r[10]==255);
                    assert(r[14+0xc8/4]==131072 && r[14+104+0x40/4]==65536);}
                if(mapScenario==2)assert(r[6]==204 && r[7]==204 && !r[8] && !r[9]);
                if(mapScenario==5)assert(!r[8] && !r[9] && !(r[10]&32));
                if(mapScenario==9)assert(r[8]==8 && r[9]==8 && !(r[10]&64));
            }
        }
        assert(end0==expected[mapScenario]);
        assert(nodeCount==(mapScenario==6 || mapScenario==8 || mapScenario==13?0U:1U));
        if(captureMode && mapScenario!=13)for(unsigned i=0;i<4;++i)assert(!gVfNativeObjectCopyBusy[i]);
        for(unsigned i=0;i<rawRecords;++i){
            std::printf("OWNED_MAP_RECORD %u %u",mapScenario,rawKinds[i]);
            for(unsigned j=0;j<rawCounts[i];++j)std::printf(" %u",rawRecordsData[i][j]);
            std::printf("\n");
        }
    }
    captureMode=false;
    captureMode=true;gVfNativeObjectLayoutBound=false;rawRecords=mapReads=0;
    vfObserveTaskMapHeaders(reinterpret_cast<void *>(taskAt),0,3,0x40010000,[](){return true;});
    assert(!rawRecords && !mapReads);
    captureMode=false;gVfNativeObjectLayoutBound=true;
    gVfFlightTrace=trace;gVfBatchCpuCopy=reinterpret_cast<uintptr_t>(&copy);
}
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
    testMapSnapshotBackend();
}
'''
with tempfile.TemporaryDirectory(prefix='ngreen-sync-batch-') as directory:
    cpp = Path(directory)/'actual.cpp'
    cpp.write_text(prefix+body+map_body+map_harness+tests)
    binary = Path(directory)/'test'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++14', '-O1', '-g',
                    '-fsanitize=address,undefined', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr[:4096]
    sys.path.insert(0, str(root/'tools/media_adapter'))
    from decode_kdebug_recorder import DiskSink, KINDS
    sinks = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        assert parts[0] == 'OWNED_MAP_RECORD'
        scenario, kind = map(int, parts[1:3]); words = list(map(int, parts[3:]))
        if scenario not in sinks:
            sinks[scenario] = DiskSink(Path(directory)/f'case-{scenario}')
        sink = sinks[scenario]
        number = sink.db.execute('SELECT COUNT(*) FROM records').fetchone()[0]+1
        sink(dict(record_id=number, group=3, identity='0000000040010000',
                  kind=kind, kind_name=KINDS[kind], words=words))
    assert len(sinks) == 17
    for scenario, sink in sinks.items():
        report = sink.map_list_report()
        assert report['lists'] == 2 and not report['native_object_lease_proven']
        if scenario in (0, 1, 5, 9, 10, 12):
            assert report['observed_complete_topologies'] == 2
        else:
            assert report['partial_or_missing_end'] > 0
        sink.close()
print('PASS actual synchronous observer ASan/UBSan native-once cases; actual nofault CPU map backend 17 cases plus strict decoder/SQL; owned callbacks only')

# New prepared-page CPU reader: fake canonical addresses backed only by owned
# arrays. No native getter, mapper, descriptor, VM or GPU call is possible.
prepared_cache_test = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include "NootedGreen/kern_flight_trace.hpp"
using namespace NGFlightTrace;
struct Image {
    static constexpr uint64_t d=0xffff800000100000ULL,v=0xffff800000102000ULL;
    static constexpr uint64_t e=0xffff800000104000ULL,p=0xffff800000106000ULL;
    static constexpr uint64_t u=0xffff800000108000ULL,x=0xffff80000010a000ULL;
    std::array<std::array<uint8_t,4096>,6> data{};
    std::array<uint64_t,6> bases{{d,v,e,p,u,x}};
    PreparedPhysicalCacheAuthority authority{0xffff800000900000ULL,0xffff800000900100ULL,true};
    bool alive=true,deny=false,change=false;unsigned pageReads=0;
    template<class T> void set(uint64_t at,T value) {
        for(unsigned i=0;i<6;++i)if(at>=bases[i] && at-bases[i]<=4096-sizeof(T)) {
            std::memcpy(data[i].data()+at-bases[i],&value,sizeof(value));return;
        }
        assert(false);
    }
    Image() {
        set(d,v);set(d+0x20,uint32_t(0x10));set(d+0x50,uint64_t(8192));
        set(d+0x88,uint32_t(1));set(d+0x90,e);
        set(v+0x130,authority.dmaMethod);set(v+0x138,authority.physicalMethod);
        set(e+0x10,p);set(e+0x18,uint64_t(120));set(p+0x28,uint32_t(4));
        for(unsigned i=0;i<4;++i)set(p+48+8*i,uint64_t(0x12345+i));
        PreparedPhysicalCacheBlock block{};block.upl=u;set(p+80,block);
    }
    bool read(uint64_t at,void* out,uint32_t bytes) {
        if(deny)return false;
        if(change && at==p+48 && ++pageReads==2)set(p+48,uint64_t(0x54321));
        for(unsigned i=0;i<6;++i)if(at>=bases[i] && bytes<=4096 && at-bases[i]<=4096-bytes) {
            std::memcpy(out,data[i].data()+at-bases[i],bytes);return true;
        }
        return false;
    }
    PreparedPhysicalCacheSample run(uint64_t offset=0) {
        return observePreparedPhysicalCache(d,offset,authority,
            [&](uint64_t at,void* out,uint32_t bytes){return read(at,out,bytes);},[&](){return alive;});
    }
};
int main() {
    Image a;auto r=a.run();assert(r.flags==15 && r.physical==0x12345000 && r.bytes==4096);
    r=a.run(4096);assert(r.flags==15 && r.physical==0x12346000);
    r=a.run(37);assert(r.flags==15 && r.physical==0x12345025 && r.bytes==4059);
    Image b;b.set(Image::e+0x18,uint64_t(160));
    PreparedPhysicalCacheBlock second{};second.upl=Image::u;second.pageInfo=2;second.offset=4096;
    b.set(Image::p+120,second);r=b.run(4196);assert(r.flags==15 && r.physical==0x12347064);
    Image c;c.set(Image::p+0x28,uint32_t(0));c.set(Image::e+0x18,uint64_t(88));
    PreparedPhysicalCacheBlock external{};external.upl=Image::u;external.pageInfo=Image::x;external.flags=2;
    c.set(Image::p+48,external);c.set(Image::x,uint64_t(0x789));c.set(Image::x+8,uint64_t(0x78a));
    r=c.run(4096);assert(r.flags==15 && r.physical==0x78a000);
    unsigned rejected=0;
    const char* reasons[]={"admission","descriptor-not-wired","physical-method","dma-method",
        "descriptor-length","descriptor-length","descriptor-type","cache-data","cache-block-layout",
        "cache-pages","block-layout","block-layout","page-pair","page-pair","admission","descriptor-vtable"};
    for(unsigned test=0;test<16;++test) {
        Image bad;
        switch(test) {
        case 0:bad.authority.exactLayout=false;break;
        case 1:bad.set(Image::d+0x88,uint32_t(0));break;
        case 2:bad.set(Image::v+0x138,uint64_t(0));break;
        case 3:bad.set(Image::v+0x130,uint64_t(0));break;
        case 4:bad.set(Image::d+0x50,uint64_t(0));break;
        case 5:bad.set(Image::d+0x50,uint64_t(0x400001));break;
        case 6:bad.set(Image::d+0x20,uint32_t(0x20));break;
        case 7:bad.set(Image::e+0x10,uint64_t(0x1000));break;
        case 8:bad.set(Image::e+0x18,uint64_t(119));break;
        case 9:bad.set(Image::p+0x28,uint32_t(16385));break;
        case 10:bad.set(Image::p+80+32,uint32_t(1));break;
        case 11:bad.set(Image::p+80+28,uint32_t(4096));break;
        case 12:bad.set(Image::p+48,uint64_t(0));break;
        case 13:bad.change=true;break;
        case 14:bad.alive=false;break;
        case 15:bad.deny=true;break;
        }
        const auto refused=bad.run();
        assert(refused.flags==0 && std::strcmp(refused.reason,reasons[test])==0);++rejected;
    }
    r=a.run(8192);assert(r.flags==0 && std::strcmp(r.reason,"descriptor-offset")==0 && rejected==16);
    r=a.run();assert(r.options==0x10 && r.wired==1 && r.descriptorBytes==8192 && r.cacheBytes==120 && r.pages==4 && r.blocks==1);
    const uint64_t root=Image::d+0x200,table=Image::v+0x200,list=Image::e+0x200;
    const uint64_t multi=0xffff800000900200ULL,sub=0xffff800000900300ULL,getLength=0xffff800000900400ULL;
    PreparedDescriptorGraphAuthority graph{a.authority,multi,sub,getLength,true};
    const auto setup=[&](Image& image,bool isSub) {
        image.set(Image::v+0x1c8,getLength);image.set(table+0x1c8,getLength);
        image.set(root,table);image.set(table+0x138,isSub?sub:multi);
        image.set(root+0x50,uint64_t(isSub?4096:8192));
        image.set(root+0x60,isSub?Image::d:list);
        if(isSub)image.set(root+0x68,uint64_t(4096));
        else {image.set(root+0x68,uint32_t(1));image.set(list,Image::d);}
    };
    const auto runGraph=[&](Image& image,uint64_t at=0) {
        return observePreparedDescriptorCache(root,at,graph,
            [&](uint64_t address,void* out,uint32_t bytes){return image.read(address,out,bytes);},
            [&](){return image.alive;});
    };
    Image m;setup(m,false);r=runGraph(m,37);assert(r.flags==15 && r.physical==0x12345025 && r.bytes==4059 && r.graphKind==2 && r.graphDepth==2);
    Image s;setup(s,true);r=runGraph(s);assert(r.flags==15 && r.physical==0x12346000 && r.bytes==4096 && r.graphKind==3 && r.graphDepth==2);
    Image siblings;setup(siblings,false);siblings.set(root+0x50,uint64_t(16384));
    siblings.set(root+0x68,uint32_t(2));siblings.set(list+8,Image::d);
    r=runGraph(siblings,8192);assert(r.flags==15 && r.physical==0x12345000);
    Image nested;setup(nested,true);nested.set(root+0x68,uint64_t(0));
    for(unsigned i=1;i<=3;++i) {
        const uint64_t at=root+uint64_t(i)*0x100;
        nested.set(at,table);nested.set(at+0x50,uint64_t(4096));nested.set(at+0x68,uint64_t(0));
        nested.set(at+0x60,Image::d);
    }
    nested.set(root+0x60,root+0x100);nested.set(root+0x100+0x60,root+0x200);
    r=runGraph(nested);assert(r.flags==15);
    nested.set(root+0x200+0x60,root+0x300);
    r=runGraph(nested);assert(r.flags==0 && std::strcmp(r.reason,"graph-depth")==0);
    Image partial;setup(partial,true);partial.set(root+0x50,uint64_t(17));
    r=runGraph(partial,3);assert(r.flags==15 && r.bytes==14 && r.physical==0x12346003);
    for(unsigned test=0;test<10;++test) {
        Image bad;setup(bad,false);
        switch(test) {
        case 0:bad.set(table+0x138,uint64_t(0xffff800000999000ULL));break;
        case 1:bad.set(table+0x1c8,uint64_t(0));break;
        case 2:bad.set(root+0x68,uint32_t(65));break;
        case 3:bad.set(root+0x68,uint32_t(0));break;
        case 4:bad.set(list,uint64_t(0));break;
        case 5:bad.set(root+0x50,uint64_t(4096));break;
        case 6:bad.set(list,root);break;
        case 7:setup(bad,true);bad.set(root+0x68,UINT64_MAX);break;
        case 8:setup(bad,true);bad.set(root+0x60,root);break;
        case 9:bad.set(Image::v+0x1c8,uint64_t(0));break;
        }
        assert(runGraph(bad).flags==0);
    }
    Image unstable;setup(unstable,true);bool changed=false;
    r=observePreparedDescriptorCache(root,0,graph,[&](uint64_t address,void* out,uint32_t bytes) {
        const bool ok=unstable.read(address,out,bytes);
        if(address==Image::p+56 && !changed){unstable.set(root+0x68,uint64_t(0));changed=true;}
        return ok;
    },[&](){return unstable.alive;});
    assert(changed && r.flags==0 && r.bytes==0 && std::strcmp(r.reason,"graph-unstable")==0);
}
'''
with tempfile.TemporaryDirectory(prefix='ngreen-prepared-physical-cache-') as directory:
    task = Path(directory)
    (task/'test.cpp').write_text(prepared_cache_test)
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I',str(root),str(task/'test.cpp'),'-o',str(task/'test')],check=True,capture_output=True,timeout=30)
    subprocess.run([str(task/'test')],check=True,capture_output=True,timeout=10)
print('PASS actual prepared physical-cache reader plus exact-length Multi/Sub selected-path graph: nested/sibling/partial/depth/cycle/foreign/overflow/unstable tests; owned CPU fixtures only')

# Compile the actual production binding with exact reviewed native bytes.
graph_binding = source[source.index('const auto preparedMulti='):source.index('const auto cpuLookup=', source.index('const auto preparedMulti='))]
graph_fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "NootedGreen/kern_flight_trace.hpp"
std::vector<uint8_t> multi,sub,length;
void decode(const char* text,std::vector<uint8_t>& out) {
 for(unsigned i=0;text[i];i+=2){unsigned v=0;for(unsigned j=0;j<2;++j){char c=text[i+j];v=v*16+(c<='9'?c-'0':c-'a'+10);}out.push_back(uint8_t(v));}
}
struct KernelPatcher{static constexpr int KernelID=0;};
int absent=0;bool preparedLayout=true;int observation=-1;
struct Patcher {
 uintptr_t solveSymbol(int,const char* name) {
  if(std::strstr(name,"IOMulti"))return absent==1?0:reinterpret_cast<uintptr_t>(multi.data());
  if(std::strstr(name,"IOSub"))return absent==2?0:reinterpret_cast<uintptr_t>(sub.data());
  assert(std::strstr(name,"getLength"));return absent==3?0:reinterpret_cast<uintptr_t>(length.data());
 }
}patcher;
NGFlightTrace::PreparedPhysicalCacheAuthority gVfPreparedPhysicalCacheAuthority{1,1,true};
NGFlightTrace::PreparedDescriptorGraphAuthority gVfPreparedDescriptorGraphAuthority{};
bool anchorMatches(uintptr_t at,const uint8_t* bytes,unsigned n){return !std::memcmp(reinterpret_cast<void*>(at),bytes,n);}
#define SYSLOG(module,format,value) observation=(value)
void check(){gVfPreparedDescriptorGraphAuthority={}; __PRODUCTION__ }
int main(){decode("__MULTI__",multi);decode("__SUB__",sub);decode("554889e5488b47505dc3",length);
 check();assert(observation==1 && gVfPreparedDescriptorGraphAuthority.exactLayout);
 for(auto* bytes:{&multi,&sub,&length})for(unsigned i=0;i<bytes->size();++i){(*bytes)[i]^=1;check();assert(!observation && !gVfPreparedDescriptorGraphAuthority.exactLayout);(*bytes)[i]^=1;}
 for(absent=1;absent<=3;++absent){check();assert(!observation);}absent=0;
 preparedLayout=false;check();assert(!observation);preparedLayout=true;check();assert(observation==1);
}
'''.replace('__PRODUCTION__',graph_binding).replace('__MULTI__','554889e5415741564155415453504889d3837f680074524189ce4989f74989fc4531ed666666662e0f1f840000000000498b4424604a8b3ce8488b07ff90c8010000498b4c24604a8b3ce9488b0f4939c77233ff91c80100004929c749ffc5418b4424684939c572c74885db740748c7030000000031c04883c4085b415c415d415e415f5dc3488b81380100004c89fe4889da4489f14883c4085b415c415d415e415f5dffe0').replace('__SUB__','554889e54157415653504889d34885d2740748c70300000000483b7750734e48c745e000000000488b47604989ff4c8b47684989f64901f04c8b08488d55e04889c74c89c641ff91380100004885db741e4885c07419498b4f504c29f1488b55e04839d1480f43ca48890beb0231c04883c4085b415e415f5dc3')
with tempfile.TemporaryDirectory(prefix='ngreen-descriptor-graph-binding-') as directory:
    task=Path(directory);(task/'test.cpp').write_text(graph_fixture)
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root),str(task/'test.cpp'),'-o',str(task/'test')],check=True,capture_output=True,timeout=30)
    subprocess.run([str(task/'test')],check=True,capture_output=True,timeout=10)
print('PASS actual native descriptor graph binding: all298 native-byte mutations and missing symbols/base-layout refusals; owned CPU only')
