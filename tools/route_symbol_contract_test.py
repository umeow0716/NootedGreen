#!/usr/bin/env python3
"""Verify every installed private route against both pinned Tahoe payloads."""

import re
import struct
import sys
from pathlib import Path


MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
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


def routed_symbols(source: Path) -> set[str]:
    text = source.read_text(encoding="utf-8")
    blocks = re.findall(
        r"KernelPatcher::RouteRequest\s+\w+\[\]\s*=\s*\{(.*?)\n\s*\};",
        text,
        flags=re.DOTALL,
    )
    if not blocks:
        raise AssertionError(f"{source}: no RouteRequest arrays found")
    names = []
    for block in blocks:
        names.extend(re.findall(r'\{\s*"([^"]+)"\s*,', block))
    uuid_specific_duplicate = (
        "__ZN31AppleIntelFramebufferController5startEP9IOService"
    )
    duplicates = sorted(
        {
            name
            for name in names
            if names.count(name) != 1
            and not (name == uuid_specific_duplicate and names.count(name) == 2)
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

    # Keep route inventory changes explicit. This count includes admission,
    # lifecycle, GGTT, GuC/CTB, IRQ and native producer routes.
    expected_route_count = 60
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
    for route in sorted(routes):
        accel_values = [table.get(route, []) for table in accelerator]
        fb_values = [table.get(route, []) for table in framebuffer]
        in_accel = any(accel_values)
        in_framebuffer = any(fb_values)
        if in_accel == in_framebuffer:
            raise AssertionError(
                f"{route}: route must belong to exactly one payload family"
            )
        if in_accel:
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
        f"({accelerator_count} accelerator, {framebuffer_count} framebuffer)"
    )


if __name__ == "__main__":
    main()
