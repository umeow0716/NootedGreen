#!/usr/bin/env python3
"""Verify every installed private route against both pinned Tahoe payloads."""

import re
import struct
import sys
from pathlib import Path


MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
NLIST_64_SIZE = 16


def checked_slice(data: bytes, start: int, size: int, label: str) -> bytes:
    if start < 0 or size < 0 or start > len(data) or size > len(data) - start:
        raise AssertionError(f"{label} escapes file bounds")
    return data[start : start + size]


def macho_symbols(path: Path) -> dict[str, list[int]]:
    data = path.read_bytes()
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != MH_MAGIC_64:
        raise AssertionError(f"{path}: not a little-endian Mach-O 64 payload")
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    checked_slice(data, 32, sizeofcmds, f"{path}: load commands")
    cursor = 32
    symtab = None
    for _ in range(ncmds):
        command, command_size = struct.unpack_from("<II", data, cursor)
        if command_size < 8 or command_size > 32 + sizeofcmds - cursor:
            raise AssertionError(f"{path}: malformed load command")
        if command == LC_SYMTAB:
            if command_size != 24 or symtab is not None:
                raise AssertionError(f"{path}: malformed or duplicate LC_SYMTAB")
            symtab = struct.unpack_from("<IIII", data, cursor + 8)
        cursor += command_size
    if cursor != 32 + sizeofcmds or symtab is None:
        raise AssertionError(f"{path}: missing load-command or symbol-table coverage")

    symbol_offset, symbol_count, string_offset, string_size = symtab
    symbols = checked_slice(
        data, symbol_offset, symbol_count * NLIST_64_SIZE, f"{path}: symbols"
    )
    strings = checked_slice(data, string_offset, string_size, f"{path}: strings")
    result: dict[str, list[int]] = {}
    for index in range(symbol_count):
        string_index = struct.unpack_from("<I", symbols, index * NLIST_64_SIZE)[0]
        value = struct.unpack_from("<Q", symbols, index * NLIST_64_SIZE + 8)[0]
        if string_index == 0:
            continue
        if string_index >= len(strings):
            raise AssertionError(f"{path}: symbol string index escapes table")
        end = strings.find(b"\0", string_index)
        if end < 0:
            raise AssertionError(f"{path}: unterminated symbol name")
        name = strings[string_index:end].decode("utf-8", errors="strict")
        result.setdefault(name, []).append(value)
    return result


def macho_vmaddr_for_file_offset(path: Path, file_offset: int) -> int:
    data = path.read_bytes()
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != MH_MAGIC_64:
        raise AssertionError(f"{path}: not a little-endian Mach-O 64 payload")
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    checked_slice(data, 32, sizeofcmds, f"{path}: load commands")
    cursor = 32
    for _ in range(ncmds):
        command, command_size = struct.unpack_from("<II", data, cursor)
        if command_size < 8 or command_size > 32 + sizeofcmds - cursor:
            raise AssertionError(f"{path}: malformed load command")
        if command == LC_SEGMENT_64:
            if command_size < 72:
                raise AssertionError(f"{path}: malformed LC_SEGMENT_64")
            vmaddr, vmsize, segment_file_offset, file_size = struct.unpack_from(
                "<QQQQ", data, cursor + 24
            )
            if (
                segment_file_offset <= file_offset
                and file_offset - segment_file_offset < file_size
            ):
                relative = file_offset - segment_file_offset
                if relative >= vmsize:
                    raise AssertionError(f"{path}: file offset escapes segment VM range")
                return vmaddr + relative
        cursor += command_size
    raise AssertionError(f"{path}: file offset 0x{file_offset:x} is not in a segment")


def cpp_byte_array(source: Path, name: str) -> bytes:
    text = source.read_text(encoding="utf-8")
    match = re.search(
        rf"constexpr\s+uint8_t\s+{re.escape(name)}\[\]\s*=\s*\{{(.*?)\}};",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise AssertionError(f"{source}: missing {name} byte array")
    values = re.findall(r"0x([0-9a-fA-F]{1,2})", match.group(1))
    if not values:
        raise AssertionError(f"{source}: empty {name} byte array")
    return bytes(int(value, 16) for value in values)


def unique_pattern_vmaddr(path: Path, pattern: bytes, label: str) -> int:
    data = path.read_bytes()
    first = data.find(pattern)
    if first < 0 or data.find(pattern, first + 1) >= 0:
        raise AssertionError(f"{path}: {label} is absent or non-unique")
    return macho_vmaddr_for_file_offset(path, first)


def single_symbol(symbols: dict[str, list[int]], name: str, path: Path) -> int:
    values = symbols.get(name, [])
    if len(values) != 1:
        raise AssertionError(f"{path}: {name} is absent or non-unique")
    return values[0]


def verify_runtime_patch_owners(source: Path, payload: Path, symbols: dict[str, list[int]]) -> None:
    header = source.parent / "kern_vf_runtime_patch.hpp"
    patcher_header = source.parent / "kern_patcherplus.hpp"
    probe_start = single_symbol(
        symbols, "__ZN16IntelAccelerator5probeEP9IOServicePi", payload
    )
    probe_end = single_symbol(
        symbols,
        "__ZN16IntelAccelerator18encodeFailureStackE15IGFailureReason",
        payload,
    )
    gpu_info_start = single_symbol(
        symbols, "__ZN16IntelAccelerator10getGPUInfoEv", payload
    )
    gpu_info_end = single_symbol(
        symbols, "__ZN16IntelAccelerator14teardownDeviceEP11IOPCIDevice", payload
    )

    sku = unique_pattern_vmaddr(
        payload, cpp_byte_array(header, "spoofedSkuFind"), "spoofed SKU anchor"
    )
    if not probe_start <= sku < probe_end:
        raise AssertionError(
            f"{payload}: spoofed SKU anchor 0x{sku:x} is outside probe "
            f"[0x{probe_start:x}, 0x{probe_end:x})"
        )

    for name in (
        "sliceFuseFind",
        "dssFuseFind",
        "euFuseFind",
        "mediaFuseFind",
        "rpmConfigFind",
        "l3BranchFind",
    ):
        address = unique_pattern_vmaddr(payload, cpp_byte_array(header, name), name)
        if not gpu_info_start <= address < gpu_info_end:
            raise AssertionError(
                f"{payload}: {name} 0x{address:x} is outside getGPUInfo "
                f"[0x{gpu_info_start:x}, 0x{gpu_info_end:x})"
            )

    normalized_source = " ".join(source.read_text(encoding="utf-8").split())
    required_source_fragments = (
        '"__ZN16IntelAccelerator5probeEP9IOServicePi", probeStart',
        '"__ZN16IntelAccelerator18encodeFailureStackE15IGFailureReason", probeEnd',
        "patcher, patchesAlways, probeStart, probeEnd - probeStart",
        '"__ZN16IntelAccelerator10getGPUInfoEv", gpuInfoStart',
        '"__ZN16IntelAccelerator14teardownDeviceEP11IOPCIDevice", gpuInfoEnd',
        "patcher, vfRuntimePatches, gpuInfoStart, gpuInfoEnd - gpuInfoStart",
        "NGVfRuntimePatch::sliceFuseFind, vfSliceFuseReplace, 1",
        "NGVfRuntimePatch::dssFuseFind, vfDssFuseReplace, 1",
        "NGVfRuntimePatch::euFuseFind, vfEuFuseReplace, 1",
        "NGVfRuntimePatch::mediaFuseFind, vfMediaFuseReplace, 1",
        "NGVfRuntimePatch::rpmConfigFind, vfRpmConfigReplace, 1",
        "NGVfRuntimePatch::l3BranchFind, r3b, 1",
    )
    for fragment in required_source_fragments:
        if fragment not in normalized_source:
            raise AssertionError(f"{source}: missing bounded patch contract: {fragment}")

    source_text = source.read_text(encoding="utf-8")
    for name in (
        "sliceFuseFind",
        "dssFuseFind",
        "euFuseFind",
        "mediaFuseFind",
        "rpmConfigFind",
        "l3BranchFind",
    ):
        if re.search(
            rf"NGVfRuntimePatch::{name}\s*,\s*\w+\s*,\s*"
            rf"arrsize\s*\(\s*NGVfRuntimePatch::{name}\s*\)",
            source_text,
        ):
            raise AssertionError(
                f"{source}: {name} uses the ambiguous mutable-array size/count form"
            )

    patcher_header_text = patcher_header.read_text(encoding="utf-8")
    normalized_patcher_header = " ".join(patcher_header_text.split())
    if "size as count plus count as skip" not in normalized_patcher_header:
        raise AssertionError(f"{patcher_header}: missing mutable-array overload guard")
    array_constructors = re.findall(
        r"template<size_t N>\s+LookupPatchPlus\((.*?)\)\s*:\s*LookupPatchPlus",
        patcher_header_text,
        flags=re.DOTALL,
    )
    if (
        len(array_constructors) != 3
        or any("size_t skip" in constructor for constructor in array_constructors)
        or any("size_t count" not in constructor for constructor in array_constructors)
    ):
        raise AssertionError(
            f"{patcher_header}: array constructors must not expose an ambiguous skip argument"
        )


def routed_symbols(source: Path) -> set[str]:
    text = source.read_text(encoding="utf-8")
    blocks = re.findall(
        r"KernelPatcher::RouteRequest\s+\w+\[\]\s*=\s*\{(.*?)\n\s*\};",
        text,
        flags=re.DOTALL,
    )
    blocks.extend(re.findall(
        r"ExactRouteRequest\s+\w+\[\]\s*=\s*\{(.*?)\n\s*\};",
        text,
        flags=re.DOTALL,
    ))
    if not blocks:
        raise AssertionError(f"{source}: no RouteRequest arrays found")
    names = []
    for block in blocks:
        names.extend(re.findall(r'\{\s*"([^"]+)"\s*,', block))
    allowed_duplicate_counts = {
        "__ZN31AppleIntelFramebufferController5startEP9IOService": 2,
        "__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm": 8,
    }
    duplicates = sorted(
        {
            name
            for name in names
            if names.count(name) != allowed_duplicate_counts.get(name, 1)
        }
    )
    if duplicates:
        raise AssertionError(f"duplicate routed symbols: {duplicates}")
    return set(names)


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: route_symbol_contract_test.py kern_gen11.cpp "
            "accelerator-a accelerator-b framebuffer-a framebuffer-b"
        )
    source = Path(sys.argv[1])
    payload_paths = [Path(value) for value in sys.argv[2:]]
    payloads = [macho_symbols(path) for path in payload_paths]
    routes = routed_symbols(source)
    system_routes = {
        "__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj",
        "__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv",
        "__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj",
        "__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs",
        "__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy",
        "__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits",
        "__ZN17IOAccel2DContext26finishEj",
        "__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy",
        "__ZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN17IOAccelGLContext214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN27IOAccelGLDrawableUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN29IOAccelDisplayPipeUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_",
        "__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei",
        "__ZN22IOGraphicsAccelerator218finalize_interruptEP22IOInterruptEventSourcei",
        "__ZN22IOGraphicsAccelerator218deviceCacheControlEP20IOSurfaceDeviceCachejyy",
        "__ZN22IOGraphicsAccelerator220emitFirstFlushEventsEv",
        "__ZN22IOGraphicsAccelerator24stopEP9IOService",
    }
    if not system_routes <= routes:
        raise AssertionError("missing explicitly admitted System-KC route")

    # Keep route inventory changes explicit. This count includes admission,
    # lifecycle, GGTT, GuC/CTB, IRQ, native producer and System-KC routes.
    expected_route_count = 165
    if len(routes) != expected_route_count:
        raise AssertionError(
            f"route inventory changed: expected {expected_route_count}, got {len(routes)}"
        )

    accelerator = payloads[:2]
    framebuffer = payloads[2:]
    accelerator_count = 0
    framebuffer_count = 0
    framebuffer_variant_specific = {
        "__ZN24AppleIntelBaseController5probeEP9IOServicePi",
        "__ZN31AppleIntelFramebufferController5probeEP9IOServicePi",
    }
    accelerator_duplicate_specific = {
        "__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm":
            [0x7396, 0x2EA12, 0x55B84, 0x5B018,
             0x6B8B8, 0x757B2, 0x79A2E, 0x844D4],
    }
    for path, symbols in zip(payload_paths[:2], accelerator):
        verify_runtime_patch_owners(source, path, symbols)
    for route in sorted(routes - system_routes):
        accel_values = [table.get(route, []) for table in accelerator]
        fb_values = [table.get(route, []) for table in framebuffer]
        in_accel = any(accel_values)
        in_framebuffer = any(fb_values)
        if in_accel == in_framebuffer:
            raise AssertionError(
                f"{route}: route must belong to exactly one payload family"
            )
        if in_accel:
            if route in accelerator_duplicate_specific:
                expected = accelerator_duplicate_specific[route]
                if any(sorted(values) != expected for values in accel_values):
                    raise AssertionError(
                        f"{route}: changed duplicate accelerator route addresses"
                    )
                accelerator_count += 1
                continue
            if any(len(values) != 1 for values in accel_values):
                raise AssertionError(f"{route}: missing or non-unique in accelerator variants")
            if accel_values[0][0] != accel_values[1][0]:
                raise AssertionError(f"{route}: accelerator variants disagree on symbol address")
            accelerator_count += 1
        else:
            counts = [len(values) for values in fb_values]
            if route in framebuffer_variant_specific:
                if sorted(counts) != [0, 1]:
                    raise AssertionError(f"{route}: expected in exactly one framebuffer UUID")
            else:
                if counts != [1, 1]:
                    raise AssertionError(f"{route}: missing or non-unique in framebuffer variants")
            framebuffer_count += 1

    print(
        "PASS: "
        f"{len(routes)} unique routed Tahoe symbols "
        f"({accelerator_count} accelerator, {framebuffer_count} framebuffer, "
        f"{len(system_routes)} System KC)"
    )


if __name__ == "__main__":
    main()
