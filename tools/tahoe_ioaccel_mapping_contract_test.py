#!/usr/bin/env python3
"""Check a locally archived Tahoe KC; not a GPU completion/safety test."""

import hashlib
import pathlib
import struct
import sys


KC_SHA256 = "5cb1be1dc530b4b953a33943567589101d3ac46bb8cf90728566ee7e5b1fa214"
IDENTIFIER = b"com.apple.iokit.IOAcceleratorFamily2"
EVENT_VTABLE = "__ZTV24IOAccelEventMachineFast2"
EVENT_FINISH = "__ZN24IOAccelEventMachineFast211finishEventEP12IOAccelEvent"
EVENT_WAIT = "__ZN20IOAccelEventMachine212waitForStampEijPj"
EVENT_CLEAN = "__ZN24IOAccelEventMachineFast210cleanEventEP12IOAccelEvent"
CONTRACTS = {
    "__ZN16IOAccelMemoryMap20getGPUVirtualAddressEv":
        bytes.fromhex("f6 47 10 40 75 08 48 8b 87 98 00 00 00 c3"),
    "__ZN16IOAccelMemoryMap8completeEv":
        bytes.fromhex("55 48 89 e5 ff 4f 0c 5d c3"),
    "__ZN16IOAccelMemoryMap11finishEventEv":
        bytes.fromhex("55 48 89 e5 48 8b 87 88 00 00 00 48 8b 80 80 03 00 00 "
                      "48 8d 77 38 48 8b 08 48 8b 89 88 01 00 00 48 89 c7 5d ff e1"),
}


def commands(image, base):
    header = struct.unpack_from("<8I", image, base)
    assert header[0] == 0xFEEDFACF, "not a little-endian 64-bit Mach-O"
    offset = base + 32
    limit = offset + header[5]
    for _ in range(header[4]):
        command, size = struct.unpack_from("<II", image, offset)
        assert size >= 8 and offset + size <= limit <= len(image), "bad load command"
        yield command, offset
        offset += size
    assert offset == limit, "load-command size mismatch"


def check(path):
    image = pathlib.Path(path).read_bytes()
    # Identity is checked before parsing this deliberately version-specific
    # fixture. An unknown KC must be reviewed, never silently accepted.
    assert hashlib.sha256(image).hexdigest() == KC_SHA256, "unreviewed KC identity"
    entries = []
    for command, offset in commands(image, 0):
        if command == 0x80000035:
            _, _, _, file_offset, name_offset, _ = struct.unpack_from("<IIQQII", image, offset)
            name_start = offset + name_offset
            if image[name_start:image.index(0, name_start)] == IDENTIFIER:
                entries.append(file_offset)
    assert len(entries) == 1, "missing/ambiguous IOAccel fileset"
    segments = []
    symtab = None
    for command, offset in commands(image, entries[0]):
        if command == 0x19:
            fields = struct.unpack_from("<II16sQQQQIIII", image, offset)
            segments.append((fields[3], fields[5], fields[6]))
        elif command == 2:
            assert symtab is None, "duplicate symbol table"
            symtab = struct.unpack_from("<6I", image, offset)[2:]
    assert symtab is not None, "missing embedded symbol table"
    symbol_offset, count, string_offset, string_size = symtab
    matches = {name: [] for name in {*CONTRACTS, EVENT_VTABLE, EVENT_FINISH, EVENT_WAIT, EVENT_CLEAN}}
    for index in range(count):
        name_offset, _, _, _, address = struct.unpack_from("<IBBHQ", image, symbol_offset + index * 16)
        assert name_offset < string_size, "invalid symbol string"
        start = string_offset + name_offset
        name = image[start:image.index(0, start, string_offset + string_size)].decode()
        if name in matches:
            matches[name].append(address)
    def address_of(name):
        assert len(matches[name]) == 1, f"missing/ambiguous {name}"
        return matches[name][0]

    def read(address, length):
        locations = [file_offset + address - virtual for virtual, file_offset, size in segments
                     if virtual <= address and address + length <= virtual + size]
        assert len(locations) == 1, f"unmapped/ambiguous address {address:#x}"
        offset = locations[0]
        return image[offset:offset + length]

    for name, expected in CONTRACTS.items():
        address = address_of(name)
        assert read(address, len(expected)) == expected, f"changed {name}"
        print(f"PASS {name} at {address:#x}")
    # XNU EXTERNAL_HEADERS/mach-o/fixup-chains.h kernel-cache rebase:
    # target:30, cacheLevel:2, next:12, isAuth:1. This archived SystemKC
    # level-1 unslid base is zero; never apply this to a live slid pointer.
    for slot, name in ((0x188, EVENT_FINISH), (0x238, EVENT_WAIT), (0x148, EVENT_CLEAN)):
        raw = struct.unpack("<Q", read(address_of(EVENT_VTABLE) + 16 + slot, 8))[0]
        assert (raw >> 30) & 3 == 1 and raw >> 63 == 0, "unexpected cache level/auth"
        assert raw & 0x3fffffff == address_of(name), f"changed event virtual {slot:#x}"
    finish = read(address_of(EVENT_FINISH), 0x192)
    assert hashlib.sha256(finish).hexdigest() == \
        "34c3638c2485ef35e2fdb1e70efeff61e935972b1db36bc72ec3084b7207c010", "changed finishEvent"
    wait = read(address_of(EVENT_WAIT), 0x34)
    assert wait[0x1c:0x32] == bytes.fromhex(
        "48 8b 4f 10 31 c0 83 b9 c8 0d 00 00 00 0f 85 f5 01 00 00 41 89 f7"), \
        "changed waitForStamp non-hardware early-success branch"
    assert read(address_of(EVENT_WAIT) + 0x224, 15) == bytes.fromhex(
        "48 83 c4 28 5b 41 5c 41 5d 41 5e 41 5f 5d c3"), "changed wait epilogue"
    print("PASS event virtuals, finishEvent identity and waitForStamp early-success path")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: tahoe_ioaccel_mapping_contract_test.py SystemKernelExtensions.kc")
    check(sys.argv[1])
