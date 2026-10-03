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
EVENT_RESTART = "__ZN20IOAccelEventMachine215restart_channelEv"
EVENT_MERGE_EXCLUDING = "__ZN24IOAccelEventMachineFast219mergeEventExcludingEP12IOAccelEventS1_i"
EVENT_SET_STAMP = "__ZN24IOAccelEventMachineFast213setEventStampEiP12IOAccelEvent"
EVENT_INCREMENT = "__ZN24IOAccelEventMachineFast214incrementStampEi"
EVENT_WRITE_STAMP = "__ZN24IOAccelEventMachineFast217writeStampCommandEiP17IOAccelEventQueueP17vendevtCommandRec"
EVENT_SCRUB = "__ZN24IOAccelEventMachineFast210scrubEventEP12IOAccelEvent"
SHARED_SCRUB = "__ZN14IOAccelShared211scrubEventsEv"
RESOURCE_SCRUB = "__ZN16IOAccelResource211scrubEventsEv"
SHARED_VTABLE = "__ZTV14IOAccelShared2"
RESOURCE_VTABLE = "__ZTV16IOAccelResource2"
GET_DATA_BUFFER = "__ZN15IOAccelContext213getDataBufferEP29IOAccelContextGetDataBufferInP30IOAccelContextGetDataBufferOutP22IOAccelResourcePrivatey"
EVENT_FINISH_UNLOCKED = "__ZN24IOAccelEventMachineFast219finishEventUnlockedEP12IOAccelEvent"
EVENT_HARDWARE_ERROR = "__ZN20IOAccelEventMachine219signalHardwareErrorE15eRestartRequesti"
EVENT_INIT = "__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent"
EVENT_COPY = "__ZN24IOAccelEventMachineFast29copyEventEP12IOAccelEventS1_"
# Symbol-bounded bodies reviewed locally. These identities do not certify
# overridden resource methods, iterator locking, DMA completion or host safety.
SCRUB_BODIES = {
    SHARED_SCRUB: (0x54, "391e92190ff359bd4c4bede93538b83d0fc7f49677456f504528bc82ba331e00"),
    RESOURCE_SCRUB: (0xc2, "4f0a89f94134348f87d80dc77e6e66c800a85e4c50d659c6303701f653cc700d"),
    "__ZN13IOAccelMemory14scrubAllEventsEv":
        (0x5a, "43bc8b5f00af74bdc67cbad08ff4994fd24ca075fe0d2c05a45a7604c71aa492"),
    EVENT_SCRUB: (0x7e, "7b3a41f9948c644a06cb48c6e86f7a014e28b9f05aee296f5c813efc3c822833"),
}
LOCK_COPIES = {
    "__ZN22IOGraphicsAccelerator215acceleratorLockEv": {
        0x14b6c7a6: "fed7918d5caa2b65a7400e742fdad37cea80fcd1aaba8a648840d44da1f48c1e",
        0x14b7e0ba: "4e9a4289539e7a9c6b462cc5a19efc669fba9d7d397db0a8726f34456d1e4253",
        0x14b901b0: "453eaae8d172876d5d7df2346e0225d3671589c04b521f4319390f4996baf4d0",
        0x14b9975a: "901b711b0117df4a425bf80c4b2b72213d1695fd27a79f5687a0b70aecaa18c3",
    },
    "__ZN22IOGraphicsAccelerator217acceleratorUnlockEv": {
        0x14b6c7f8: "c5a432f48d0f3208f2525bd995dfc1a5fbd1239af11163b853e6d38983777a2d",
        0x14b70806: "b4f0dbfafce3dc7e3f14d8f56f98f82b54037474c765f71832c8937008395072",
        0x14b7e084: "bdbe6331672d1f8760649b884188f8e06163016b0e1a7bc4bc6bb3bf5a49ce15",
        0x14b90202: "3b470039180b89751871154bec962b1b4a7e508555ce8a3de7ea6c2493fc3bf6",
        0x14b997ac: "ba3edbb0b0b596697ac6b352f469864fd98ca922d99b0bc67a169ab587255977",
    },
}
CONTRACTS = {
    "__ZNK22IOGraphicsAccelerator223isLockedByCurrentThreadEv":
        bytes.fromhex("55 48 89 e5 b0 01 5d c3"),
    "__ZN22IOGraphicsAccelerator29lock_busyEv":
        bytes.fromhex("55 48 89 e5 48 81 c7 58 01 00 00 5d e9 7f 93 46 eb 90"),
    "__ZN22IOGraphicsAccelerator211unlock_busyEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb 48 81 c7 58 01 00 00 e8 63 93 46 eb "
                      "83 f8 01 75 39 f6 83 78 0c 00 00 04 75 30 48 8b 93 60 01 00 00 "
                      "48 85 d2 74 24 48 8b 03 48 8b 8b 68 01 00 00 48 8b 80 38 07 00 00 "
                      "48 89 df be 01 80 ff e3 45 31 c0 48 83 c4 08 5b 5d ff e0 "
                      "48 83 c4 08 5b 5d c3"),
    "__ZN22IOGraphicsAccelerator218acceleratorDidLockEPKci":
        bytes.fromhex("83 3d 11 f5 03 00 00 74 21 55 48 89 e5 e8 92 90 46 eb "
                      "bf 19 00 12 85 48 89 c6 31 d2 31 c9 45 31 c0 45 31 c9 "
                      "5d e9 a4 90 46 eb c3 90"),
    "__ZN22IOGraphicsAccelerator221acceleratorWillUnlockEPKci":
        bytes.fromhex("83 3d e5 f4 03 00 00 74 21 55 48 89 e5 e8 66 90 46 eb "
                      "bf 1a 00 12 85 48 89 c6 31 d2 31 c9 45 31 c0 45 31 c9 "
                      "5d e9 78 90 46 eb c3 90"),
    "__ZN17IOAccelSharedList8IteratorC1ERS_":
        bytes.fromhex("55 48 89 e5 48 8b 06 48 89 07 5d c3"),
    "__ZN17IOAccelSharedList8Iterator13getNextSharedEv":
        bytes.fromhex("48 8b 07 48 85 c0 74 0c 55 48 89 e5 48 8b 48 10 48 89 0f 5d c3 90"),
    "__ZN19IOAccelResourceList15ReverseIteratorC1ERS_":
        bytes.fromhex("55 48 89 e5 48 8b 46 08 48 89 07 5d c3 90"),
    "__ZN19IOAccelResourceList15ReverseIterator15getPrevResourceEv":
        bytes.fromhex("48 8b 07 48 85 c0 74 0c 55 48 89 e5 48 8b 48 50 48 89 0f 5d c3 90"),
    "__ZN22IOGraphicsAccelerator211scrubEventsEv":
        bytes.fromhex("55 48 89 e5 41 57 41 56 53 48 83 ec 28 48 89 fb "
                      "49 bf aa aa aa aa aa aa aa aa 4c 8d 75 e0 4d 89 3e "
                      "48 8d b7 88 0a 00 00 4c 89 f7 e8 b2 96 fd ff "
                      "4c 89 f7 e8 b6 96 fd ff 48 85 c0 74 0e 48 8b 08 "
                      "48 89 c7 ff 91 28 01 00 00 eb e5 4c 8d 75 c8 4d 89 3e "
                      "4d 89 7e 08 4d 89 7e 10 48 81 c3 00 0b 00 00 4c 89 f7 "
                      "48 89 de e8 84 9c fd ff 4c 89 f7 e8 8a 9c fd ff "
                      "48 85 c0 74 1d 48 8d 5d c8 48 8b 08 48 89 c7 "
                      "ff 91 28 02 00 00 48 89 df e8 6d 9c fd ff 48 85 c0 "
                      "75 e7 48 83 c4 28 5b 41 5e 41 5f 5d c3 90"),
    "__ZN15IOAccelChannel213setEventStampEP12IOAccelEvent":
        bytes.fromhex("55 48 89 e5 48 89 f2 48 8b 47 18 48 8b 80 80 03 00 00 "
                      "8b 77 20 48 8b 08 48 8b 89 d0 01 00 00 48 89 c7 5d ff e1 90"),
    "__ZN15IOAccelChannel214incrementStampEv":
        bytes.fromhex("55 48 89 e5 48 8b 47 18 48 8b 80 80 03 00 00 8b 77 20 "
                      "48 8b 08 48 8b 89 d8 01 00 00 48 89 c7 5d ff e1"),
    "__ZN15IOAccelChannel219mergeEventExcludingEP12IOAccelEventS1_":
        bytes.fromhex("55 48 89 e5 48 8b 47 18 48 8b 80 80 03 00 00 8b 4f 20 "
                      "48 8b 38 4c 8b 87 c8 01 00 00 48 89 c7 5d 41 ff e0 90"),
    EVENT_INCREMENT:
        bytes.fromhex("55 48 89 e5 41 57 41 56 53 50 48 89 fb 48 63 c6 48 c1 e0 03 "
                      "4c 8d 34 40 42 8b 84 37 fc 00 00 00 44 8d 78 01 44 31 f8 "
                      "3d 00 00 00 40 72 0d 48 8b 7b 10 48 8b 07 ff 90 f0 08 00 00 "
                      "46 89 bc 33 fc 00 00 00 48 8b 43 10 ff 80 a0 00 00 00 "
                      "48 83 c4 08 5b 41 5e 41 5f 5d c3"),
    EVENT_WRITE_STAMP:
        bytes.fromhex("55 48 89 e5 48 63 f6 48 8d 04 76 8b 84 c7 fc 00 00 00 "
                      "48 8b 17 4c 8b 82 a0 02 00 00 48 89 ca 89 c1 5d 41 ff e0 90"),
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
    symbols = {name: [] for name in (b"_OSIncrementAtomic", b"_OSDecrementAtomic", b"_thread_wakeup_prim",
                                   b"__ZN15IORegistryEntry18getRegistryEntryIDEv", b"_kernel_debug",
                                   b"_IOLockLock", b"_IOLockUnlock")}
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
    assert system[0x1012c:0x10132] == bytes.fromhex("ff 25 5e 40 01 00"), "changed decrement import stub"
    decrement_raw = struct.unpack_from("<Q", system, 0x24190)[0]
    assert (decrement_raw >> 30) & 3 == 0 and decrement_raw >> 63 == 0, "unexpected decrement cache level/auth"
    decrement_address = bases[0] + (decrement_raw & 0x3fffffff)
    assert decrement_address == symbols[b"_OSDecrementAtomic"][0], "busy decrement import does not resolve to OSDecrementAtomic"
    decrement_locations = [f + decrement_address - v for v, f, size in segments
                           if v <= decrement_address and decrement_address + 15 <= v + size]
    assert len(decrement_locations) == 1, "unmapped decrement implementation"
    decrement_offset = decrement_locations[0]
    assert boot[decrement_offset:decrement_offset + 15] == bytes.fromhex(
        "55 48 89 e5 b8 ff ff ff ff f0 0f c1 07 5d c3"), "changed atomic decrement"
    for stub, name in ((0x101a4, b"__ZN15IORegistryEntry18getRegistryEntryIDEv"),
                       (0x101ce, b"_kernel_debug"), (0x10012, b"_IOLockLock"),
                       (0x10018, b"_IOLockUnlock")):
        assert system[stub:stub + 2] == b"\xff\x25", "changed lock notification import stub"
        pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
        raw_pointer = struct.unpack_from("<Q", system, pointer)[0]
        assert (raw_pointer >> 30) & 3 == 0 and raw_pointer >> 63 == 0, "unexpected lock notification cache level/auth"
        assert bases[0] + (raw_pointer & 0x3fffffff) == symbols[name][0], "changed lock notification import identity"
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
    print("PASS busy/termination atomics, lock notifications and event wakeup imports across SystemKC/BootKC")


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
    matches = {name: [] for name in {*CONTRACTS, *SCRUB_BODIES, *LOCK_COPIES, SHARED_VTABLE, RESOURCE_VTABLE,
                                    EVENT_VTABLE, EVENT_FINISH, EVENT_WAIT, EVENT_CLEAN, EVENT_SIGNAL, EVENT_RESTART,
                                    EVENT_MERGE_EXCLUDING, EVENT_SET_STAMP, GET_DATA_BUFFER,
                                    EVENT_INIT, EVENT_COPY, EVENT_FINISH_UNLOCKED, EVENT_HARDWARE_ERROR}}
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
    for name, (length, digest) in SCRUB_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, f"changed {name}"
    # There are multiple real local definitions, not one ambiguous address to
    # pick arbitrarily. Require the full reviewed copy inventory and bodies.
    for name, copies in LOCK_COPIES.items():
        assert sorted(matches[name]) == sorted(copies), f"changed local mutex copy inventory: {name}"
        length = 0x52 if "acceleratorLockEv" in name else 0x36
        for address, digest in copies.items():
            assert hashlib.sha256(read(address, length)).hexdigest() == digest, f"changed mutex copy {address:#x}"
    print("PASS all four local mutex-lock and five mutex-unlock bodies")
    buffer_start = address_of(GET_DATA_BUFFER)
    buffer_body = read(buffer_start, 0x9e4)
    assert hashlib.sha256(buffer_body).hexdigest() == \
        "7840d4fbada0dbedc42efd2896dcafefcc54b25c03833e00cf57b55e2ab7607b", "changed getDataBuffer"
    # Helper-only call graph scans miss the second, inlined unlock/lock window.
    for offset, target in ((0x244, 0x14b6c7f8), (0x26f, 0x14b6c7a6),
                           (0x552, 0x10018), (0x593, 0x10012)):
        assert buffer_body[offset] == 0xe8, "changed buffer wait lock edge"
        assert buffer_start + offset + 5 + struct.unpack_from("<i", buffer_body, offset + 1)[0] == target, \
            "changed buffer wait lock target"
    for offset in (0x25e, 0x56c):
        assert buffer_body[offset:offset + 6] == bytes.fromhex("ff 90 78 01 00 00"), "changed unlocked event wait"
    for offset in (0x29b, 0x5e9):
        assert buffer_body[offset:offset + 4] == bytes.fromhex("4d 8b 6e 38"), "changed post-wait buffer reload"
    print("PASS getDataBuffer helper and inlined mutex-release wait windows")
    unlocked_start = address_of(EVENT_FINISH_UNLOCKED)
    unlocked_body = read(unlocked_start, 0x194)
    assert hashlib.sha256(unlocked_body).hexdigest() == \
        "fb78fffbb00c991767abc188fa7bbf743432de41b03ddd3c839d008963809dc2", "changed finishEventUnlocked"
    assert unlocked_body[0x6f:0x80] == bytes.fromhex(
        "49 8b 4e 10 83 b9 c8 0d 00 00 00 0f 85 f7 00 00 00"), "changed unlocked finish termination bypass"
    assert unlocked_body[0xdc:0xe2] == bytes.fromhex("ff 90 38 02 00 00"), "changed unlocked stamp wait virtual"
    assert unlocked_body[0xfa] == 0xe8 and unlocked_start + 0xff + struct.unpack_from("<i", unlocked_body, 0xfb)[0] == \
        address_of(EVENT_HARDWARE_ERROR), "changed unlocked timeout error request"
    assert hashlib.sha256(read(address_of(EVENT_HARDWARE_ERROR), 0x112)).hexdigest() == \
        "fa0c96832f317613aa5389d04fdcae250b356be84cac4c009438b77b96afd649", "changed software hardware-error signaling"
    print("PASS inherited unlocked finish retry and software error-request identities")
    for table, slot, name in ((SHARED_VTABLE, 0x128, SHARED_SCRUB),
                              (RESOURCE_VTABLE, 0x228, RESOURCE_SCRUB),
                              (EVENT_VTABLE, 0x270, EVENT_SCRUB)):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert (raw >> 30) & 3 == 1 and raw >> 63 == 0, "unexpected scrub cache level/auth"
        assert raw & 0x3fffffff == address_of(name), f"changed scrub virtual {slot:#x}"
    scrub = read(address_of(EVENT_SCRUB), 0x7e)
    assert scrub[0x58:0x66] == bytes.fromhex(
        "4c 8b 47 10 41 83 b8 c8 0d 00 00 00 74 0f"), "changed scrub termination bypass"
    assert scrub[0x66:0x6a] == bytes.fromhex("48 89 14 ce"), "changed scrub event clearing"
    print("PASS shared/resource/memory scrub graph and non-hardware termination bypass")
    # XNU EXTERNAL_HEADERS/mach-o/fixup-chains.h kernel-cache rebase:
    # target:30, cacheLevel:2, next:12, isAuth:1. This archived SystemKC
    # level-1 unslid base is zero; never apply this to a live slid pointer.
    for slot, name in ((0x188, EVENT_FINISH), (0x238, EVENT_WAIT),
                       (0x148, EVENT_CLEAN), (0x250, EVENT_TERMINATE), (0x228, EVENT_SIGNAL),
                       (0x1c8, EVENT_MERGE_EXCLUDING), (0x1d0, EVENT_SET_STAMP),
                       (0x1d8, EVENT_INCREMENT), (0x1e0, EVENT_WRITE_STAMP),
                       (0x140, EVENT_INIT), (0x1b0, EVENT_COPY), (0x178, EVENT_FINISH_UNLOCKED)):
        raw = struct.unpack("<Q", read(address_of(EVENT_VTABLE) + 16 + slot, 8))[0]
        assert (raw >> 30) & 3 == 1 and raw >> 63 == 0, "unexpected cache level/auth"
        assert raw & 0x3fffffff == address_of(name), f"changed event virtual {slot:#x}"
    finish = read(address_of(EVENT_FINISH), 0x192)
    assert hashlib.sha256(finish).hexdigest() == \
        "34c3638c2485ef35e2fdb1e70efeff61e935972b1db36bc72ec3084b7207c010", "changed finishEvent"
    assert hashlib.sha256(read(address_of(EVENT_SIGNAL), 0xa8)).hexdigest() == \
        "02c75b9f3864be75d75b3b2fd1b777a52e016265036c7e77c096d810db3e260b", "changed signalStamp"
    assert hashlib.sha256(read(address_of(EVENT_RESTART), 0x23e)).hexdigest() == \
        "35a836d5773e442205b5f415e658630d2e643ed364a39bb800312d48b424345c", "changed restart_channel"
    assert hashlib.sha256(read(address_of(EVENT_MERGE_EXCLUDING), 0x1d4)).hexdigest() == \
        "20517fccc05f6ca02f2e15867e6b55ef6ec21ab7872f14a5abd9410bd8b8b4d8", "changed mergeEventExcluding"
    assert hashlib.sha256(read(address_of(EVENT_SET_STAMP), 0x9e)).hexdigest() == \
        "6240e1c9918181dd5d32c49d5fe01dab22e7705dc0d4d1d94b1e7189d8c8c995", "changed setEventStamp"
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
