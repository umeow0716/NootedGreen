#!/usr/bin/env python3
"""Verify the compiled x86_64 bridge's nonstandard return ABI exactly."""
import struct
import sys
from pathlib import Path


MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
BRIDGE = b"_ngVfCommandPoolInitBridge"
HELPER = b"_ngVfCommandPoolInitHelper"


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: vf_command_pool_bridge_object_test.py kern_gen11.o")
    image = Path(sys.argv[1]).read_bytes()
    assert len(image) >= 32 and struct.unpack_from("<I", image)[0] == MH_MAGIC_64
    commands, command_bytes = struct.unpack_from("<II", image, 16)
    assert 32 + command_bytes <= len(image)
    cursor = 32
    sections = []
    symtab = None
    for _ in range(commands):
        command, size = struct.unpack_from("<II", image, cursor)
        assert size >= 8 and cursor + size <= 32 + command_bytes
        if command == LC_SEGMENT_64:
            section_count = struct.unpack_from("<I", image, cursor + 64)[0]
            assert size == 72 + section_count * 80
            section_cursor = cursor + 72
            for _ in range(section_count):
                fields = struct.unpack_from("<16s16sQQIIIIIIII", image, section_cursor)
                sections.append({
                    "address": fields[2], "size": fields[3], "offset": fields[4],
                    "reloc": fields[6], "reloc_count": fields[7],
                })
                section_cursor += 80
        elif command == LC_SYMTAB:
            assert symtab is None and size == 24
            symtab = struct.unpack_from("<IIII", image, cursor + 8)
        cursor += size
    assert cursor == 32 + command_bytes and symtab is not None
    symbol_offset, symbol_count, string_offset, string_size = symtab
    assert symbol_offset + symbol_count * 16 <= len(image)
    assert string_offset + string_size <= len(image)
    strings = image[string_offset:string_offset + string_size]
    symbols = []
    for index in range(symbol_count):
        string_index, kind, section, _, value = struct.unpack_from(
            "<IBBHQ", image, symbol_offset + index * 16)
        assert string_index < len(strings)
        end = strings.find(b"\0", string_index)
        assert end >= 0
        symbols.append((strings[string_index:end], kind, section, value))
    by_name = {}
    for index, symbol in enumerate(symbols):
        by_name.setdefault(symbol[0], []).append((index, *symbol[1:]))
    assert len(by_name.get(BRIDGE, [])) == len(by_name.get(HELPER, [])) == 1
    bridge_index, _, bridge_section, bridge_value = by_name[BRIDGE][0]
    helper_index = by_name[HELPER][0][0]
    assert bridge_index != helper_index and 1 <= bridge_section <= len(sections)
    section = sections[bridge_section - 1]
    relative = bridge_value - section["address"]
    assert relative >= 0 and relative + 32 <= section["size"]
    body_offset = section["offset"] + relative
    body = image[body_offset:body_offset + 32]
    expected_body = bytes.fromhex(
        "53 48 83 ec 08 48 89 cb 48 8d 44 24 18 50 "
        "e8 00 00 00 00 48 83 c4 10 48 89 df 5b 84 c0 c3 0f 0b"
    )
    assert body == expected_body, (
        "changed command-pool bridge machine ABI: " + body.hex(" ")
    )

    relocations = []
    for index in range(section["reloc_count"]):
        address, bits = struct.unpack_from(
            "<iI", image, section["reloc"] + index * 8)
        symbol = bits & 0xFFFFFF
        pcrel = (bits >> 24) & 1
        length = (bits >> 25) & 3
        external = (bits >> 27) & 1
        kind = bits >> 28
        if address == bridge_value + 15:
            relocations.append((symbol, pcrel, length, external, kind))
    assert relocations == [(helper_index, 1, 2, 1, 2)], \
        "bridge call is not one external PC-relative 32-bit branch to helper"
    print("PASS: exact compiled command-pool bridge stack/RDI/ZF ABI and helper relocation")


if __name__ == "__main__":
    main()
