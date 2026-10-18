#!/usr/bin/env python3
"""Pin Tahoe boolean submission boundaries and VF fail-stop propagation."""

import hashlib
import struct
import sys
import subprocess
import tempfile
from pathlib import Path

from route_symbol_contract_test import macho_symbols, single_symbol
from vf_event_vector_callgraph_contract_test import macho_layout


SUBMIT_BLIT = (
    "__ZN16IntelAccelerator10submitBlitEP15blit3d_params_tRK8IGVectorI"
    "11rect_pair_t25IGIOMallocAllocatorPolicyEP11IGAccelTaskb"
)
SUBMIT_CCS = (
    "__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER"
    "16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh"
)
SUBMIT_DEPTH = (
    "__ZN15IGAccelResource18submitDepthResolveEPNS_17ResourceInfoEntryER"
    "16IntelAcceleratorP11IGAccelTask17EIntelResolveTypehhtt"
)

EXPECTED_CALLS = {
    SUBMIT_BLIT: (
        0x6E38, 0x2D0A6, 0x55B04, 0x5A8B8, 0x5CE06, 0x5D53D,
        0x5E145, 0x5ED6A, 0x602F2, 0x60E54, 0x61590, 0x62299,
        0x667DE, 0x669B9, 0x66BB2, 0x704BD, 0x706B8, 0x70973,
        0x71330, 0x71538, 0x750E4, 0x795B5, 0x797DE, 0x820DD,
        0x82A65, 0x8351E,
    ),
    SUBMIT_CCS: (0x503DE, 0x739D6, 0x740D8, 0x78F23),
    SUBMIT_DEPTH: (0x4F1C5, 0x50105, 0x5227C, 0x534F3, 0x73B83),
}

EXPECTED_BODIES = {
    SUBMIT_BLIT: (0x5D6, "2c824c92f89bba013df05d127d6d81d677e39a62d6233c04786bc3f33fa81ef5"),
    SUBMIT_CCS: (0x554, "a10208dea187f2099cf0deab208c392b50c600d292f2604d6fffea3b7aacc528"),
    SUBMIT_DEPTH: (0x3F0, "a3f33094b424e909e708e33b10b3be2d6abf6e3682dda5fa273331de867febde"),
}


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start:offset + 1]
    raise AssertionError(f"unterminated source function: {signature}")


def source_contract(source: str, path: Path) -> None:
    compact = "".join(source.split())
    assert 'constboolcaptureOnlyLockReady=captureOnlyArmed&&gVfCaptureStreamResetBound&&this->oVfMediaEnd&&gVfMediaPublicationRouteReady&&gVfMediaEndCaller&&vfEnsurePageTableUpdateLock();' in compact
    assert 'captureOnlyLockReady&&gVfPageTableUpdateLock?1U:0U' in compact
    work = function_body(source, "bool Gen11::vfSubmitWorkItem(")
    guard = work[work.index("if (NGVfSubmission::captureOnlyRejectVideo("):]
    guard = guard[:guard.index("VfContextQueueGuard queue(that);")]
    for token in ('checkKernelArgument("-ngreencaptureonly")',
                  'gVfIdentity == VfIdentity::Virtual', 'return false;', 'queued=0'):
        assert token in guard, f"{path}: incomplete prequeue capture refusal"
    assert 'return true;' not in guard
    assert work.index('captureOnlyRejectVideo(') < work.index('VfContextQueueGuard queue(that);')
    media = function_body(source,'void Gen11::vfMediaEnd(')
    cancel = function_body(media,'if(NGVfSubmission::captureOnlyStopBeforeNativeEnd(')
    for token in ('checkKernelArgument("-ngreencaptureonly")','gVfIdentity==VfIdentity::Virtual',
                  'vfObservePreEndCpuState(*entry,span,count);','NGVfSubmission::cancelCaptureStream(',
                  'span.offset,count,savedSideband,span.used,gVfCaptureStreamResetBound && live(count)',
                  'entry->failed=true;','entry->captureCancelled=true;','stamp-advanced=0','return;'):
        assert token in cancel, f'{path}: incomplete pre-stamp cancellation'
    assert 'FunctionCast' not in cancel
    assert media.index(cancel)<media.rindex('FunctionCast(vfMediaEnd,callback->oVfMediaEnd)')
    assert 'if (entry->failed && !entry->captureCancelled) return;' in media
    outer = function_body(source,'IOReturn Gen11::vfContextSubmit(')
    assert 'return mediaScope.failed()?kIOReturnBadArgument:result;' in outer
    route_tokens = (
        f'{{"{SUBMIT_BLIT}",submitBlit,this->osubmitBlit}}',
        f'{{"{SUBMIT_CCS}",vfSubmitCCSResolve,this->oVfSubmitCCSResolve}}',
        f'{{"{SUBMIT_DEPTH}",vfSubmitDepthResolve,this->oVfSubmitDepthResolve}}',
    )
    for token in route_tokens:
        assert token in compact, f"{path}: missing VF result boundary route"

    helper = "".join(function_body(
        source, "static bool vfRequireSuccessfulNativeSubmission(").split())
    for token in (
        "if(submitted)returntrue;",
        "vfMarkProtocolFault(failure);",
        'PANIC_COND(true,"ngreen","%s",failure);',
        "returnfalse;",
    ):
        assert token in helper, f"{path}: incomplete submission fail-stop helper"
    assert helper.index("vfMarkProtocolFault(failure);") < helper.index(
        'PANIC_COND(true,"ngreen","%s",failure);')

    blit = "".join(function_body(source, "bool Gen11::submitBlit(").split())
    for token in (
        "if(gVfIdentity!=VfIdentity::Virtual)",
        "constuint64_tcount=*reinterpret_cast<constuint64_t*>(param_2);",
        "if(count==0)",
        "if(!vfNativeGpuWorkReady())",
        "void*blit2D=getBlit2DContext(param_3,true);",
        "void*blit3D=getBlit3DContext(param_3,true);",
        "constboolsubmitted=FunctionCast(submitBlit,callback->osubmitBlit)",
        '"NativeVFblitreturnedwithoutpublishingnon-emptywork"',
    ):
        assert token in blit, f"{path}: incomplete VF blit result boundary: {token}"
    assert blit.index("if(count==0)") < blit.index("if(!vfNativeGpuWorkReady())") < \
        blit.index("constboolsubmitted=") < blit.index(
            '"NativeVFblitreturnedwithoutpublishingnon-emptywork"')

    ccs = "".join(function_body(
        source, "bool Gen11::vfSubmitCCSResolve(").split())
    depth = "".join(function_body(
        source, "bool Gen11::vfSubmitDepthResolve(").split())
    for body, trampoline, forwarded, failure in (
            (ccs, "oVfSubmitCCSResolve",
             "native(that,entry,accelerator,task,resolveType,plane,level)",
             '"NativeVFCCSresolvereturnedfalseafteradmission"'),
            (depth, "oVfSubmitDepthResolve",
             "native(that,entry,accelerator,task,resolveType,plane,level,"
             "width,height)",
             '"NativeVFdepthresolvereturnedfalseafteradmission"')):
        for token in (
            f"callback->{trampoline}",
            "if(gVfIdentity!=VfIdentity::Virtual)",
            "accelerator!=gVfAccelerator",
            "!vfNativeGpuWorkReady()",
            f"return{forwarded};",
            f"constboolsubmitted={forwarded};",
            failure,
        ):
            assert token in body, \
                f"{path}: incomplete VF resolve result boundary: {token}"
        assert body.index("accelerator!=gVfAccelerator") < \
            body.index(f"constboolsubmitted={forwarded};") < body.index(failure)


def source_mutations(path: Path) -> None:
    source = path.read_text(encoding="utf-8")
    source_contract(source, path)
    mutations = (
        ('gVfMediaEndCaller &&\n\t\t\t\t\t\tvfEnsurePageTableUpdateLock();',
         'gVfMediaEndCaller &&\n\t\t\t\t\t\tgVfPageTableUpdateLock;'),
        ('captureOnlyLockReady && gVfPageTableUpdateLock?1U:0U',
         'true?1U:0U'),
        ('if (NGVfSubmission::captureOnlyRejectVideo(\n\t        checkKernelArgument("-ngreencaptureonly"),',
         'if (NGVfSubmission::captureOnlyRejectVideo(\n\t        true,'),
        ('if(NGVfSubmission::captureOnlyStopBeforeNativeEnd(\n\t    checkKernelArgument("-ngreencaptureonly"),',
         'if(NGVfSubmission::captureOnlyStopBeforeNativeEnd(\n\t    true,'),
        ('span.offset,count,savedSideband,span.used,gVfCaptureStreamResetBound && live(count)',
         'span.used,count,savedSideband,span.bytes,true'),
        ('entry->failed=true;\n\t\tentry->captureCancelled=true;',
         'entry->failed=false;\n\t\tentry->captureCancelled=true;'),
        ('gVfIdentity == VfIdentity::Virtual, static_cast<uint32_t>(hwCsType)',
         'true, static_cast<uint32_t>(hwCsType)'),
        ('\t\treturn false;\n\t}\n\tVfContextQueueGuard queue(that);',
         '\t\treturn true;\n\t}\n\tVfContextQueueGuard queue(that);'),
        (f'{{"{SUBMIT_CCS}",\n\t\t\t  vfSubmitCCSResolve, this->oVfSubmitCCSResolve}},\n', ""),
        (f'{{"{SUBMIT_DEPTH}",\n\t\t\t  vfSubmitDepthResolve, this->oVfSubmitDepthResolve}},\n', ""),
        ("vfMarkProtocolFault(failure);", ""),
        ('PANIC_COND(true, "ngreen", "%s", failure);', ""),
        ("if (count == 0)", "if (false)"),
        ("if (!vfNativeGpuWorkReady())\n\t\treturn vfRequireSuccessfulNativeSubmission(false,\n\t\t\t\"VF blit submission reached a stopped transport\");",
         "if (false)\n\t\treturn false;"),
        ("const bool submitted = native(\n\t\tthat, entry, accelerator, task, resolveType, plane, level);",
         "const bool submitted = true;"),
        ("const bool submitted = native(that, entry, accelerator, task, resolveType,\n\t\tplane, level, width, height);",
         "const bool submitted = true;"),
    )
    rejected = 0
    for before, after in mutations:
        assert source.count(before) == 1, f"ambiguous mutation anchor: {before}"
        changed = source.replace(before, after, 1)
        try:
            source_contract(changed, path)
        except (AssertionError, ValueError):
            rejected += 1
            continue
        raise AssertionError(f"{path}: unsafe submission-result mutation escaped")
    print(f"PASS: {rejected} VF submission-result source mutations rejected")


def direct_references(path: Path, target: int) -> tuple[tuple[int, ...], tuple[int, ...], tuple[int, ...]]:
    data = path.read_bytes()
    text_address, text_file_offset, text_size, relocated = macho_layout(data, path)
    calls = []
    tails = []
    address_takes = []
    text_end = text_file_offset + text_size
    for offset in range(text_file_offset, text_end - 6):
        instruction = text_address + offset - text_file_offset
        if data[offset] in (0xE8, 0xE9) and not any(
                instruction + byte in relocated for byte in range(1, 5)):
            destination = instruction + 5 + struct.unpack_from(
                "<i", data, offset + 1)[0]
            if destination == target:
                (calls if data[offset] == 0xE8 else tails).append(instruction)
        if (data[offset] & 0xF8) == 0x48 and data[offset + 1] == 0x8D and \
                (data[offset + 2] & 0xC7) == 0x05 and not any(
                    instruction + byte in relocated for byte in range(3, 7)):
            destination = instruction + 7 + struct.unpack_from(
                "<i", data, offset + 3)[0]
            if destination == target:
                address_takes.append(instruction)
    return tuple(calls), tuple(tails), tuple(address_takes)


def payload_contract(path: Path) -> None:
    data = path.read_bytes()
    symbols = macho_symbols(path)
    video=single_symbol(symbols,'__ZN19IGAccelVideoContext16endCommandStreamER24IOAccelCommandStreamInfo',path)
    assert data[video+0xdb:video+0xe1]==bytes.fromhex('ff9048010000'), f'{path}: native early incrementStamp edge changed'
    assert data[video+0x337:video+0x34c]==bytes.fromhex('418b4634c1e0024101463031c04189463441894638'), \
        f'{path}: native CPU stream disposal epilogue changed'
    ring=single_symbol(symbols,'__ZN20IGHardwareRingBuffer12submitToRingEv',path)
    cold=single_symbol(symbols,'__ZN20IGHardwareRingBuffer12submitToRingEv.cold.1',path)
    assert data[ring+0x196:ring+0x1a0]==bytes.fromhex('ff904801000084c07414')
    assert data[ring+0x1b4]==0xe8 and ring+0x1b9+struct.unpack_from('<i',data,ring+0x1b5)[0]==cold
    ordered = sorted(
        value for values in symbols.values() for value in values if value
    )
    for name, expected_calls in EXPECTED_CALLS.items():
        target = single_symbol(symbols, name, path)
        calls, tails, address_takes = direct_references(path, target)
        assert calls == expected_calls, f"{path}: changed complete call inventory: {name}"
        assert not tails and not address_takes, \
            f"{path}: submission boundary gained tail/address-taken entry: {name}"
        length, digest = EXPECTED_BODIES[name]
        end = min(value for value in ordered if value > target)
        assert end - target == length, f"{path}: changed submission body boundary: {name}"
        assert hashlib.sha256(data[target:end]).hexdigest() == digest, \
            f"{path}: changed submission body: {name}"
    print(
        f"PASS {path}: 35 direct calls enter three exact boolean submission boundaries"
    )


def object_contract(path: Path) -> None:
    symbols = macho_symbols(path)
    for name in (
        "__ZN5Gen1118vfSubmitCCSResolveEPvS0_S0_S0_jhh",
        "__ZN5Gen1120vfSubmitDepthResolveEPvS0_S0_S0_jhhtt",
    ):
        single_symbol(symbols, name, path)
    print(
        f"PASS {path}: compiled exact x86_64 CCS/depth ABI symbols; "
        "source forwarding locked"
    )


def capture_readiness_owned_test(source: str,path: Path) -> None:
    start=source.index('const bool captureOnlyLockReady=')
    expression=source[start:source.index(';',start)+1]
    fixture=r'''
#include <cassert>
bool captureOnlyArmed=true,gVfCaptureStreamResetBound=true;
bool gVfMediaPublicationRouteReady=true,gVfMediaEndCaller=true;
bool gVfPageTableUpdateLock=false,sleepable=true,allocation=true;
unsigned attempts=0,allocations=0;
bool vfEnsurePageTableUpdateLock(){
 ++attempts;if(!sleepable)return false;
 if(gVfPageTableUpdateLock)return true;
 if(!allocation)return false;
 ++allocations;gVfPageTableUpdateLock=true;return true;
}
struct Binding { bool oVfMediaEnd=true;
 bool ready(){EXPRESSION
 return captureOnlyLockReady && gVfPageTableUpdateLock;}
};
int main(){
 Binding b;
 assert(!gVfPageTableUpdateLock && b.ready() && allocations==1);
 assert(b.ready() && allocations==1);
 sleepable=false;assert(!b.ready());sleepable=true;
 gVfPageTableUpdateLock=false;allocation=false;assert(!b.ready());
 allocation=true;
 for(unsigned i=0;i<5;++i){
  captureOnlyArmed=gVfCaptureStreamResetBound=gVfMediaPublicationRouteReady=gVfMediaEndCaller=b.oVfMediaEnd=true;
  if(i==0)captureOnlyArmed=false;if(i==1)gVfCaptureStreamResetBound=false;
  if(i==2)gVfMediaPublicationRouteReady=false;if(i==3)gVfMediaEndCaller=false;
  if(i==4)b.oVfMediaEnd=false;
  unsigned before=attempts;assert(!b.ready() && attempts==before);
 }
}
'''.replace('EXPRESSION',expression)
    with tempfile.TemporaryDirectory(prefix='ngreen-capture-readiness-') as directory:
        cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test'
        cpp.write_text(fixture)
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('PASS actual capture readiness expression: lazy lock, existing lock, denied context/allocation and five closed gates; CPU model only')


def capture_pre_end_owned_test(source: str,path: Path) -> None:
    media=function_body(source,'void Gen11::vfMediaEnd(')
    guard=function_body(media,'if(NGVfSubmission::captureOnlyStopBeforeNativeEnd(')
    fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "kern_vf_submission_gate.hpp"
enum class VfIdentity {Unknown,PF,Virtual};
VfIdentity gVfIdentity=VfIdentity::Virtual;
bool enabled=true,gVfCaptureStreamResetBound=true,ownerLive=true;
unsigned observed=0,nativeCalls=0;
struct Entry{bool failed=false,captureCancelled=false;} owner,*entry=&owner;
struct Span{uint32_t offset=256,used=512;} span;
struct Publication{struct {uint32_t words=16;} preparation;} publication;
alignas(uint32_t) uint8_t storage[64]{};
void *stream=storage;
template<class T>T& getMember(void *p,size_t o){return *reinterpret_cast<T *>(static_cast<uint8_t *>(p)+o);}
volatile uint32_t &words=getMember<volatile uint32_t>(stream,0x34);
bool checkKernelArgument(const char *s){assert(!strcmp(s,"-ngreencaptureonly"));return enabled;}
bool live(uint32_t count){return ownerLive && words==count;}
void vfObservePreEndCpuState(const Entry &,const Span &,uint32_t count){assert(count==publication.preparation.words);++observed;}
struct Refused{};
#define PANIC_COND(condition,...) do{if(condition)throw Refused{};}while(0)
#define SYSLOG(...) ((void)0)
void reset(){
 memset(storage,0xa5,sizeof(storage));owner={};span={};publication={};observed=nativeCalls=0;
 enabled=ownerLive=gVfCaptureStreamResetBound=true;gVfIdentity=VfIdentity::Virtual;
 getMember<volatile uint32_t>(stream,0x30)=256;words=16;
 getMember<volatile uint32_t>(stream,0x38)=7;
}
void production(){
'''
    fixture+=guard+r'''
 ++nativeCalls;
}
int main(){
 reset();uint8_t before[64];memcpy(before,storage,sizeof(storage));production();
 assert(!nativeCalls && observed==1 && owner.failed && owner.captureCancelled);
 assert(getMember<volatile uint32_t>(stream,0x30)==320 && !words && !getMember<volatile uint32_t>(stream,0x38));
 for(unsigned i=0;i<64;++i)if(i<0x30 || i>=0x3c)assert(before[i]==storage[i]);
 // A later CPU stream in the same failed scope is disposed, never submitted.
 span.offset=320;getMember<volatile uint32_t>(stream,0x30)=320;words=16;production();
 assert(!nativeCalls && observed==2 && owner.failed && owner.captureCancelled);
 assert(getMember<volatile uint32_t>(stream,0x30)==384 && !words);
 for(unsigned test=0;test<3;++test){
  reset();if(test==0)enabled=false;else gVfIdentity=test==1?VfIdentity::PF:VfIdentity::Unknown;
  production();assert(nativeCalls==1 && !observed && !owner.failed && !owner.captureCancelled && words==16);
 }
 for(unsigned test=0;test<4;++test){
  reset();if(test==0)ownerLive=false;if(test==1)gVfCaptureStreamResetBound=false;
  if(test==2)span.offset=260;if(test==3)span.used=300;
  bool refused=false;try{production();}catch(const Refused&){refused=true;}
  assert(refused && !nativeCalls && !owner.failed && !owner.captureCancelled && words==16);
  assert(getMember<volatile uint32_t>(stream,0x30)==256);
 }
}
'''
    with tempfile.TemporaryDirectory(prefix='ngreen-pre-end-cancel-') as directory:
        cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test'
        cpp.write_text(fixture)
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-I'+str(path.parent),str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('PASS actual pre-end cancellation: error latch, repeated CPU stream disposal, native pass-through, untouched neighbouring fields; no VM/GPU')


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: vf_submission_result_contract_test.py "
            "source compiled-object payload payload"
        )
    source_mutations(Path(sys.argv[1]))
    capture_readiness_owned_test(Path(sys.argv[1]).read_text(),Path(sys.argv[1]))
    capture_pre_end_owned_test(Path(sys.argv[1]).read_text(),Path(sys.argv[1]))
    object_contract(Path(sys.argv[2]))
    for argument in sys.argv[3:]:
        payload_contract(Path(argument))


if __name__ == "__main__":
    main()
