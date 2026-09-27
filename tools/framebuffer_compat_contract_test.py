#!/usr/bin/env python3
"""Validate the pinned TGL framebuffer compatibility-provider ABI."""

import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path


BASE_METHODS = {
    "__ZN24AppleIntelBaseController14ReadRegister32Em",
    "__ZN24AppleIntelBaseController14ReadRegister64EPVvm",
    "__ZN24AppleIntelBaseController15WriteRegister32Emj",
    "__ZN24AppleIntelBaseController15WriteRegister64EPVvmy",
    "__ZN24AppleIntelBaseController16hwSetupDSBMemoryEv",
    "__ZN24AppleIntelBaseController9getPMTNowEv",
}
PRODUCTION_METHODS = {
    symbol.replace("24AppleIntelBaseController", "31AppleIntelFramebufferController")
    for symbol in BASE_METHODS
}
COMMON_METHODS = {
    "__ZN17AppleIntelPortHAL13probePortModeEv",
    "__ZN21AppleIntelDisplayPath19DSBEngineBusyStatusE",
}


def nm_symbols(path: Path, undefined: bool) -> set[str]:
    flags = ["-u"] if undefined else ["-g"]
    output = subprocess.run(
        ["nm", *flags, str(path)], check=True, capture_output=True, text=True
    ).stdout
    symbols = set()
    for line in output.splitlines():
        match = re.search(r"(?:^|\s)(__?[A-Za-z0-9_]+)$", line)
        if match:
            symbols.add(match.group(1))
    return symbols


def macho_external_relocations(path: Path):
    data = path.read_bytes()
    header = struct.unpack_from("<IiiIIIII", data, 0)
    assert header[0] == 0xFEEDFACF
    command_count = header[4]
    offset = 32
    symtab = dysymtab = None
    sections = []
    for _ in range(command_count):
        command, size = struct.unpack_from("<II", data, offset)
        assert size >= 8 and offset + size <= len(data)
        if command == 0x2:
            symtab = struct.unpack_from("<IIII", data, offset + 8)
        elif command == 0xB:
            dysymtab = struct.unpack_from("<18I", data, offset + 8)
        elif command == 0x19:
            section_count = struct.unpack_from("<I", data, offset + 64)[0]
            section_offset = offset + 72
            for _ in range(section_count):
                address, section_size, file_offset = struct.unpack_from(
                    "<QQI", data, section_offset + 32
                )
                sections.append((address, section_size, file_offset))
                section_offset += 80
        offset += size
    assert symtab and dysymtab and sections

    symbol_offset, symbol_count, string_offset, string_size = symtab
    assert string_offset + string_size <= len(data)
    names = []
    for index in range(symbol_count):
        string_index = struct.unpack_from("<I", data, symbol_offset + index * 16)[0]
        assert string_index < string_size
        end = data.find(b"\0", string_offset + string_index, string_offset + string_size)
        assert end >= 0
        names.append(data[string_offset + string_index : end].decode())

    relocation_offset, relocation_count = dysymtab[14], dysymtab[15]
    assert relocation_offset + relocation_count * 8 <= len(data)
    result = []
    for index in range(relocation_count):
        address, word = struct.unpack_from("<iI", data, relocation_offset + index * 8)
        symbol_index = word & 0xFFFFFF
        external = (word >> 27) & 1
        if not external or symbol_index >= len(names):
            continue
        section = next(
            (item for item in sections if item[0] <= address < item[0] + item[1]),
            None,
        )
        assert section is not None
        file_offset = section[2] + address - section[0]
        result.append((address, file_offset, names[symbol_index], data))
    return result


def validate_payload(plist_path: Path, binary_path: Path, expected: set[str]) -> None:
    with plist_path.open("rb") as stream:
        info = plistlib.load(stream)
    libraries = info["OSBundleLibraries"]
    assert info["OSBundleRequired"] == "Root"
    assert libraries["com.StezzaPilot.NootedGreen"] == "1.0.0"
    assert "org.smichaud.HookCase" not in libraries

    undefined = nm_symbols(binary_path, True)
    compatibility_imports = {
        symbol
        for symbol in undefined
        if symbol in BASE_METHODS
        or symbol in PRODUCTION_METHODS
        or symbol in COMMON_METHODS
        or symbol == "_strnstr"
    }
    assert compatibility_imports == expected

    gtt_calls = []
    setup_calls = []
    for address, file_offset, symbol, data in macho_external_relocations(binary_path):
        if symbol.endswith("ReadRegister64EPVvm") or symbol.endswith(
            "WriteRegister64EPVvmy"
        ):
            assert data[file_offset - 1] in (0xE8, 0xE9)
            # Every admitted caller loads its base from controller +0xCA0.
            assert b"\xA0\x0C\x00\x00" in data[max(0, file_offset - 40) : file_offset]
            gtt_calls.append(address)
        elif symbol.endswith("hwSetupDSBMemoryEv"):
            assert data[file_offset - 1] == 0xE8
            setup_calls.append(address)
    assert len(gtt_calls) == 8
    assert len(setup_calls) == 1


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: framebuffer_compat_contract_test.py OBJECT SOURCE "
            "DEBUG_PLIST DEBUG_MACHO PRODUCTION_PLIST PRODUCTION_MACHO"
        )
    object_path, source_path, debug_plist, debug_binary, production_plist, production_binary = (
        map(Path, sys.argv[1:])
    )
    source = source_path.read_text(encoding="utf-8")
    defined = nm_symbols(object_path, False)
    debug_expected = BASE_METHODS | COMMON_METHODS | {"_strnstr"}
    production_expected = PRODUCTION_METHODS | COMMON_METHODS
    assert debug_expected | production_expected <= defined
    for literal in ("0xC50", "0xCA0", "0xCEC", "0xDCC", "0x548"):
        assert literal in source
    assert "DSBEngineBusyStatus[3]" in source
    assert "DSB_CHICKEN" not in source

    validate_payload(debug_plist, debug_binary, debug_expected)
    validate_payload(production_plist, production_binary, production_expected)
    print("PASS: exact pinned TGL framebuffer compatibility ABI and GGTT callers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
