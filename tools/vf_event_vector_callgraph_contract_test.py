#!/usr/bin/env python3
"""Pin every direct call into the eight Tahoe event-vector grow copies."""
import struct
import sys
from pathlib import Path

from route_symbol_contract_test import macho_symbols


MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_DYSYMTAB = 0xB

EXPECTED = {
    0x7396: (
        (0x6922, "__ZN16IGAccel2DContext8blitCopyEP12IOAccelEventP16IOAccelResource2S3_P22IOAccel2DBlitRectStrucj", (0x6979, 0x699B, 0x6ABB)),
        (0x6EB2, "__ZN16IGAccel2DContext8blitFillEP12IOAccelEventjP16IOAccelResource2P22IOAccel2DBlitRectStrucj", (0x6F21, 0x6F40, 0x7030)),
        (0x740E, "__ZL20AddSrcResourceEventsR18wait_update_eventsP15IGAccelResource", (0x7441, 0x7486, 0x74BF)),
        (0x74E8, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x7521, 0x7563, 0x75B1, 0x75F1, 0x762F)),
    ),
    0x2EA12: (
        (0x2C84C, "__ZN16IntelAccelerator16submitCCSResolveEP15IGAccelResourceP22color_resolve_params_tRK8IGVectorI11blit_rect_t25IGIOMallocAllocatorPolicyEP11IGAccelTask", (0x2CA93,)),
        (0x2CAFC, "__ZN16IntelAccelerator14submitSwapCopyEP12IOAccelEventP16IOAccelResource2S3_", (0x2CB6B, 0x2CB8A, 0x2CC5A, 0x2CC9F, 0x2CD1A, 0x2CD5A, 0x2CD96)),
    ),
    0x55B84: (
        (0x55420, "__Z25process_MSAABufferResolveR26IGAccelSegmentResourceListR24IGAccelCommandDescriptorR22IOGraphicsAccelerator2RKN14IntelMTLRender20sResolveResourceDescES8_NS5_14eResolveFilterE10BufferTypeR12IOAccelEvent", (0x555C7, 0x555E6, 0x55752)),
        (0x55BFC, "__ZL20AddSrcResourceEventsR18wait_update_eventsP15IGAccelResource", (0x55C2F, 0x55C74, 0x55CAD)),
        (0x55CD6, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x55D0F, 0x55D51, 0x55D9F, 0x55DDF, 0x55E1D)),
    ),
    0x5B018: (
        (0x59950, "__ZN21IntelMTLBlitFunctions7executeEN12IntelMTLBlit7eTokensER19IGAccelCommandQueueR26IGAccelSegmentResourceListRK20IOAccelKernelCommandR24IGAccelCommandDescriptorR13IGHeapsAccessR22IOGraphicsAccelerator2R17IGHardwareContextR12IOAccelEvent", (0x59AA0, 0x59ABF, 0x59DE0, 0x59DFF, 0x5A48D, 0x5A820, 0x5A86A)),
        (0x5B090, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x5B0C9, 0x5B10B, 0x5B159, 0x5B199, 0x5B1D7)),
        (0x5B202, "__ZL20AddSrcResourceEventsR18wait_update_eventsP15IGAccelResource", (0x5B235, 0x5B27A, 0x5B2B3)),
    ),
    0x6B8B8: (
        (0x5C8DE, "__ZN16IGAccelGLContext27process_token_TexSubImage2DER24IOAccelCommandStreamInfo", (0x5CA7B, 0x5CA9B)),
        (0x5CF82, "__ZN16IGAccelGLContext27process_token_BufferSubDataER24IOAccelCommandStreamInfo", (0x5D092, 0x5D0B1)),
        (0x5D648, "__ZN16IGAccelGLContext27process_token_CopyPixelsSrcER24IOAccelCommandStreamInfo", (0x5DB8C, 0x5DBAB, 0x5DCDC)),
        (0x5E2BE, "__ZN16IGAccelGLContext30process_token_CopyPixelsSrcFBOER24IOAccelCommandStreamInfo", (0x5E7F9, 0x5E81B)),
        (0x5EF00, "__ZN16IGAccelGLContext29process_token_BlitFramebufferER24IOAccelCommandStreamInfo", (0x5F0AB, 0x5F0CA, 0x5F4E2, 0x5F8D3)),
        (0x60548, "__ZN16IGAccelGLContext33process_token_AsyncReadDrawBufferER24IOAccelCommandStreamInfo", (0x605FF, 0x6061B, 0x60AD7)),
        (0x60EF4, "__ZN16IGAccelGLContext26process_token_TexPBOUploadER24IOAccelCommandStreamInfo", (0x610AE, 0x610CE)),
        (0x616E4, "__ZN16IGAccelGLContext28process_token_BlitLibSetup2DER24IOAccelCommandStreamInfo", (0x6172C, 0x6174B, 0x61ECD, 0x620CF)),
        (0x65E10, "__ZN16IGAccelGLContext29process_token_GenerateMipmapsER24IOAccelCommandStreamInfo", (0x65F08, 0x65F27)),
        (0x66DF0, "__ZN16IGAccelGLContext32process_token_ResolveDepthBufferER24IOAccelCommandStreamInfo", (0x66E5D, 0x66E7C, 0x66F60)),
        (0x6B745, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x6B77E, 0x6B7C0, 0x6B80E, 0x6B84E, 0x6B88C)),
        (0x6B930, "__ZL20AddSrcResourceEventsR18wait_update_eventsP15IGAccelResource", (0x6B963, 0x6B9A8, 0x6B9E1)),
    ),
    0x757B2: (
        (0x6FBB6, "__ZN15IGAccelResource6pageonEP12IOAccelEventby", (0x6FDD7, 0x6FDF6, 0x6FEDB, 0x6FF1F, 0x6FF5F, 0x70061, 0x700A4, 0x700E4)),
        (0x70B82, "__ZN15IGAccelResource7pageoffEP12IOAccelEventbPby", (0x70CB5, 0x70CD4, 0x70DBB, 0x70DFB, 0x70E37, 0x70F3D, 0x70F7D, 0x70FBC)),
        (0x73A20, "__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh", (0x73ACC, 0x73AE8)),
        (0x7415E, "__ZN15IGAccelResource18submitDepthResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask17EIntelResolveTypehhtt", (0x741EB, 0x7420B)),
        (0x7454E, "__ZN15IGAccelResource15generateMipMapsEttttjjjjjyRK22SGfxRenderSurfaceState", (0x74625, 0x74647, 0x74724, 0x7476D, 0x747C2, 0x7480C, 0x74848, 0x748D4, 0x7491D, 0x74972, 0x749BC, 0x74A08)),
        (0x7582A, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x75863, 0x758A5, 0x758F3, 0x75933, 0x75971)),
    ),
    0x79A2E: (
        (0x78434, "__ZN23IGAccelSharedUserClient13depth_resolveEPvy", (0x7849D, 0x784B9)),
        (0x79022, "__ZN23IGAccelSharedUserClient13icbBufferBlitEPvy", (0x79200, 0x7921C)),
        (0x79AA6, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x79AE3, 0x79B25, 0x79B73, 0x79BB3, 0x79BF1)),
    ),
    0x844D4: (
        (0x8195A, "__ZN14IGAccelSurface13copyBufferDMAEiiiijjP12IOAccelEventP16IOAccelResource2yP16IOAccelSysMemoryyyjj", (0x81C5F, 0x81C81, 0x8203E, 0x8208B)),
        (0x82316, "__ZN14IGAccelSurface15submitSwapFlushEP12IOAccelEventP16IOAccelResource2S3_PK19IOAccelSwapFlushRecjjjPK13IOAccelBounds", (0x8242C, 0x8244B, 0x82662)),
        (0x830B4, "__ZN14IGAccelSurface17submitCopyForwardEP12IOAccelEventjP16IOAccelResource2S3_PK13IOAccelBoundsj", (0x831A0, 0x831BF, 0x832DD)),
        (0x8454C, "__ZL20AddSrcResourceEventsR18wait_update_eventsP15IGAccelResource", (0x8457F, 0x845C4, 0x845FD)),
        (0x84626, "__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", (0x8465F, 0x846A1, 0x846EF, 0x8472F, 0x8476D)),
    ),
}

INITIAL_REQUESTS = {
    0x7396: (0x6979, 0x699B, 0x6F21, 0x6F40),
    0x2EA12: (0x2CB6B, 0x2CB8A),
    0x55B84: (0x555C7, 0x555E6),
    0x5B018: (0x59AA0, 0x59ABF, 0x59DE0, 0x59DFF),
    0x6B8B8: (0x5CA7B, 0x5CA9B, 0x5D092, 0x5D0B1, 0x5DB8C, 0x5DBAB,
              0x5E7F9, 0x5E81B, 0x5F0AB, 0x5F0CA, 0x605FF, 0x6061B,
              0x610AE, 0x610CE, 0x6172C, 0x6174B, 0x65F08, 0x65F27,
              0x66E5D, 0x66E7C),
    0x757B2: (0x6FDD7, 0x6FDF6, 0x70CB5, 0x70CD4, 0x73ACC, 0x73AE8,
              0x741EB, 0x7420B, 0x74625, 0x74647),
    0x79A2E: (0x7849D, 0x784B9, 0x79200, 0x7921C),
    0x844D4: (0x81C5F, 0x81C81, 0x8242C, 0x8244B, 0x831A0, 0x831BF),
}


def macho_layout(data: bytes, path: Path) -> tuple[int, int, int, set[int]]:
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != MH_MAGIC_64:
        raise AssertionError(f"{path}: not a little-endian Mach-O 64 payload")
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    cursor = 32
    end = cursor + sizeofcmds
    text = None
    external = None
    for _ in range(ncmds):
        command, command_size = struct.unpack_from("<II", data, cursor)
        if command_size < 8 or cursor + command_size > end:
            raise AssertionError(f"{path}: malformed load command")
        if command == LC_SEGMENT_64:
            segment = data[cursor + 8:cursor + 24].split(b"\0", 1)[0]
            vmaddr, _, file_offset, file_size = struct.unpack_from("<QQQQ", data, cursor + 24)
            if segment == b"__TEXT":
                if file_offset > len(data) or file_size > len(data) - file_offset:
                    raise AssertionError(f"{path}: __TEXT escapes file")
                text = (vmaddr, file_offset, file_size)
        elif command == LC_DYSYMTAB:
            if command_size != 80 or external is not None:
                raise AssertionError(f"{path}: malformed or duplicate LC_DYSYMTAB")
            fields = struct.unpack_from("<18I", data, cursor + 8)
            external = (fields[14], fields[15])
        cursor += command_size
    if text is None or external is None:
        raise AssertionError(f"{path}: missing __TEXT or LC_DYSYMTAB")
    external_offset, external_count = external
    if external_offset > len(data) or external_count > (len(data) - external_offset) // 8:
        raise AssertionError(f"{path}: external relocation table escapes file")
    relocated = {
        struct.unpack_from("<i", data, external_offset + index * 8)[0]
        for index in range(external_count)
    }
    return *text, relocated


def direct_calls(path: Path) -> tuple[dict[int, tuple[int, ...]], tuple[int, ...], tuple[int, ...]]:
    data = path.read_bytes()
    vmaddr, file_offset, file_size, relocated = macho_layout(data, path)
    found = {target: [] for target in EXPECTED}
    tail_jumps = []
    address_takes = []
    end = file_offset + file_size
    for offset in range(file_offset, end - 6):
        call = vmaddr + offset - file_offset
        if data[offset] in (0xE8, 0xE9):
            if any(call + byte in relocated for byte in range(1, 5)):
                continue
            target = call + 5 + struct.unpack_from("<i", data, offset + 1)[0]
            if target in found:
                if data[offset] == 0xE8:
                    found[target].append(call)
                else:
                    tail_jumps.append(call)
        if (data[offset] & 0xF8) == 0x48 and data[offset + 1] == 0x8D and \
                (data[offset + 2] & 0xC7) == 0x05:
            if any(call + byte in relocated for byte in range(3, 7)):
                continue
            target = call + 7 + struct.unpack_from("<i", data, offset + 3)[0]
            if target in found:
                address_takes.append(call)
    return ({target: tuple(calls) for target, calls in found.items()},
            tuple(tail_jumps), tuple(address_takes))


def verify(path: Path) -> int:
    data = path.read_bytes()
    vmaddr, file_offset, _, _ = macho_layout(data, path)
    def payload_offset(address: int) -> int:
        return file_offset + address - vmaddr
    symbols = macho_symbols(path)
    actual, tail_jumps, address_takes = direct_calls(path)
    assert not tail_jumps, f"{path}: unexpected event-vector tail calls"
    assert not address_takes, f"{path}: event-vector function address became reachable"
    expected_flat = {
        target: tuple(call for _, _, calls in owners for call in calls)
        for target, owners in EXPECTED.items()
    }
    assert actual == expected_flat, f"{path}: changed complete grow direct-call inventory"
    initial = {
        target: tuple(call for call in calls
                      if b"\xbe\x04\x00\x00\x00" in
                      data[payload_offset(call) - 20:payload_offset(call)])
        for target, calls in actual.items()
    }
    assert initial == INITIAL_REQUESTS, \
        f"{path}: changed fixed-capacity event-vector initialization calls"
    for calls in initial.values():
        for call in calls:
            offset = payload_offset(call)
            window = data[offset - 64:offset]
            has_rbp_relative_lea = any(
                window[index] in (0x48, 0x4C) and
                window[index + 1] == 0x8D and
                (window[index + 2] & 0xC7) in (0x45, 0x85)
                for index in range(len(window) - 2)
            )
            assert has_rbp_relative_lea, \
                f"{path}: initial vector lost frame-local address at 0x{call:x}"
    checked_appends = 0
    for target, calls in actual.items():
        initial_calls = set(INITIAL_REQUESTS[target])
        for call in calls:
            if call in initial_calls:
                continue
            offset = payload_offset(call)
            assert b"\x84\xc0" in data[offset + 5:offset + 20], \
                f"{path}: grow result use changed at 0x{call:x}"
            checked_appends += 1
    assert checked_appends == 95
    ordered = sorted((address, name) for name, values in symbols.items()
                     for address in values if address)
    for owners in EXPECTED.values():
        for owner_start, owner_name, calls in owners:
            assert owner_start in symbols.get(owner_name, []), \
                f"{path}: changed event-vector caller owner {owner_name}"
            owner_end = min(address for address, _ in ordered if address > owner_start)
            assert all(owner_start <= call < owner_end for call in calls), \
                f"{path}: call escaped owner {owner_name}"
    return sum(len(calls) for calls in actual.values())


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: vf_event_vector_callgraph_contract_test.py payload payload"
        )
    counts = [verify(Path(argument)) for argument in sys.argv[1:]]
    assert counts[0] == counts[1] == 147
    print(
        "PASS: complete 147-call / 38-owner event-vector grow graph; "
        "52 initial requests, 95 checked append-growth calls and no direct "
        "tail/address-taken edges in both payloads"
    )


if __name__ == "__main__":
    main()
