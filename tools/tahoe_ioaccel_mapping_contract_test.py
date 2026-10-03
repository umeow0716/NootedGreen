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
EVENT_DISABLE_STAMP_LOCKED = "__ZN20IOAccelEventMachine223disable_stamp_interruptEi"
EVENT_ENABLE_STAMP = "__ZN20IOAccelEventMachine220enableStampInterruptEi"
EVENT_DISABLE_STAMP = "__ZN20IOAccelEventMachine221disableStampInterruptEi"
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
    EVENT_ENABLE_STAMP: bytes.fromhex("55 48 89 e5 5d c3"),
    EVENT_DISABLE_STAMP: bytes.fromhex("55 48 89 e5 5d c3"),
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
                                   b"_IOLockLock", b"_IOLockUnlock", b"_assert_wait_deadline",
                                   b"_thread_block", b"_clock_interval_to_deadline",
                                   b"__ZN10IOWorkLoop8workLoopEv",
                                   b"__ZN10IOWorkLoop14runActionBlockEU13block_pointerFivE",
                                   b"__ZTV22IOInterruptEventSource", b"__ZTV18IOTimerEventSource",
                                   b"__ZN22IOInterruptEventSource23normalInterruptOccurredEPvP9IOServicei",
                                   b"__ZN18IOTimerEventSource12setTimeoutUSEj",
                                   b"__ZN18IOTimerEventSource13cancelTimeoutEv",
                                   b"_thread_call_cancel", b"_thread_call_cancel_wait",
                                   b"__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E",
                                   b"__ZN18IOTimerEventSource4initEjP8OSObjectPFvS1_PS_E",
                                   b"__ZN18IOTimerEventSource4initEP8OSObjectPFvS1_PS_E",
                                   b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE",
                                   b"__ZN18IOTimerEventSource14setTimeoutFuncEv")}
    symbols[b"__ZN18IOTimerEventSource11setWorkLoopEP10IOWorkLoop"] = []
    symbols[b"__ZTV8OSObject"] = []
    symbols[b"__ZN8OSObject4freeEv"] = []
    symbols[b"__ZN8OSObjectdlEPvm"] = []
    symbols[b"__ZNK13IOEventSource11getWorkLoopEv"] = []
    symbols[b"__ZN18IOTimerEventSource7disableEv"] = []
    symbols[b"__ZN18IOTimerEventSource10wakeAtTimeEjyy"] = []
    symbols[b"__ZN18IOTimerEventSource17timeoutAndReleaseEPvS0_"] = []
    for name in (b"__ZTV10IOWorkLoop", b"__ZN10IOWorkLoop8openGateEv",
                 b"__ZN10IOWorkLoop4initEv",
                 b"__ZTV13IOCommandGate", b"__ZN13IOCommandGate10runCommandEPvS0_S0_S0_",
                 b"__ZN13IOCommandGate4initEP8OSObjectPFiS1_PvS2_S2_S2_E",
                 b"__ZN13IOEventSource9setActionEPFvP8OSObjectzE",
                 b"__ZNK13IOCommandGate9MetaClass5allocEv",
                 b"__ZN13IOCommandGate10gMetaClassE", b"__ZTVN13IOCommandGate9MetaClassE",
                 b"__ZN13IOCommandGate9runActionEPFiP8OSObjectPvS2_S2_S2_ES2_S2_S2_S2_",
                 b"__ZN10IOWorkLoop13_maintRequestEPvS0_S0_S0_",
                 b"__ZN10IOWorkLoop9closeGateEv",
                 b"__ZN10IOWorkLoop17removeEventSourceEP13IOEventSource"):
        symbols[name] = []
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
    def kernel_read(address, length):
        matches = [f + address - v for v, f, size in segments
                   if v <= address and address + length <= v + size]
        assert len(matches) == 1, "unmapped/ambiguous event-source implementation"
        return boot[matches[0]:matches[0] + length]

    # These Boot KC vtable entries are canonical pointers, NOT System KC
    # chained cache-level targets. Do not silently apply the latter decoder.
    for table, slot, method, length, digest in (
            (b"__ZTV18IOTimerEventSource", 0x128,
             b"__ZN18IOTimerEventSource11setWorkLoopEP10IOWorkLoop", 0x60,
             "c65344022bf8d9bc0d0b8e7531c46ebb93e5f1330b3c1b230cc950fd0eb76ff8"),
            (b"__ZTV18IOTimerEventSource", 0x158,
             b"__ZN18IOTimerEventSource7disableEv", 0x60,
             "dfdaf0a822e0cead05bf9fd7676a3aaa0a78cff67f3cd9c270be9ef5c006e573"),
            (b"__ZTV13IOCommandGate", 0x140,
             b"__ZN13IOEventSource9setActionEPFvP8OSObjectzE", 0x50,
             "8cdc3bc5b1db948a9d976aa06d9bcb318519e859a4fd655f687ce8870b7d04d6"),
            (b"__ZTV13IOCommandGate", 0x1b8,
             b"__ZN13IOCommandGate4initEP8OSObjectPFiS1_PvS2_S2_S2_E", 0x30,
             "168ec388c4b98c395fa7cb00389015d571f9153b32d6a717f8f41b9955c125b3"),
            (b"__ZTV10IOWorkLoop", 0x88, b"__ZN10IOWorkLoop4initEv", 0x1a0,
             "3f0dee6fd2d4c2cbf11c7fcd7b0788c9bf2abb3c08526ae060fe159dfb7367f7"),
            (b"__ZTV13IOCommandGate", 0x1c8,
             b"__ZN13IOCommandGate9runActionEPFiP8OSObjectPvS2_S2_S2_ES2_S2_S2_S2_", 0x280,
             "b99aaabcc82bfb3f8116857b0ceed68604d84a049fc2068cd36335bd3d8ace12"),
            (b"__ZTV13IOCommandGate", 0x1c0,
             b"__ZN13IOCommandGate10runCommandEPvS0_S0_S0_", 0x30,
             "e3e3a145d770ee11a0fe2424d9adda5df15c0be7c5fa458c3278a44d5ee7dd41"),
            (b"__ZTV10IOWorkLoop", 0x178, b"__ZN10IOWorkLoop8openGateEv", 0x70,
             "4474d3fed4f663608045d23044b0ab55df1259532e987df6ae9ee3c9c3ad9e8a"),
            (b"__ZTV10IOWorkLoop", 0x180, b"__ZN10IOWorkLoop9closeGateEv", 0x90,
             "423e960ba7f1dcd3e429a66e4fe1e83046e4eb9ec13d301178fe085736ff5d89"),
            (b"__ZTV10IOWorkLoop", 0x148, b"__ZN10IOWorkLoop17removeEventSourceEP13IOEventSource", 0x30,
             "d055def6065f00813231758928fc7c6a20c8383b7f8ff890f7353517cde22747"),
            (b"__ZTV18IOTimerEventSource", 0x1c0,
             b"__ZN18IOTimerEventSource4initEP8OSObjectPFvS1_PS_E", 0x50,
             "a7e670f9a67cfecffd99d5280e9841fa418f6f9281d45edef55f3a2598ce63bb"),
            (b"__ZTV18IOTimerEventSource", 0x218,
             b"__ZN18IOTimerEventSource13cancelTimeoutEv", 0x70,
             "ca6a6e9adbde9fcd3242f53c7c407c5dbc8b737620144c1ffc34f1aae08cb4ac"),
            (b"__ZTV22IOInterruptEventSource", 0x1e0,
             b"__ZN22IOInterruptEventSource23normalInterruptOccurredEPvP9IOServicei", 0x140,
             "7a10ac711e79fee59301c61844df895055cc3e47d20e083526ac0b361640e9bb"),
            (b"__ZTV18IOTimerEventSource", 0x1d8,
             b"__ZN18IOTimerEventSource12setTimeoutUSEj", 0x20,
             "521f14403d11af5a02a549db936f42356c779fc6914a43036c671eb1cf8caeb5")):
        target = symbols[method][0]
        assert struct.unpack("<Q", kernel_read(symbols[table][0] + 16 + slot, 8))[0] == target, \
            "changed kernel event-source virtual target"
        assert hashlib.sha256(kernel_read(target, length)).hexdigest() == digest, \
            "changed reviewed kernel event-source body"
    normal_body = kernel_read(symbols[b"__ZN22IOInterruptEventSource23normalInterruptOccurredEPvP9IOServicei"][0], 0x140)
    assert normal_body.count(bytes.fromhex("ff 43 54")) == 1, "changed pending interrupt increment"
    assert normal_body.count(bytes.fromhex("48 8b 7b 30 48 8b 07 ff 90 70 01 00 00")) == 1, \
        "changed workloop notification virtual"
    print("PASS Boot KC interrupt pending notification and microsecond timer base virtuals")
    cancel = symbols[b"__ZN18IOTimerEventSource13cancelTimeoutEv"][0]
    for offset, method in ((0x1e, b"_thread_call_cancel"), (0x25, b"_thread_call_cancel_wait")):
        instruction = kernel_read(cancel + offset, 5)
        assert instruction[0] == 0xe8 and cancel + offset + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[method][0], \
            "changed timer cancel versus cancel-wait branch target"
    assert kernel_read(cancel + 0x14, 4) == bytes.fromhex("f6 43 2a 02"), \
        "changed timer active-mode wait selector"
    print("PASS Boot KC timer cancel virtual and conditional cancel-wait dispatch")
    # The setup symbol window includes a five-entry jump table after code.
    # Fix both the reviewed window and data targets, not a linear decoding of
    # that table as instructions. Init's further virtual delegation is pending.
    for method, length, digest in (
            (b"__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E", 0xc0,
             "79088c15da63def57438cdb0a553488e577861f0100f3a0c442a484585a51d5a"),
            (b"__ZN18IOTimerEventSource4initEjP8OSObjectPFvS1_PS_E", 0x20,
             "191a549b208c6e8133230403a4342f3f15b278a09ce9f57652b7e9a33f2dd046"),
            (b"__ZN18IOTimerEventSource14setTimeoutFuncEv", 0x120,
             "031283000cf6d490d994506e758769c7f9712765b731b2d9b03f93dc42dfb13c")):
        assert hashlib.sha256(kernel_read(symbols[method][0], length)).hexdigest() == digest, \
            "changed timer factory/options/setup window"
    setup = symbols[b"__ZN18IOTimerEventSource14setTimeoutFuncEv"][0]
    table = setup + 0x104
    for index, offset in enumerate((0x70, 0xb6, 0xa4, 0xad, 0x9b)):
        assert table + struct.unpack("<i", kernel_read(table + 4 * index, 4))[0] == setup + offset, \
            "changed timer priority jump-table target"
    factory = symbols[b"__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E"][0]
    assert kernel_read(factory + 0x8d, 5) == bytes.fromhex("be 01 00 00 00"), "changed default timer options"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV18IOTimerEventSource"][0] + 16 + 0x220, 8))[0] == symbols[b"__ZN18IOTimerEventSource4initEjP8OSObjectPFvS1_PS_E"][0], \
        "changed timer options init virtual"
    print("PASS Boot KC default timer factory/options and setup jump table")
    timer_init = symbols[b"__ZN18IOTimerEventSource4initEP8OSObjectPFvS1_PS_E"][0]
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV18IOTimerEventSource"][0] + 16 + 0x1b8, 8))[0] == setup, \
        "changed timer init setup virtual"
    call = timer_init + 9
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE"][0], \
        "changed timer base event-source init edge"
    assert kernel_read(timer_init + 0x18, 6) == bytes.fromhex("ff 90 b8 01 00 00"), \
        "changed timer setup dispatch instruction"
    print("PASS Boot KC timer init delegation reaches pinned setup virtual")
    timeout = symbols[b"__ZN18IOTimerEventSource17timeoutAndReleaseEPvS0_"][0]
    body = kernel_read(timeout, 0x120)
    assert hashlib.sha256(body).hexdigest() == "aea398e58d775cac636c1c9275b989e05b4eb8322ccfeb71c8c0434afbc9e2dc", \
        "changed reviewed passive timer callback body"
    for offset, instruction in (
            (0x4c, "ff 90 80 01 00 00"),  # workloop gate entry
            (0x7f, "44 39 20"),           # generation comparison
            (0xf0, "ff 90 78 01 00 00"), # workloop gate exit
            (0x117, "ff 60 28")):        # source release tail
        assert kernel_read(timeout + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed passive timer generation/gate/release instruction"
    print("PASS Boot KC passive timer generation, workloop gate and release body")
    for method, offset, opcode, target in (
            (b"__ZN10IOWorkLoop9closeGateEv", 0x23, 0xe8, b"_IOLockLock"),
            (b"__ZN10IOWorkLoop8openGateEv", 0x5c, 0xe9, b"_IOLockUnlock")):
        call = symbols[method][0] + offset
        instruction = kernel_read(call, 5)
        assert instruction[0] == opcode and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[target][0], \
            "changed workloop recursive gate mutex edge"
    print("PASS Boot KC recursive workloop gate and removal delegation bodies")
    removal = symbols[b"__ZN10IOWorkLoop17removeEventSourceEP13IOEventSource"][0]
    for offset, encoded in ((0x4, "48 89 f2"),
                            (0x7, "48 8b 7f 20"),
                            (0xe, "48 8b 80 c0 01 00 00"),
                            (0x15, "be 01 00 00 00"),
                            (0x20, "ff e0")):
        expected = bytes.fromhex(encoded)
        assert kernel_read(removal + offset, len(expected)) == expected, \
            "changed synchronous remove delegation/operation selector"
    print("PASS Boot KC removal delegates operation 1 and source to control gate")
    command = symbols[b"__ZN13IOCommandGate10runCommandEPvS0_S0_S0_"][0]
    assert kernel_read(command + 0x13, 11) == bytes.fromhex("48 8b 77 20 48 8b 80 c8 01 00 00"), \
        "changed command stored-action/runAction virtual dispatch"
    print("PASS Boot KC command-gate removal wrapper preserves action delegation")
    action = symbols[b"__ZN13IOCommandGate9runActionEPFiP8OSObjectPvS2_S2_S2_ES2_S2_S2_S2_"][0]
    for offset, instruction in ((0x43, "ff 90 80 01 00 00"),
                                (0xa5, "41 ff d7"),
                                (0x100, "ff 90 78 01 00 00"),
                                (0x155, "ff 90 90 01 00 00")):
        assert kernel_read(action + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed command action gate/invocation/sleep instruction"
    print("PASS Boot KC command action gated invocation and disabled-gate sleep body")
    maintenance = symbols[b"__ZN10IOWorkLoop13_maintRequestEPvS0_S0_S0_"][0]
    assert hashlib.sha256(kernel_read(maintenance, 0x290)).hexdigest() == \
        "c442e1b551f044431ce91ab6d9a0288f45de5571d4557c3817130109f5ece1fb", \
        "changed workloop maintenance add/remove body"
    for offset, instruction in ((0x236, "ff 90 28 01 00 00"),
                                (0x244, "ff 90 30 01 00 00"),
                                (0x250, "ff 50 28")):
        assert kernel_read(maintenance + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed source detach/next-clear/release order"
    print("PASS Boot KC maintenance removal detach and reference-release body")
    workloop_table = symbols[b"__ZTV10IOWorkLoop"][0]
    assert struct.unpack("<Q", kernel_read(workloop_table + 16 + 0x118, 8))[0] == maintenance, \
        "changed base workloop maintenance action virtual"
    workloop_init = symbols[b"__ZN10IOWorkLoop4initEv"][0]
    for offset, instruction in ((0xe6, "4c 8b b8 18 01 00 00"),
                                (0x112, "4c 89 fa"),
                                (0x11f, "4c 89 73 20")):
        assert kernel_read(workloop_init + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed workloop control-gate maintenance binding instruction"
    print("PASS Boot KC base workloop initialization binds maintenance action")
    allocator = symbols[b"__ZNK13IOCommandGate9MetaClass5allocEv"][0]
    assert hashlib.sha256(kernel_read(allocator, 0x80)).hexdigest() == \
        "73955144ac7f8379c9117469d11186a3c0f61d1d121055713e6fcd754c738c41", \
        "changed command-gate allocator window"
    # Next symbol includes a separate unnamed initializer after this window.
    lea = allocator + 0x4b
    instruction = kernel_read(lea, 7)
    assert instruction[:3] == bytes.fromhex("48 8d 0d") and \
        lea + 7 + struct.unpack_from("<i", instruction, 3)[0] == symbols[b"__ZTV13IOCommandGate"][0] + 16, \
        "command-gate allocator no longer installs base vtable"
    init = symbols[b"__ZN13IOCommandGate4initEP8OSObjectPFiS1_PvS2_S2_S2_E"][0]
    call = init + 9
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE"][0], \
        "changed command-gate inherited init target"
    print("PASS Boot KC command-gate allocator vtable and inherited init edge")
    base_init = symbols[b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE"][0]
    assert hashlib.sha256(kernel_read(base_init, 0x70)).hexdigest() == \
        "f975c99f5099be0529c344faf80ba56970164feafab786803119f95ba64e0441", \
        "changed inherited event-source owner/action init"
    assert kernel_read(base_init + 0x12, 4) == bytes.fromhex("48 89 5f 18"), "changed event-source owner store"
    assert kernel_read(base_init + 0x1c, 6) == bytes.fromhex("ff 90 40 01 00 00"), "changed event-source action setter virtual"
    print("PASS Boot KC inherited owner storage and effective command action setter")
    # gMetaClass is runtime-initialized zero storage in this file. Resolve the
    # initializer's symbolic reference, not a fictitious on-disk object vptr.
    load = workloop_init + 0xed
    instruction = kernel_read(load, 7)
    metaclass = symbols[b"__ZN13IOCommandGate10gMetaClassE"][0]
    assert instruction[:3] == bytes.fromhex("48 8b 05") and \
        load + 7 + struct.unpack_from("<i", instruction, 3)[0] == metaclass, \
        "changed workloop control-gate metaclass reference"
    assert kernel_read(metaclass, 8) == b"\0" * 8, "changed on-disk metaclass initialization state"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTVN13IOCommandGate9MetaClassE"][0] + 16 + 0x88, 8))[0] == allocator, \
        "changed command-gate metaclass allocator virtual"
    print("PASS Boot KC control-gate metaclass reference and allocator identity (runtime init pending)")
    # Unnamed metaclass initializer follows the allocator's separate window.
    initializer = allocator + 0x80
    assert hashlib.sha256(kernel_read(initializer, 0x70)).hexdigest() == \
        "2d431a9f495d3d9cd94bd4af3c6e6840f7cc0f6b45063ddda97fe1a057f64da2", \
        "changed command-gate metaclass initializer window"
    lea = initializer + 0x51
    instruction = kernel_read(lea, 7)
    assert instruction[:3] == bytes.fromhex("48 8d 05") and \
        lea + 7 + struct.unpack_from("<i", instruction, 3)[0] == symbols[b"__ZTVN13IOCommandGate9MetaClassE"][0] + 16, \
        "changed metaclass initializer vtable address"
    write = initializer + 0x58
    instruction = kernel_read(write, 7)
    assert instruction[:3] == bytes.fromhex("48 89 05") and \
        write + 7 + struct.unpack_from("<i", instruction, 3)[0] == metaclass, \
        "changed metaclass initializer vptr store"
    print("PASS Boot KC metaclass initializer writes the declared allocator vtable")
    stub = 0x10138
    assert system[stub:stub + 6] == bytes.fromhex("ff 25 62 40 01 00"), "changed accelerator workloop factory stub"
    pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
    raw = struct.unpack_from("<Q", system, pointer)[0]
    assert (raw >> 30) & 3 == 0 and raw >> 63 == 0, "unexpected workloop factory cache/auth"
    assert bases[0] + (raw & 0x3fffffff) == symbols[b"__ZN10IOWorkLoop8workLoopEv"][0], \
        "accelerator factory no longer resolves to IOWorkLoop::workLoop"
    call = 0x14ba02c7
    assert system[call] == 0xe8 and call + 5 + struct.unpack_from("<i", system, call + 1)[0] == stub, \
        "changed accelerator workloop construction call"
    assert system[call + 5:call + 12] == bytes.fromhex("49 89 86 f0 00 00 00"), "changed accelerator workloop store"
    print("PASS paired KC accelerator workloop factory import and field store (full lifecycle pending)")
    factory = symbols[b"__ZN10IOWorkLoop8workLoopEv"][0]
    assert hashlib.sha256(kernel_read(factory, 0xb0)).hexdigest() == \
        "f29215957d359f5d3c645b2752a35c06301834d22c2755190eb0e9e8f9709284", \
        "changed reviewed workloop factory body"
    lea = factory + 0x54
    instruction = kernel_read(lea, 7)
    assert instruction[:3] == bytes.fromhex("48 8d 05") and \
        lea + 7 + struct.unpack_from("<i", instruction, 3)[0] == workloop_table + 16, \
        "workloop factory no longer installs base vtable"
    assert kernel_read(factory + 0x8d, 6) == bytes.fromhex("ff 90 88 00 00 00"), "changed workloop init dispatch"
    assert kernel_read(factory + 0x9d, 5) == bytes.fromhex("ff 50 28 31 db"), "changed failed workloop init release/null result"
    print("PASS Boot KC concrete base workloop factory and failed-init cleanup")
    stub = 0x1051c
    assert system[stub:stub + 6] == bytes.fromhex("ff 25 ae 41 01 00"), "changed stop block import"
    pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
    raw = struct.unpack_from("<Q", system, pointer)[0]
    assert (raw >> 30) & 3 == 0 and raw >> 63 == 0, "unexpected stop block import cache/auth"
    block_api = symbols[b"__ZN10IOWorkLoop14runActionBlockEU13block_pointerFivE"][0]
    assert bases[0] + (raw & 0x3fffffff) == block_api, "changed stop block API identity"
    for start, length, digest in (
            (block_api, 0x40, "a38a67b32da3d99f873a5c26a175e11b20292dd190e22c189df7f5a815c15472"),
            (block_api + 0x40, 0x10, "353d84cf14acdfbc12e30defec09060ca37f6116ecc2aedb54919200c05b601b"),
            (0xffffff8000ac9be0, 0x60, "42ddad5ca2b1ca4851b501da78fc0a811a324aa38b798c6d591b2f08195b6100")):
        assert hashlib.sha256(kernel_read(start, length)).hexdigest() == digest, "changed gated block execution body"
    assert struct.unpack("<Q", kernel_read(workloop_table + 16 + 0x1a0, 8))[0] == 0xffffff8000ac9be0, \
        "changed base workloop synchronous action virtual"
    print("PASS paired KC stop block API and synchronous base workloop action body")
    detach = symbols[b"__ZN18IOTimerEventSource11setWorkLoopEP10IOWorkLoop"][0]
    assert kernel_read(detach + 0x15, 6) == bytes.fromhex("ff 90 58 01 00 00"), "changed timer detach disable dispatch"
    assert kernel_read(detach + 0x1e, 4) == bytes.fromhex("48 89 5f 30"), "changed timer workloop-pointer store"
    disable = symbols[b"__ZN18IOTimerEventSource7disableEv"][0]
    for offset, name in ((0x1e, b"_thread_call_cancel"), (0x25, b"_thread_call_cancel_wait")):
        call = disable + offset
        instruction = kernel_read(call, 5)
        assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[name][0], \
            "changed timer disable cancel branch"
    print("PASS Boot KC timer detach disables before clearing workloop")
    wake = symbols[b"__ZN18IOTimerEventSource10wakeAtTimeEjyy"][0]
    assert hashlib.sha256(kernel_read(wake, 0x130)).hexdigest() == \
        "28931d5cf72ac20f040ffb5fed17e87ba6c3eacc876e81c3d3ac05f18992ca2c", \
        "changed reviewed timer scheduling/retention body"
    for offset, instruction in ((0x6a, "ff 50 20"), (0x74, "ff 50 20"),
                                (0xbf, "ff 50 28"), (0xc9, "ff 50 28")):
        assert kernel_read(wake + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed passive timer schedule retain/release instruction"
    print("PASS Boot KC passive timer schedule-time reference pairing body")
    cancel = symbols[b"_thread_call_cancel"][0]
    # The next named symbol includes an unnamed locked helper: do not merge
    # the public wrapper and helper into one alleged function body.
    for start, length, digest in (
            (cancel, 0x120, "1744ae5843301d3f9f35d6e75d790c12866f84823dbceb19bd8b0bad8b215913"),
            (cancel + 0x120, 0x1f0, "04ebbd1202258d02da097a28b70152d3307dc5317d8d8214bc78bde5cfcf702d")):
        assert hashlib.sha256(kernel_read(start, length)).hexdigest() == digest, \
            "changed reviewed non-waiting thread-call cancellation window"
    call = cancel + 0x7b
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == cancel + 0x120, \
        "changed public cancellation to locked-helper edge"
    print("PASS Boot KC cancellation wrapper and separate locked-helper windows (not a drain proof)")
    object_free = symbols[b"__ZN8OSObject4freeEv"][0]
    assert hashlib.sha256(kernel_read(object_free, 0x30)).hexdigest() == \
        "6b5b497597ec0a7094328a70abfd1f3c877fe7f1e07f32a017bc104382ef37cd", \
        "changed reviewed OSObject free wrapper window"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV8OSObject"][0] + 0xa0, 8))[0] == object_free, \
        "changed OSObject base free dispatch slot"
    assert kernel_read(object_free + 0x28, 3) == bytes.fromhex("ff 60 08"), \
        "changed OSObject free deleting-destructor dispatch"
    print("PASS Boot KC OSObject base free dispatch and deleting-destructor edge (callee review pending)")
    object_delete = symbols[b"__ZN8OSObjectdlEPvm"][0]
    assert hashlib.sha256(kernel_read(object_delete, 0x40)).hexdigest() == \
        "04b6869b3dc3784361e720a8287053f5f525011e12bbf9cd0b9fd8bcf8a609ae", \
        "changed reviewed OSObject sized delete wrapper"
    print("PASS Boot KC sized object-delete wrapper (allocator callee pending)")
    source_workloop = symbols[b"__ZNK13IOEventSource11getWorkLoopEv"][0]
    assert kernel_read(source_workloop, 0x10) == bytes.fromhex("55 48 89 e5 48 8b 47 30 5d c3 66 0f 1f 44 00 00"), \
        "changed reviewed event-source workloop getter body"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV18IOTimerEventSource"][0] + 16 + 0x168, 8))[0] == source_workloop, \
        "changed effective timer attached-workloop getter slot"
    print("PASS Boot KC timer effective attached-workloop getter (not callback slot)")
    cancel_wait = symbols[b"_thread_call_cancel_wait"][0]
    assert hashlib.sha256(kernel_read(cancel_wait, 0x3d0)).hexdigest() == \
        "ab79f907dfbfeb04b2723874f2299984cdc722577b7c745328f5d916f5c84d48", \
        "changed reviewed cancellation-wait body"
    for offset, instruction in ((0xd6, "40 f6 c6 20"),
                                (0xdf, "84 c0"),
                                (0x173, "4c 8b 73 70"),
                                (0x177, "4c 39 73 78"),
                                (0x1a6, "80 4b 42 02")):
        assert kernel_read(cancel_wait + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed cancellation-wait mode/result/snapshot/waiter branch"
    print("PASS Boot KC conditional cancellation-wait and fixed counter snapshot (resubmission not excluded)")
    invoke = 0xffffff8000ad01b0
    assert hashlib.sha256(kernel_read(invoke, 0x160)).hexdigest() == \
        "db682f5eff10d5a738d4dc74d880b75b3718960bd2716644dd9f7f0700ba11f9", \
        "changed reviewed timer action invocation helper window"
    call = timeout + 0xc1
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == invoke, \
        "changed passive timer invocation helper edge"
    assert kernel_read(timeout + 0xb0, 4) == bytes.fromhex("48 8b 4b 18"), \
        "changed callback owner argument load"
    assert kernel_read(invoke + 0x4a, 9) == bytes.fromhex("48 89 df 48 89 d6 41 ff d4"), \
        "changed direct timer action owner/source invocation"
    print("PASS Boot KC passive timer owner forwarding and action helper (owner lifetime not established)")
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
                       (0x10018, b"_IOLockUnlock"), (0x10c30, b"_assert_wait_deadline"),
                       (0x10366, b"_thread_block"), (0x100d8, b"_clock_interval_to_deadline")):
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
    # Complete reviewed inherited stop and its captured-owner block. These
    # hashes do not certify called virtuals or runActionBlock synchronization.
    for start, length, digest in (
            (0x14ba1a7c, 0x43f, "5a5b95178a1b0bd70d3699730f0fcadc69f4449232a74a1e44231ea94172820b"),
            (0x14ba1ebb, 0x2d7, "bfcad84d6dc88c187f2478166bc752a66dbba41d2d4a123a4d8be8d8653232b1")):
        assert hashlib.sha256(image[start:start + length]).hexdigest() == digest, "changed accelerator stop/block body"
    stop_block = image[0x14ba1ebb:0x14ba2192]
    assert stop_block.count(bytes.fromhex("ff 90 48 01 00 00")) == 12, "changed stop-block removal inventory"
    assert image[0x14ba1d7d:0x14ba1d8b] == bytes.fromhex("ff 50 28 49 c7 86 f0 00 00 00 00 00 00 00"), \
        "changed stop workloop release/clear sequence"
    print("PASS complete inherited accelerator stop/block and twelve source removal calls")
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
                                    EVENT_INIT, EVENT_COPY, EVENT_FINISH_UNLOCKED, EVENT_HARDWARE_ERROR,
                                    EVENT_DISABLE_STAMP_LOCKED, EVENT_ENABLE_STAMP, EVENT_DISABLE_STAMP}}
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
                       (0x140, EVENT_INIT), (0x1b0, EVENT_COPY), (0x178, EVENT_FINISH_UNLOCKED),
                       (0x240, EVENT_ENABLE_STAMP), (0x248, EVENT_DISABLE_STAMP)):
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
    assert hashlib.sha256(read(address_of(EVENT_WAIT), 0x314)).hexdigest() == \
        "66a79a6eeeae4a30fc91586b1e09f54b702ed2c86501af4e4d0e7bcfe7ddb1f9", "changed full waitForStamp"
    assert hashlib.sha256(read(address_of(EVENT_DISABLE_STAMP_LOCKED), 0x48)).hexdigest() == \
        "f1d296bd9cb53f41b56e43d5ad536695b5b4d83ef4ffa99cbd56a31837b021ea", "changed timeout waiter cleanup"
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
