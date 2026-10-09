#!/usr/bin/env python3
"""Pin the Tahoe VF media IOSurface divisor guard and its Apple references."""

import hashlib
import re
import struct
import sys
from pathlib import Path


MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
LC_FILESET_ENTRY = 0x80000035
SYSTEM_KC_SHA256 = "5cb1be1dc530b4b953a33943567589101d3ac46bb8cf90728566ee7e5b1fa214"
CALCULATE = "__ZN15IGAccelResource38calculateIOSurfaceDeviceCacheVRAMBytesEPyS0_"
CALCULATE_END = "__ZN15IGAccelResource14getLevelOffsetEhhPi"
TGL_START = 0x72834
TGL_END = 0x7339C
TGL_BODY_SHA256 = "ae0544ef25e2a5c2fb3c66ff79cb60d5b7700073f7c91f4bac5517ff99b71cd0"
ORIGINAL = bytes.fromhex(
    "83bd24ffffff00899d20ffffff741b488b4d8089c831d241f7f74189c4"
    "89d831d2f7f189c3e9ca000000"
)
REPLACEMENT = bytes.fromhex(
    "899d20ffffff4585ff741f488b4d8085c9741789c831d241f7f74189c4"
    "89d831d2f7f189c3e9ca000000"
)
APPLE_REFERENCES = {
    "com.apple.driver.AppleIntelICLGraphics": {
        "uuid": "30c5ada1ff2b3bc6ba2e12df2dd390e8",
        "start": 0x140E68CA,
        "end": 0x140E7614,
        "sha256": "6d5692982d1852dddcc22e098878006c93f73e84d90a07f292bd1f0c086d6655",
        "unguarded": bytes.fromhex(
            "83bd3cffffff00898550ffffff744289c14489f831d241f7f44189c6"
            "89c831d241f7f789c6"
        ),
    },
    "com.apple.driver.AppleIntelKBLGraphics": {
        "uuid": "e663e9d6ec863d959d04d6dfdad057b5",
        "start": 0x1450E68C,
        "end": 0x1450F4D0,
        "sha256": "a7d5f8bb9f3d34a8225e9e0519e3d6dd29b69104ea565350b72dbc32ffd9f269",
        "unguarded": bytes.fromhex(
            "83bd38ffffff00742b4489f831d241f7f44189c38b8570ffffff"
            "31d241f7f789c7"
        ),
    },
}


def commands(data: bytes, base: int):
    if base + 32 > len(data) or struct.unpack_from("<I", data, base)[0] != MH_MAGIC_64:
        raise AssertionError("not a little-endian 64-bit Mach-O")
    command_count, command_bytes = struct.unpack_from("<II", data, base + 16)
    cursor = base + 32
    end = cursor + command_bytes
    if end > len(data):
        raise AssertionError("load commands escape image")
    for _ in range(command_count):
        command, size = struct.unpack_from("<II", data, cursor)
        if size < 8 or cursor + size > end:
            raise AssertionError("malformed load command")
        yield command, cursor
        cursor += size
    if cursor != end:
        raise AssertionError("load-command size mismatch")


def parse_macho(data: bytes, base: int, label: str):
    segments = []
    symtab = None
    uuids = []
    for command, offset in commands(data, base):
        if command == LC_SEGMENT_64:
            fields = struct.unpack_from("<II16sQQQQIIII", data, offset)
            segments.append((fields[3], fields[5], fields[6]))
        elif command == LC_SYMTAB:
            if symtab is not None:
                raise AssertionError(f"{label}: duplicate symbol table")
            symtab = struct.unpack_from("<6I", data, offset)[2:]
        elif command == 0x1B:
            uuids.append(data[offset + 8:offset + 24].hex())
    if symtab is None or len(uuids) != 1:
        raise AssertionError(f"{label}: incomplete Mach-O metadata")
    symbol_offset, count, string_offset, string_size = symtab
    if symbol_offset + count * 16 > len(data) or \
            string_offset + string_size > len(data):
        raise AssertionError(f"{label}: symbol table escapes image")
    symbols = {}
    for index in range(count):
        entry = symbol_offset + index * 16
        name_offset, _, _, _, address = struct.unpack_from("<IBBHQ", data, entry)
        if name_offset >= string_size:
            raise AssertionError(f"{label}: invalid symbol string offset")
        start = string_offset + name_offset
        end = data.find(b"\0", start, string_offset + string_size)
        if end < 0:
            raise AssertionError(f"{label}: unterminated symbol")
        name = data[start:end].decode("utf-8")
        symbols.setdefault(name, []).append(address)
    return segments, symbols, uuids[0]


def single(symbols, name: str, label: str) -> int:
    values = symbols.get(name, [])
    if len(values) != 1:
        raise AssertionError(f"{label}: missing or ambiguous {name}")
    return values[0]


def vm_slice(data: bytes, segments, start: int, end: int, label: str) -> bytes:
    locations = [
        file_offset + start - virtual
        for virtual, file_offset, size in segments
        if virtual <= start and end <= virtual + size
    ]
    if len(locations) != 1:
        raise AssertionError(f"{label}: unmapped or ambiguous VM range")
    offset = locations[0]
    if offset + end - start > len(data):
        raise AssertionError(f"{label}: mapped range escapes image")
    return data[offset:offset + end - start]


def verify_tgl_payload(data: bytes, label: str) -> None:
    segments, symbols, _ = parse_macho(data, 0, label)
    start = single(symbols, CALCULATE, label)
    end = single(symbols, CALCULATE_END, label)
    if (start, end) != (TGL_START, TGL_END):
        raise AssertionError(f"{label}: changed cache-size function bounds")
    body = vm_slice(data, segments, start, end, label)
    if hashlib.sha256(body).hexdigest() != TGL_BODY_SHA256:
        raise AssertionError(f"{label}: changed cache-size function body")
    if body.count(ORIGINAL) != 1 or body.index(ORIGINAL) != 0x382:
        raise AssertionError(f"{label}: changed sparse-plane divide boundary")
    if REPLACEMENT in body:
        raise AssertionError(f"{label}: payload was modified instead of VF runtime patching")


def c_array(source: str, name: str) -> bytes:
    match = re.search(
        rf"static const uint8_t {re.escape(name)}\[\] = \{{(.*?)\n\s*\}};",
        source,
        re.S,
    )
    if not match:
        raise AssertionError(f"missing byte array {name}")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
    raise AssertionError(f"unterminated function {signature}")


def containing_vf_block(body: str, marker: int) -> str:
    start = body.rfind("if (vfActive) {", 0, marker)
    if start < 0:
        raise AssertionError("surface guard is not inside a VF gate")
    opening = body.index("{", start)
    depth = 0
    for cursor in range(opening, len(body)):
        if body[cursor] == "{":
            depth += 1
        elif body[cursor] == "}":
            depth -= 1
            if depth == 0:
                if marker >= cursor:
                    raise AssertionError("surface guard escaped the VF gate")
                return body[start:cursor + 1]
    raise AssertionError("unterminated VF gate")


def verify_source(source: str) -> None:
    body = function_body(
        source,
        "bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)",
    )
    marker = body.index("surfacePlaneDivisionPatch")
    vf_block = containing_vf_block(body, marker)
    required = {
        CALCULATE: 1,
        CALCULATE_END: 1,
        "surfaceBytesEnd - surfaceBytesStart != 0xb68": 1,
        "surfacePlaneDivisionFind": 3,
        "surfacePlaneDivisionReplace": 3,
        "surfacePlaneDivisionPatch.apply(": 1,
        "patcher, surfaceBytesStart,": 1,
        "surfaceBytesEnd - surfaceBytesStart": 2,
        "V338: admitted sparse VF IOSurface plane through native surface-level fallback": 1,
    }
    for token, count in required.items():
        if vf_block.count(token) != count:
            raise AssertionError(f"changed/missing VF-only surface guard token: {token}")
    if c_array(source, "surfacePlaneDivisionFind") != ORIGINAL:
        raise AssertionError("changed exact native sparse-plane divide anchor")
    if c_array(source, "surfacePlaneDivisionReplace") != REPLACEMENT:
        raise AssertionError("changed exact sparse-plane fallback replacement")
    if len(ORIGINAL) != len(REPLACEMENT) or len(ORIGINAL) != 42:
        raise AssertionError("surface guard no longer preserves the exact code span")
    # The replacement first stores the native plane-size result, then rejects
    # either zero divisor.  Its remaining two divisions and result moves are
    # byte-identical to the native path.
    if REPLACEMENT[:6] != ORIGINAL[7:13] or \
            REPLACEMENT[11:15] != ORIGINAL[15:19] or \
            REPLACEMENT[19:] != ORIGINAL[19:]:
        raise AssertionError("valid-plane arithmetic is no longer native-equivalent")
    if REPLACEMENT[6:19] != bytes.fromhex(
            "4585ff741f488b4d8085c97417"):
        raise AssertionError("changed dual-divisor fallback guards")


def verify_guard_model() -> None:
    def native(plane_count, row_bytes, bytes_per_element, plane_size):
        if plane_count == 0:
            return "fallback"
        if row_bytes == 0 or bytes_per_element == 0:
            return "divide-error"
        return row_bytes // bytes_per_element, plane_size // row_bytes

    def patched(plane_count, row_bytes, bytes_per_element, plane_size):
        # The native zero-plane preparation clears bytes_per_element, so the
        # first test also preserves the original zero-plane fallback.
        if plane_count == 0:
            bytes_per_element = 0
        if bytes_per_element == 0 or row_bytes == 0:
            return "fallback"
        return row_bytes // bytes_per_element, plane_size // row_bytes

    for plane_count in (0, 1, 2):
        for row_bytes in (0, 64, 3840):
            for bytes_per_element in (0, 1, 2, 4):
                for plane_size in (0, 4096, 8294400):
                    before = native(
                        plane_count, row_bytes, bytes_per_element, plane_size
                    )
                    after = patched(
                        plane_count, row_bytes, bytes_per_element, plane_size
                    )
                    if before == "divide-error":
                        if after != "fallback":
                            raise AssertionError("invalid divisor did not use fallback")
                    elif before != after:
                        raise AssertionError("valid native plane calculation changed")


def fileset_entries(data: bytes):
    result = {}
    for command, offset in commands(data, 0):
        if command != LC_FILESET_ENTRY:
            continue
        _, _, _, file_offset, name_offset, _ = struct.unpack_from(
            "<IIQQII", data, offset
        )
        start = offset + name_offset
        end = data.find(b"\0", start)
        if end < 0:
            raise AssertionError("unterminated fileset identifier")
        result[data[start:end].decode("utf-8")] = file_offset
    return result


def verify_apple_references(data: bytes) -> None:
    if hashlib.sha256(data).hexdigest() != SYSTEM_KC_SHA256:
        raise AssertionError("unreviewed Tahoe SystemKC identity")
    entries = fileset_entries(data)
    for identifier, expected in APPLE_REFERENCES.items():
        if identifier not in entries:
            raise AssertionError(f"missing Apple reference {identifier}")
        segments, symbols, uuid = parse_macho(
            data, entries[identifier], identifier
        )
        if uuid != expected["uuid"]:
            raise AssertionError(f"{identifier}: changed UUID")
        start = single(symbols, CALCULATE, identifier)
        end = single(symbols, CALCULATE_END, identifier)
        if (start, end) != (expected["start"], expected["end"]):
            raise AssertionError(f"{identifier}: changed function bounds")
        body = vm_slice(data, segments, start, end, identifier)
        if hashlib.sha256(body).hexdigest() != expected["sha256"]:
            raise AssertionError(f"{identifier}: changed function body")
        if body.count(expected["unguarded"]) != 1:
            raise AssertionError(
                f"{identifier}: native plane-divisor assumption changed"
            )
    print("PASS exact Tahoe ICL/KBL native IOSurface divisor references")


def expect_source_failure(source: str, old: str, new: str) -> None:
    if source.count(old) != 1:
        raise AssertionError(f"mutation anchor is not unique: {old}")
    mutated = source.replace(old, new, 1)
    try:
        verify_source(mutated)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"source mutation was accepted: {old}")


def main() -> None:
    if len(sys.argv) not in (5, 6):
        raise SystemExit(
            "usage: vf_media_surface_size_contract_test.py "
            "kern_gen11.cpp kern_gen11.hpp production debug [SystemKC]"
        )
    source = Path(sys.argv[1]).read_text()
    header = Path(sys.argv[2]).read_text()
    production = Path(sys.argv[3]).read_bytes()
    debug = Path(sys.argv[4]).read_bytes()
    if "class Gen11" not in header:
        raise AssertionError("unexpected Gen11 header")
    verify_tgl_payload(production, "production")
    verify_tgl_payload(debug, "debug")
    if production[TGL_START:TGL_END] != debug[TGL_START:TGL_END]:
        raise AssertionError("production/debug cache-size functions differ")
    verify_source(source)
    verify_guard_model()
    mutations = (
        ("surfaceBytesEnd - surfaceBytesStart != 0xb68",
         "surfaceBytesEnd - surfaceBytesStart != 0xb69"),
        ("0x45, 0x85, 0xff,", "0x45, 0x85, 0xf6,"),
        ("0x85, 0xc9,", "0x85, 0xd2,"),
        ("0x74, 0x1f,", "0x75, 0x1f,"),
        ("0x74, 0x17,", "0x75, 0x17,"),
        ("surfacePlaneDivisionPatch.apply(",
         "surfacePlaneDivisionPatch.skip("),
        ("V338: admitted sparse VF IOSurface plane through native surface-level fallback",
         "V338: skipped sparse plane"),
    )
    for old, new in mutations:
        expect_source_failure(source, old, new)
    print("PASS exact TGL sparse-plane fallback and seven source mutations")
    if len(sys.argv) == 6:
        verify_apple_references(Path(sys.argv[5]).read_bytes())


if __name__ == "__main__":
    main()
