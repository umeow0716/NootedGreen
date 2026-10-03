#!/usr/bin/env python3
"""Check a locally archived Tahoe KC; not a GPU completion/safety test."""

import hashlib
import pathlib
import struct
import sys


KC_SHA256 = "5cb1be1dc530b4b953a33943567589101d3ac46bb8cf90728566ee7e5b1fa214"
BOOT_SHA256 = "5cba9e36ceed5d73e1d569d1772bc46fecbd0359f824db689863e686d856ea3b"
IDENTIFIER = b"com.apple.iokit.IOAcceleratorFamily2"
EVENT_VTABLE = "__ZTV24IOAccelEventMachineFast2"
EVENT_FINISH = "__ZN24IOAccelEventMachineFast211finishEventEP12IOAccelEvent"
EVENT_WAIT = "__ZN20IOAccelEventMachine212waitForStampEijPj"
EVENT_CLEAN = "__ZN24IOAccelEventMachineFast210cleanEventEP12IOAccelEvent"
EVENT_TERMINATE = "__ZN24IOAccelEventMachineFast224deviceTerminatedUnlockedEv"
EVENT_SIGNAL = "__ZN20IOAccelEventMachine211signalStampEij"
CONTRACTS = {
    EVENT_TERMINATE:
        bytes.fromhex("55 48 89 e5 48 8b 47 28 48 85 c0 74 30 8b 4f 30 85 c9 "
                      "7e 29 48 8d 97 04 01 00 00 31 f6 4c 8b 04 f0 4d 85 c0 "
                      "74 08 8b 0a 41 89 08 8b 4f 30 48 ff c6 4c 63 c1 48 83 "
                      "c2 18 4c 39 c6 7c e0 0f ae f8 48 8d 05 65 00 04 00 "
                      "5d ff a0 60 02 00 00"),
    "__ZN22IOGraphicsAccelerator224deviceTerminatedUnlockedEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb 48 81 c7 c8 0d 00 00 "
                      "e8 e9 dc 46 eb 85 c0 75 09 83 bb d0 0d 00 00 00 "
                      "74 07 48 83 c4 08 5b 5d c3"),
    "__ZN22IOGraphicsAccelerator217enableAcceleratorEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb f6 87 92 0c 00 00 08 75 0c "
                      "48 8b bb 80 03 00 00 e8 d8 10 fd ff 80 8b 78 0c 00 00 02 "
                      "48 83 c4 08 5b 5d c3"),
    "__ZN22IOGraphicsAccelerator218disableAcceleratorEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb f6 87 92 0c 00 00 08 75 0c "
                      "48 8b bb 80 03 00 00 e8 86 10 fd ff 80 a3 78 0c 00 00 fd "
                      "48 83 c4 08 5b 5d c3"),
    "__ZN20IOAccelEventMachine233stopHardwareProgressTimerUnlockedEv":
        bytes.fromhex("55 48 89 e5 f6 47 74 01 74 1a 48 8b 47 60 48 85 c0 74 11 "
                      "c6 47 74 00 48 8b 08 48 89 c7 5d ff a1 18 02 00 00 5d c3"),
    "__ZN20IOAccelEventMachine234startHardwareProgressTimerUnlockedEv":
        bytes.fromhex("55 48 89 e5 f6 47 74 01 75 20 48 8b 47 60 48 85 c0 74 17 "
                      "c6 47 74 01 8b 77 70 48 8b 08 48 8b 89 d0 01 00 00 "
                      "48 89 c7 5d ff e1 5d c3"),
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


def check_boot_atomic(system, path):
    boot = pathlib.Path(path).read_bytes()
    assert hashlib.sha256(boot).hexdigest() == BOOT_SHA256, "unreviewed BootKC identity"
    segments = []
    kernel = []
    bases = []
    for command, offset in commands(boot, 0):
        if command == 0x19:
            f = struct.unpack_from("<II16sQQQQIIII", boot, offset)
            segments.append((f[3], f[5], f[6]))
            if f[2].rstrip(b"\0") == b"__HIB":
                bases.append(f[3])
        elif command == 0x80000035:
            _, _, _, file_offset, name_offset, _ = struct.unpack_from("<IIQQII", boot, offset)
            start = offset + name_offset
            if boot[start:boot.index(0, start)] == b"com.apple.kernel":
                kernel.append(file_offset)
    assert len(kernel) == len(bases) == 1, "missing/ambiguous kernel/base"
    symbols = {b"_OSIncrementAtomic": [], b"_thread_wakeup_prim": []}
    for command, offset in commands(boot, kernel[0]):
        if command != 2:
            continue
        symbol_offset, count, string_offset, _ = struct.unpack_from("<6I", boot, offset)[2:]
        for index in range(count):
            name_offset, _, _, _, address = struct.unpack_from("<IBBHQ", boot, symbol_offset + 16 * index)
            start = string_offset + name_offset
            name = boot[start:boot.index(0, start)]
            if name in symbols:
                symbols[name].append(address)
    assert all(len(values) == 1 for values in symbols.values()), "missing/ambiguous kernel imports"
    assert system[0x10132:0x10138] == bytes.fromhex("ff 25 60 40 01 00"), "changed atomic import stub"
    raw = struct.unpack_from("<Q", system, 0x24198)[0]
    assert (raw >> 30) & 3 == 0 and raw >> 63 == 0, "unexpected atomic import cache level/auth"
    address = bases[0] + (raw & 0x3fffffff)
    assert address == symbols[b"_OSIncrementAtomic"][0], "atomic import does not resolve to OSIncrementAtomic"
    wake_stub = 0x10cae
    assert system[wake_stub:wake_stub + 2] == b"\xff\x25", "changed wakeup import stub"
    wake_pointer = wake_stub + 6 + struct.unpack_from("<i", system, wake_stub + 2)[0]
    wake_raw = struct.unpack_from("<Q", system, wake_pointer)[0]
    assert (wake_raw >> 30) & 3 == 0 and wake_raw >> 63 == 0, "unexpected wakeup cache level/auth"
    assert bases[0] + (wake_raw & 0x3fffffff) == symbols[b"_thread_wakeup_prim"][0], \
        "signalStamp import does not resolve to thread_wakeup_prim"
    locations = [f + address - v for v, f, size in segments if v <= address and address + 15 <= v + size]
    assert len(locations) == 1, "unmapped atomic implementation"
    offset = locations[0]
    assert boot[offset:offset + 15] == bytes.fromhex(
        "55 48 89 e5 b8 01 00 00 00 f0 0f c1 07 5d c3"), "changed atomic increment"
    print("PASS termination atomic and event wakeup imports across SystemKC/BootKC")


def check(path, boot_path=None):
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
    matches = {name: [] for name in {*CONTRACTS, EVENT_VTABLE, EVENT_FINISH, EVENT_WAIT, EVENT_CLEAN, EVENT_SIGNAL}}
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
    for slot, name in ((0x188, EVENT_FINISH), (0x238, EVENT_WAIT),
                       (0x148, EVENT_CLEAN), (0x250, EVENT_TERMINATE), (0x228, EVENT_SIGNAL)):
        raw = struct.unpack("<Q", read(address_of(EVENT_VTABLE) + 16 + slot, 8))[0]
        assert (raw >> 30) & 3 == 1 and raw >> 63 == 0, "unexpected cache level/auth"
        assert raw & 0x3fffffff == address_of(name), f"changed event virtual {slot:#x}"
    finish = read(address_of(EVENT_FINISH), 0x192)
    assert hashlib.sha256(finish).hexdigest() == \
        "34c3638c2485ef35e2fdb1e70efeff61e935972b1db36bc72ec3084b7207c010", "changed finishEvent"
    assert hashlib.sha256(read(address_of(EVENT_SIGNAL), 0xa8)).hexdigest() == \
        "02c75b9f3864be75d75b3b2fd1b777a52e016265036c7e77c096d810db3e260b", "changed signalStamp"
    wait = read(address_of(EVENT_WAIT), 0x34)
    assert wait[0x1c:0x32] == bytes.fromhex(
        "48 8b 4f 10 31 c0 83 b9 c8 0d 00 00 00 0f 85 f5 01 00 00 41 89 f7"), \
        "changed waitForStamp non-hardware early-success branch"
    assert read(address_of(EVENT_WAIT) + 0x224, 15) == bytes.fromhex(
        "48 83 c4 28 5b 41 5c 41 5d 41 5e 41 5f 5d c3"), "changed wait epilogue"
    print("PASS event virtuals, finishEvent identity and waitForStamp early-success path")
    if boot_path is not None:
        check_boot_atomic(image, boot_path)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: tahoe_ioaccel_mapping_contract_test.py SystemKernelExtensions.kc [BootKernelExtensions.kc]")
    check(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)
