#!/usr/bin/env python3
"""Pin Tahoe boolean submission boundaries and VF fail-stop propagation."""

import hashlib
import struct
import sys
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


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: vf_submission_result_contract_test.py "
            "source compiled-object payload payload"
        )
    source_mutations(Path(sys.argv[1]))
    object_contract(Path(sys.argv[2]))
    for argument in sys.argv[3:]:
        payload_contract(Path(argument))


if __name__ == "__main__":
    main()
