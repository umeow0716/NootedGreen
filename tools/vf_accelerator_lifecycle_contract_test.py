#!/usr/bin/env python3
"""Prove that the VF engine wrapper preserves Apple's IOAccel lifecycle tail."""

import pathlib
import struct
import sys


ENABLE = "__ZN22IOGraphicsAccelerator217enableAcceleratorEv"
DISABLE = "__ZN22IOGraphicsAccelerator218disableAcceleratorEv"
START = "__ZN16IntelAccelerator19startGraphicsEngineEv"
STOP = "__ZN16IntelAccelerator18stopGraphicsEngineEv"
ACCELERATOR_START = "__ZN16IntelAccelerator5startEP9IOService"
BRIDGE_ENABLE = "__ZN17IGInterruptBridge6enableEv"
BRIDGE_DISABLE = "__ZN17IGInterruptBridge7disableEv"
BRIDGE_FILTER = "__ZN17IGInterruptBridge22interruptFilterHandlerEP28IOFilterInterruptEventSource"
BRIDGE_READ = "__ZN17IGInterruptBridge22readAndClearInterruptsER8IGBitSetILm46EE"
BRIDGE_ENABLE_INTERRUPTS = "__ZN17IGInterruptBridge16enableInterruptsEv"
BRIDGE_DISABLE_INTERRUPTS = "__ZN17IGInterruptBridge17disableInterruptsEv"
BRIDGE_READ_RCS = "__ZN17IGInterruptBridge25readAndClearRCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_CCS = "__ZN17IGInterruptBridge25readAndClearCCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_BCS = "__ZN17IGInterruptBridge25readAndClearBCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_GUC = "__ZN17IGInterruptBridge25readAndClearGuCInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_VCS = "__ZN17IGInterruptBridge25readAndClearVCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_VECS = "__ZN17IGInterruptBridge26readAndClearVECSInterruptsER8IGBitSetILm46EEj"
EVENT_INIT = "__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent"
SCHEDULER_ENABLE = "__ZN12IGScheduler416enableInterruptsEv"
SCHEDULER_DISABLE = "__ZN12IGScheduler417disableInterruptsEv"
SCHEDULER_ERROR_ENABLE = "__ZN12IGScheduler421enableErrorInterruptsEv"
SCHEDULER_ERROR_DISABLE = "__ZN12IGScheduler422disableErrorInterruptsEv"
STREAMER_ERROR_ENABLE = "__ZN26IGHardwareCommandStreamer420enableErrorInterruptEv"
STREAMER_ERROR_DISABLE = "__ZN26IGHardwareCommandStreamer421disableErrorInterruptEv"
PCI_CONFIGURE_INTERRUPTS = "__ZN11IOPCIDevice19configureInterruptsEjjjj"
SCHEDULER_INIT_FIRMWARE = "__ZN11IGScheduler12initFirmwareEv"
SCHEDULER4_VTABLE = "__ZTV12IGScheduler4"
SCHEDULER4_INIT = "__ZN12IGScheduler419initWithAcceleratorEP22IOGraphicsAccelerator2"
SCHEDULER4_LOAD_FIRMWARE = "__ZN12IGScheduler412loadFirmwareEv"
COMMAND_STREAMER_FACTORY = "__ZN26IGHardwareCommandStreamer423hardwareCommandStreamerEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler410IGHwCsType"
COMMAND_STREAMER_INIT = "__ZN26IGHardwareCommandStreamer44initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler410IGHwCsType"
COMMAND_STREAMER_REGISTER = "__ZN26IGHardwareCommandStreamer421registerForInterruptsEv"
REQUEST_ENABLE_CALLBACK = "__ZN17IGInterruptBridge21requestEnableCallbackEP8OSObjectPFvS1_zE"
GUC_INIT_INTERRUPTS = "__ZN13IGHardwareGuC14initInterruptsEv"
GUC_REGISTER_INTERRUPTS = "__ZN13IGHardwareGuC21registerForInterruptsEv"
BRIDGE_REGISTER_TYPE = "__ZN17IGInterruptBridge24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_"
GUC_WITH_OPTIONS = "__ZN13IGHardwareGuC11withOptionsEP16IntelAccelerator"
GUC_INIT_WITH_OPTIONS = "__ZN13IGHardwareGuC15initWithOptionsEP16IntelAccelerator"
GUC_INIT_WORK_HISTORY = "__ZN13IGHardwareGuC19initWorkItemHistoryEj"
GUC_INIT_DOORBELLS = "__ZN13IGHardwareGuC13initDoorbellsEv"
CTB_WITH_OPTIONS = "__ZN21IGHardwareGuCCTBuffer11withOptionsEP22IOGraphicsAccelerator2"
CTB_INIT = "__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2"
GUC_LOAD_BINARY = "__ZN13IGHardwareGuC13loadGuCBinaryEv"
GUC_REGISTER_CTB = "__ZN13IGHardwareGuC31registerCommandTransportBuffersEv"
GUC_MMIO_ACTION = "__ZN13IGHardwareGuC19mmioHostToGuCActionEPKjjiPj"
CREATE_UK_CONTEXT = "__ZN13IGHardwareGuC15createUkContextEy25UK_GEN11_CONTEXT_PRIORITY"
MAPPED_GET_MEMORY = "__ZNK14IGMappedBuffer9getMemoryEv"
SYS_MEMORY_PHYSICAL = "__ZN16IGAccelSysMemory18getPhysicalSegmentEyPy"
BLIT3D_BOUNDS_START = "__ZN23IGHardwareBlit2DContext10initializeEv"
BLIT3D_BOUNDS_END = "__ZN21IGAccelDisplayMachine9MetaClassC1Ev"
BLIT3D_GLOBAL_INIT = "__GLOBAL__sub_I_IGHardwareContext.cpp"
BLIT3D_SCRATCH_ANCHOR = bytes.fromhex(
    "48 8d 05 19 31 03 00 48 8b 00 48 89 05 df d6 0c 00")
# Error 0x215 clears the native start result and jumps directly to the final
# result/stack-check block, bypassing Tahoe's common virtual-stop cleanup.
DPSM_START_FAILURE_ANCHOR = bytes.fromhex(
    "be 15 02 00 00 45 31 f6 e9 41 ff ff ff")


def macho_inventory(path):
    image = pathlib.Path(path).read_bytes()
    header = struct.unpack_from("<8I", image)
    if header[0] != 0xFEEDFACF:
        raise AssertionError(f"{path}: not a little-endian Mach-O 64 image")

    symtab = None
    dysymtab = None
    offset = 32
    for _ in range(header[4]):
        command, size = struct.unpack_from("<2I", image, offset)
        if command == 0x2:
            symtab = struct.unpack_from("<6I", image, offset)[2:]
        elif command == 0xB:
            dysymtab = struct.unpack_from("<20I", image, offset)
        offset += size
    if not symtab or not dysymtab:
        raise AssertionError(f"{path}: missing Mach-O symbol/relocation tables")

    symbol_offset, symbol_count, string_offset, _ = symtab
    names = []
    values = []
    for index in range(symbol_count):
        string_index, _, _, _, value = struct.unpack_from(
            "<IBBHQ", image, symbol_offset + index * 16)
        if string_index:
            end = image.index(0, string_offset + string_index)
            name = image[string_offset + string_index:end].decode()
        else:
            name = ""
        names.append(name)
        values.append(value)

    wanted = {ENABLE, DISABLE}
    wanted_indexes = {index: name for index, name in enumerate(names)
                      if name in wanted}
    relocations = {}
    external_offset, external_count = dysymtab[16], dysymtab[17]
    for index in range(external_count):
        address, bits = struct.unpack_from(
            "<iI", image, external_offset + index * 8)
        symbol_index = bits & 0xFFFFFF
        if symbol_index in wanted_indexes:
            name = wanted_indexes[symbol_index]
            if name in relocations:
                raise AssertionError(f"{path}: duplicate {name} relocation")
            # X86_64_RELOC_BRANCH, external, PC-relative, 32-bit displacement.
            if ((bits >> 24) & 1, (bits >> 25) & 3,
                    (bits >> 27) & 1, (bits >> 28) & 0xF) != (1, 2, 1, 2):
                raise AssertionError(f"{path}: incompatible {name} relocation")
            relocations[name] = address

    def value(name):
        matches = [values[i] for i, candidate in enumerate(names)
                   if candidate == name and values[i]]
        if len(matches) != 1:
            raise AssertionError(f"{path}: expected one defined {name}")
        return matches[0]

    def next_symbol(address):
        following = sorted(candidate for candidate in values
                           if candidate > address)
        if not following:
            raise AssertionError(f"{path}: no symbol after {address:#x}")
        return following[0]

    def direct_branches(owner, target):
        owner_start = value(owner)
        owner_end = next_symbol(owner_start)
        target_start = value(target)
        calls = []
        for candidate in range(owner_start, owner_end - 4):
            if image[candidate] not in (0xE8, 0xE9):
                continue
            displacement = struct.unpack_from("<i", image, candidate + 1)[0]
            if candidate + 5 + displacement == target_start:
                calls.append(candidate)
        return calls

    accelerator_start = value(ACCELERATOR_START)
    accelerator_start_end = next_symbol(accelerator_start)
    dpsm_failures = []
    cursor = 0
    while True:
        cursor = image.find(DPSM_START_FAILURE_ANCHOR, cursor)
        if cursor < 0:
            break
        dpsm_failures.append(cursor)
        cursor += 1
    if len(dpsm_failures) != 1 or not (
            accelerator_start < dpsm_failures[0] < accelerator_start_end):
        raise AssertionError(
            f"{path}: native post-engine 0x215 failure edge changed")

    # initFirmware reaches the Gen11 scheduler implementation through virtual
    # slot 0x220.  Keep the complete retained native bootstrap chain explicit:
    # every hardware-facing GuC descendant below must remain intercepted by the
    # VF routes checked in source_contract().
    scheduler4_vtable = value(SCHEDULER4_VTABLE)
    load_firmware_slot = struct.unpack_from(
        "<Q", image, scheduler4_vtable + 16 + 0x220)[0]
    if load_firmware_slot != value(SCHEDULER4_LOAD_FIRMWARE):
        raise AssertionError(
            f"{path}: scheduler-4 firmware virtual slot changed")

    retained_edges = (
        (SCHEDULER4_INIT, COMMAND_STREAMER_FACTORY),
        (COMMAND_STREAMER_FACTORY, COMMAND_STREAMER_INIT),
        (COMMAND_STREAMER_INIT, REQUEST_ENABLE_CALLBACK),
        (COMMAND_STREAMER_REGISTER, BRIDGE_REGISTER_TYPE),
        (SCHEDULER4_LOAD_FIRMWARE, GUC_WITH_OPTIONS),
        (GUC_WITH_OPTIONS, GUC_INIT_WITH_OPTIONS),
        (GUC_INIT_WITH_OPTIONS, GUC_INIT_WORK_HISTORY),
        (GUC_INIT_WITH_OPTIONS, GUC_INIT_DOORBELLS),
        (GUC_INIT_WITH_OPTIONS, CTB_WITH_OPTIONS),
        (GUC_INIT_WITH_OPTIONS, GUC_INIT_INTERRUPTS),
        (GUC_INIT_WITH_OPTIONS, GUC_LOAD_BINARY),
        (GUC_INIT_WITH_OPTIONS, GUC_REGISTER_CTB),
        (GUC_INIT_WITH_OPTIONS, CREATE_UK_CONTEXT),
        (CTB_WITH_OPTIONS, CTB_INIT),
        (GUC_INIT_INTERRUPTS, REQUEST_ENABLE_CALLBACK),
        (GUC_REGISTER_INTERRUPTS, BRIDGE_REGISTER_TYPE),
        (GUC_REGISTER_CTB, GUC_MMIO_ACTION),
    )
    for owner, target in retained_edges:
        if not direct_branches(owner, target):
            raise AssertionError(
                f"{path}: retained native bootstrap edge {owner} -> {target} changed")

    # The runtime patch must search across the private global constructor.
    # Its production bounds deliberately use exported symbols because the
    # binary contains many identically named __GLOBAL__D_a local symbols.
    blit3d_start = value(BLIT3D_BOUNDS_START)
    blit3d_end = value(BLIT3D_BOUNDS_END)
    blit3d_global_init = value(BLIT3D_GLOBAL_INIT)
    blit3d_anchors = []
    cursor = 0
    while True:
        cursor = image.find(BLIT3D_SCRATCH_ANCHOR, cursor)
        if cursor < 0:
            break
        blit3d_anchors.append(cursor)
        cursor += 1
    if len(blit3d_anchors) != 1:
        raise AssertionError(
            f"{path}: expected one Blit3D scratch constructor anchor")
    blit3d_anchor = blit3d_anchors[0]
    if not (blit3d_start < blit3d_global_init < blit3d_anchor < blit3d_end):
        raise AssertionError(
            f"{path}: exported Blit3D patch bounds do not enclose constructor anchor")
    if blit3d_end - blit3d_start > 0x400:
        raise AssertionError(f"{path}: Blit3D patch bounds exceed runtime limit")

    for lifecycle, owner, bridge in (
            (ENABLE, START, BRIDGE_ENABLE),
            (DISABLE, STOP, BRIDGE_DISABLE)):
        relocation = relocations.get(lifecycle)
        owner_start = value(owner)
        if relocation is None or not owner_start < relocation < next_symbol(owner_start):
            raise AssertionError(
                f"{path}: {lifecycle} is not called by {owner}")
        opcode = relocation - 1
        if image[opcode] != 0xE8:
            raise AssertionError(f"{path}: {lifecycle} is not a direct call")

        bridge_target = value(bridge)
        preceding = []
        for candidate in range(max(owner_start, opcode - 32), opcode):
            if image[candidate] != 0xE8 or candidate + 5 > len(image):
                continue
            displacement = struct.unpack_from("<i", image, candidate + 1)[0]
            if candidate + 5 + displacement == bridge_target:
                preceding.append(candidate)
        if not preceding:
            raise AssertionError(
                f"{path}: {bridge} does not precede {lifecycle}")

    for scheduler, scheduler_error, streamer in (
            (SCHEDULER_ENABLE, SCHEDULER_ERROR_ENABLE, STREAMER_ERROR_ENABLE),
            (SCHEDULER_DISABLE, SCHEDULER_ERROR_DISABLE, STREAMER_ERROR_DISABLE)):
        if not direct_branches(scheduler, scheduler_error):
            raise AssertionError(
                f"{path}: {scheduler} no longer dispatches {scheduler_error}")
        if not direct_branches(scheduler_error, streamer):
            raise AssertionError(
                f"{path}: {scheduler_error} no longer owns the physical {streamer} call")

    # TGL/ADL/RPL VFs use this native Gen11 virtual-interrupt register block.
    # Treat every aligned 0x190000-range displacement in the live bridge bodies
    # as MMIO and prove that it remains inside i915's VF allowlist.
    vf_ranges = (
        (0x190010, 0x190010), (0x190018, 0x19001C),
        (0x190030, 0x190048), (0x190060, 0x190064),
        (0x190070, 0x190074), (0x190090, 0x190090),
        (0x1900A0, 0x1900A0), (0x1900A8, 0x1900AC),
        (0x1900B0, 0x1900B4), (0x1900D0, 0x1900D4),
        (0x1900E8, 0x1900EC), (0x1900F0, 0x1900F4),
        (0x190100, 0x190100),
    )

    def mmio_offsets(name):
        start = value(name)
        body = image[start:next_symbol(start)]
        offsets = set()
        for cursor in range(len(body) - 3):
            candidate = struct.unpack_from("<I", body, cursor)[0]
            if 0x190000 <= candidate <= 0x190400 and candidate % 4 == 0:
                offsets.add(candidate)
        return offsets

    reader_helpers = (
        BRIDGE_READ_RCS, BRIDGE_READ_CCS, BRIDGE_READ_BCS,
        BRIDGE_READ_GUC, BRIDGE_READ_VCS, BRIDGE_READ_VECS,
    )
    bridge_offsets = {}
    for name in (BRIDGE_ENABLE, BRIDGE_DISABLE, BRIDGE_FILTER, BRIDGE_READ,
                 BRIDGE_ENABLE_INTERRUPTS, BRIDGE_DISABLE_INTERRUPTS,
                 *reader_helpers):
        offsets = mmio_offsets(name)
        bridge_offsets[name] = offsets
        for offset in offsets:
            if not any(first <= offset <= last for first, last in vf_ranges):
                raise AssertionError(
                    f"{path}: native VF interrupt body uses non-allowlisted MMIO {offset:#x}")
    if bridge_offsets[BRIDGE_ENABLE] != {0x190010}:
        raise AssertionError(f"{path}: bridge enable master-register contract changed")
    if bridge_offsets[BRIDGE_DISABLE] != {0x190010}:
        raise AssertionError(f"{path}: bridge disable master-register contract changed")
    if bridge_offsets[BRIDGE_FILTER] != {0x190010}:
        raise AssertionError(f"{path}: bridge filter master-register contract changed")
    required_reader = {0x190010, 0x190018, 0x19001C}
    if bridge_offsets[BRIDGE_READ] != required_reader:
        raise AssertionError(f"{path}: native virtual-MMIO reader inventory changed")
    for helper in reader_helpers:
        if not direct_branches(BRIDGE_READ, helper):
            raise AssertionError(f"{path}: native reader no longer calls {helper}")
    for helper in reader_helpers[:4]:
        if bridge_offsets[helper] != {0x190060, 0x190070}:
            raise AssertionError(f"{path}: bank-0 reader inventory changed for {helper}")
    for helper in reader_helpers[4:]:
        if bridge_offsets[helper] != {0x190064, 0x190074}:
            raise AssertionError(f"{path}: bank-1 reader inventory changed for {helper}")
    required_enable = {
        0x190030, 0x190034, 0x190038, 0x19003C, 0x190040, 0x190044,
        0x190048, 0x190060, 0x190064, 0x190070, 0x190074,
    }
    if not required_enable <= bridge_offsets[BRIDGE_ENABLE_INTERRUPTS]:
        raise AssertionError(f"{path}: native virtual-MMIO enable inventory changed")
    required_disable = {0x190030, 0x190034, 0x190038, 0x19003C, 0x190040, 0x190044}
    if not required_disable <= bridge_offsets[BRIDGE_DISABLE_INTERRUPTS]:
        raise AssertionError(f"{path}: native virtual-MMIO disable inventory changed")

    # enable() tests +0x8a8 at entry and jumps past its callback-list walk when
    # it is already true. requestEnableCallback() merely appends to +0x8b8 and
    # does not compensate for that one-shot boundary. GuC init registers only
    # after creating its software event source, proving the VF wrapper must run
    # a post-enable registration immediately.
    enable_start = value(BRIDGE_ENABLE)
    enable_body = image[enable_start:next_symbol(enable_start)]
    if not enable_body.startswith(b"\x55\x48\x89\xe5\x41\x56\x53\x80\xbf\xa8\x08\x00\x00\x00"):
        raise AssertionError(f"{path}: bridge enabled-state entry guard changed")
    request_start = value(REQUEST_ENABLE_CALLBACK)
    request_body = image[request_start:next_symbol(request_start)]
    if b"\x49\x89\x9f\xb8\x08\x00\x00" not in request_body:
        raise AssertionError(f"{path}: callback request no longer appends to +0x8b8")
    if not direct_branches(GUC_INIT_INTERRUPTS, REQUEST_ENABLE_CALLBACK):
        raise AssertionError(f"{path}: GuC init no longer registers through the bridge request")

    # createUkContext gets the private IGAccelMemory owner and invokes its
    # two-argument virtual getPhysicalSegment slot. The concrete system-memory
    # method supplies MemoryManager+0x88 mapper options to the owned IOMD. This
    # is not IOMemoryDescriptor's public three-argument virtual ABI.
    if not direct_branches(CREATE_UK_CONTEXT, MAPPED_GET_MEMORY):
        raise AssertionError(f"{path}: createUkContext no longer obtains IGAccelMemory")
    create_start = value(CREATE_UK_CONTEXT)
    create_body = image[create_start:next_symbol(create_start)]
    if create_body.count(b"\xff\x91\x58\x01\x00\x00") != 1:
        raise AssertionError(
            f"{path}: createUkContext physical-segment virtual ABI changed")
    physical_start = value(SYS_MEMORY_PHYSICAL)
    physical_body = image[physical_start:next_symbol(physical_start)]
    for sequence in (
            b"\x48\x8b\x47\x28",              # accelerator at +0x28
            b"\x48\x8b\xbf\xd0\x00\x00\x00",  # owned IOMD at +0xd0
            b"\x8b\x88\x88\x00\x00\x00",      # mapper options at +0x88
            b"\xff\x90\x38\x01\x00\x00"):       # IOMD segment method +0x138
        if sequence not in physical_body:
            raise AssertionError(
                f"{path}: IGAccelSysMemory mapper-aware segment ABI changed")

    print(f"PASS: native bridge/IOAccel lifecycle order in {path}")


def function_body(source, signature):
    cursor = 0
    while True:
        start = source.index(signature, cursor)
        opening = source.find("{", start + len(signature))
        declaration = source.find(";", start + len(signature))
        if opening >= 0 and (declaration < 0 or opening < declaration):
            break
        cursor = start + len(signature)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated {signature}")


def source_contract(path):
    source = pathlib.Path(path).read_text()
    required = (
        "com.apple.iokit.IOAcceleratorFamily2",
        "com.apple.iokit.IOPCIFamily",
        ENABLE,
        DISABLE,
        EVENT_INIT,
        "this->vfInterruptBridgeEnable = irqEnable",
        "this->vfInterruptBridgeDisable = irqDisable",
        SCHEDULER_ENABLE,
        SCHEDULER_DISABLE,
        PCI_CONFIGURE_INTERRUPTS,
        SCHEDULER_INIT_FIRMWARE,
        REQUEST_ENABLE_CALLBACK,
        SYS_MEMORY_PHYSICAL,
        BRIDGE_FILTER,
        BRIDGE_READ,
        BRIDGE_ENABLE_INTERRUPTS,
        BRIDGE_DISABLE_INTERRUPTS,
        SCHEDULER_ERROR_ENABLE,
        SCHEDULER_ERROR_DISABLE,
    )
    for token in required:
        if token not in source:
            raise AssertionError(f"{path}: missing lifecycle token {token}")

    for token in (
            "gVfUsesMemoryIrq = vfActive &&",
            "NGGpuCapabilities::hasIovMemoryIrq(gpuDevice)",
            "gVfMemIrqConfigured && gVfMemIrqRequested != 0",
            "gVfMmioIrqReady != 0"):
        if token not in source:
            raise AssertionError(f"{path}: missing interrupt-transport capability gate {token}")

    pci_resolution = function_body(
        source,
        "bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)")
    if "index == kextIOPCIFamily.loadIndex" not in pci_resolution:
        raise AssertionError(f"{path}: PCI allocator is not resolved from IOPCIFamily")
    if ("KernelPatcher::KernelID" in pci_resolution and
            pci_resolution.index("KernelPatcher::KernelID") <
            pci_resolution.index(PCI_CONFIGURE_INTERRUPTS)):
        raise AssertionError(f"{path}: IOPCIFamily symbol is incorrectly resolved from KernelID")
    for token in (
            GUC_LOAD_BINARY,
            GUC_INIT_DOORBELLS,
            CTB_INIT,
            REQUEST_ENABLE_CALLBACK,
            CREATE_UK_CONTEXT,
            GUC_MMIO_ACTION):
        if token not in pci_resolution:
            raise AssertionError(
                f"{path}: retained native bootstrap descendant is not VF-routed: {token}")

    memory_routes_start = pci_resolution.index(
        "KernelPatcher::RouteRequest memoryIrqRoutes[]")
    virtual_routes_start = pci_resolution.index(
        "KernelPatcher::RouteRequest virtualMmioIrqRoutes[]", memory_routes_start)
    virtual_routes_end = pci_resolution.index(
        '"Failed to isolate VF physical engine error interrupts"', virtual_routes_start)
    memory_routes = pci_resolution[memory_routes_start:virtual_routes_start]
    virtual_routes = pci_resolution[virtual_routes_start:virtual_routes_end]
    for token in (BRIDGE_FILTER, BRIDGE_READ, BRIDGE_ENABLE_INTERRUPTS,
                  BRIDGE_DISABLE_INTERRUPTS, SCHEDULER_ENABLE, SCHEDULER_DISABLE):
        if token not in memory_routes:
            raise AssertionError(f"{path}: memory-IRQ route set is missing {token}")
        if token in virtual_routes:
            raise AssertionError(f"{path}: virtual-MMIO route wrongly replaces {token}")
    for token in (SCHEDULER_ERROR_ENABLE, SCHEDULER_ERROR_DISABLE,
                  "vfSuppressPhysicalErrorInterrupts"):
        if token not in virtual_routes:
            raise AssertionError(f"{path}: virtual-MMIO isolation is missing {token}")

    master_patch = pci_resolution.index("static const uint8_t masterDisable[]")
    memory_gate = pci_resolution.rfind("if (gVfUsesMemoryIrq)", 0, master_patch)
    if memory_gate < 0 or not memory_routes_start > master_patch > memory_gate:
        raise AssertionError(f"{path}: GFX_MSTR_IRQ patch is not memory-IRQ-only")

    configure_memirq = function_body(source, "bool vfConfigureMemIrq()")
    if "!gVfUsesMemoryIrq" not in configure_memirq:
        raise AssertionError(f"{path}: unsupported devices can configure memory IRQ")
    consume_memirq = function_body(source, "uint64_t vfConsumeMemoryInterrupts()")
    if "!gVfUsesMemoryIrq" not in consume_memirq:
        raise AssertionError(f"{path}: unsupported devices can consume memory IRQ")
    configure_ctb = function_body(
        source, "bool vfConfigureModernCtb(bool g2h, uint32_t appleDescriptorAddress)")
    if "g2h && gVfUsesMemoryIrq && !vfConfigureMemIrq()" not in configure_ctb:
        raise AssertionError(f"{path}: CTB registration does not capability-gate memory IRQ")
    attach = function_body(source, "bool Gen11::vfAttachContextDesc(void *that, const uint32_t *descriptor)")
    if "!gVfUsesMemoryIrq ||" not in attach or "vfPrepareContextMemoryIrq(" not in attach:
        raise AssertionError(f"{path}: LRCA memory-IRQ mutation is not capability-gated")

    drain = function_body(
        source, "bool Gen11::vfDrainGuCToHost(void *that, IOInterruptEventSource *source,")
    if "vfCtbConsumerReady(!synchronousPoll)" not in drain:
        raise AssertionError(
            f"{path}: synchronous CTB teardown cannot outlive hardware IRQ disable")

    scratch_start = pci_resolution.index(
        "mach_vm_address_t blit3dBoundsStart")
    scratch_end = pci_resolution.index(
        'SYSLOG("ngreen", "V250:', scratch_start)
    scratch_contract = pci_resolution[scratch_start:scratch_end]
    normalized_scratch_contract = "".join(scratch_contract.split())
    for token in (
            '{"' + BLIT3D_BOUNDS_START + '",blit3dBoundsStart}',
            '{"' + BLIT3D_BOUNDS_END + '",blit3dBoundsEnd}',
            "blit3dBoundsEnd<=blit3dBoundsStart",
            "blit3dBoundsEnd-blit3dBoundsStart>0x400",
            "patcher,blit3dBoundsStart,blit3dBoundsEnd-blit3dBoundsStart"):
        if token not in normalized_scratch_contract:
            raise AssertionError(
                f"{path}: incomplete production Blit3D patch-bound contract: {token}")
    for forbidden in (
            "__ZN25IGHardwareExtendedContext9MetaClassD0Ev",
            "__ZN23IGHardwareBlit3DContext9MetaClassD0Ev"):
        if forbidden in scratch_contract:
            raise AssertionError(
                f"{path}: Blit3D anchor is bounded by a symbol before its address")

    start = function_body(source, "bool Gen11::startGraphicsEngine(void *that)")
    bridge = start.index("callback->vfInterruptBridgeEnable)(")
    firmware = start.index("callback->vfSchedulerInitFirmware)(scheduler)")
    ready = start.index("if (!vfNativeGpuWorkReady())")
    accelerator = start.index("callback->ioGraphicsEnableAccelerator)(that)")
    if not bridge < firmware < ready < accelerator:
        raise AssertionError(
            f"{path}: VF MSI/firmware/transport/accelerator lifecycle order is reversed")
    failure = start.index("VF scheduler firmware initialization failed")
    disable = start.index("callback->vfInterruptBridgeDisable)(", failure)
    fault = start.index("VF scheduler firmware initialization failure", failure)
    if not failure < disable < fault:
        raise AssertionError(
            f"{path}: firmware failure does not close the early VF MSI consumer")
    if "IGMemoryManager::initCache" not in start or "must omit it" not in start:
        raise AssertionError(
            f"{path}: physical VF cache initialization is not explicitly excluded")
    if start.index("vfInterruptBridgeEnable") > accelerator:
        raise AssertionError(f"{path}: VF start lifecycle order is reversed")
    if start.count("initEvent(eventMachine") != 2:
        raise AssertionError(f"{path}: VF start does not initialize both native events")
    stop = function_body(source, "bool Gen11::stopGraphicsEngine(void *that)")
    if stop.index("vfInterruptBridgeDisable") > stop.index(
            "ioGraphicsDisableAccelerator"):
        raise AssertionError(f"{path}: VF stop lifecycle order is reversed")

    accelerator_start = function_body(source, "bool Gen11::start(void *that, void *provider)")
    configure = accelerator_start.index("callback->ioPciConfigureInterrupts)(")
    native_start = accelerator_start.index("FunctionCast(start, callback->ostart)")
    if configure > native_start:
        raise AssertionError(f"{path}: VF MSI is configured after native start")
    if "pciDevice, kIOInterruptTypePCIMessaged, 1, 1, 0" not in accelerator_start:
        raise AssertionError(f"{path}: VF MSI request is not exactly one required vector")
    native_result = accelerator_start.index(
        "const auto result = FunctionCast(start, callback->ostart)(that, provider)")
    firmware_live = accelerator_start.index(
        "gVfSchedulerFirmwareReady && !gVfDeviceStopping", native_result)
    rollback = accelerator_start.index("acceleratorStop(that, nullptr)", firmware_live)
    quiesced = accelerator_start.index("!gVfDmaQuiesced", rollback)
    start_fault = accelerator_start.index(
        "native accelerator start failed after VF bootstrap", quiesced)
    if not native_result < firmware_live < rollback < quiesced < start_fault:
        raise AssertionError(
            f"{path}: live post-engine native-start failure is not DMA-quiesced before fault")

    late_callback = function_body(
        source, "void Gen11::vfRequestEnableCallback(void *that, OSObject *requestor,")
    enabled = late_callback.index("getMember<uint8_t>(that, 0x8A8)")
    immediate = late_callback.index("action(requestor)")
    original = late_callback.index("callback->oVfRequestEnableCallback)(")
    if not enabled < immediate < original:
        raise AssertionError(
            f"{path}: post-enable callback is not serviced before native queue fallback")

    quiesce = function_body(source, "bool vfQuiesceDeviceForShutdown(void *guc)")
    for token in (
            "if (!gVfCtbEverEnabled)",
            "vfDirectContextTableUnowned()",
            "OSCompareAndSwap(0, 1, &gVfDmaQuiesced)",
            "OSCompareAndSwap(0, 1, &gVfContextShutdownComplete)"):
        if token not in quiesce:
            raise AssertionError(f"{path}: incomplete pre-CTB rollback proof: {token}")

    unowned = function_body(source, "bool vfDirectContextTableUnowned()")
    for token in (
            "entry.state != kVfGucContextEmpty",
            "entry.contextBacking",
            "entry.refCount",
            "entry.enablePending",
            "entry.disablePending"):
        if token not in unowned:
            raise AssertionError(
                f"{path}: incomplete direct-context ownership proof: {token}")

    failed_bootstrap = function_body(
        source, "bool vfRollbackFailedPostCtbBootstrap(void *guc)")
    for token in (
            "guc != gVfHardwareGuc",
            "gVfCtbEverEnabled",
            "gVfSchedulerFirmwareReady",
            "gVfProtocolFault",
            "gVfMmioPoisoned",
            "vfCloseContextOperationGateAndWait(guc)",
            "vfDirectContextTableUnowned()",
            "vfCloseIrqCallbackGateAndWait(guc)",
            "kGucActionHost2GucControlCtb, 0",
            "NGVfMmioResponse::noData(reply[0])",
            "OSCompareAndSwap(0, 1, &gVfDmaQuiesced)"):
        if token not in failed_bootstrap:
            raise AssertionError(
                f"{path}: incomplete failed post-CTB rollback proof: {token}")

    mmio_start = source.index(
        "bool Gen11::vfMmioHostToGuCAction(void *that, const uint32_t *request,")
    configured = source.index("vfConfigureModernCtb(", mmio_start)
    consume = source.index("vfConsumeMemoryInterrupts()", configured)
    drain = source.index("pollVfGuCToHost(that)", consume)
    response = source.index("NGVfLegacyCtb::responseStatus(ok)", drain)
    if not configured < consume < drain < response:
        raise AssertionError(
            f"{path}: CTB enable-boundary drain is not ordered before success")
    for signature in (
            "bool vfInvalidateTLBSync(void *guc)",
            "bool vfWaitForContextState(void *guc, uint16_t gucId, VfGucContextState wanted)",
            "bool vfWaitForContextTransition(void *guc, uint16_t gucId, uint32_t lrcaPage,"):
        if "pollVfGuCToHost(guc)" not in function_body(source, signature):
            raise AssertionError(f"{path}: synchronous GuC wait lacks bounded G2H polling: {signature}")

    bootstrap_abort = function_body(
        source, "static void vfAbortSchedulerBootstrap(const char *reason)")
    if ("gVfSubmissionStopped" not in bootstrap_abort or
            "vfMarkProtocolFault" in bootstrap_abort or
            "gVfProtocolFault" in bootstrap_abort):
        raise AssertionError(
            f"{path}: local bootstrap abort poisons its required teardown transport")
    create = function_body(
        source, "uint32_t Gen11::vfCreateUkContext(void *that, uint64_t owner, int priority)")
    for token in (
            "using GetMemory = void *(*)(void *)",
            "using GetPhysicalSegment = uint64_t (*)(void *, uint64_t, uint64_t *)",
            'metaCast("IGAccelSysMemory")',
            "callback->vfAccelSysMemoryGetPhysicalSegment",
            "vfAbortSchedulerBootstrap(\"invalid VF proxy process physical segment\")"):
        if token not in create:
            raise AssertionError(f"{path}: missing exact proxy DMA ABI/rollback token {token}")
    for forbidden in ("memory->getPhysicalSegment", "kIOMemoryMapperNone"):
        if forbidden in create:
            raise AssertionError(
                f"{path}: proxy DMA lookup reintroduced wrong ABI {forbidden}")
    workqueue = function_body(
        source, "bool Gen11::vfWorkQueueInit(void *that, void *accelerator, uint32_t id, void *process)")
    if workqueue.index("vfAbortSchedulerBootstrap(") > workqueue.index(
            "NGWorkQueue::unwindFailedInit"):
        raise AssertionError(
            f"{path}: failed workqueue releases mappings before stopping producers")
    unmap = function_body(
        source, "void Gen11::IGHardwareGlobalPageTableUnmapRange(void *that,")
    if "gVfCtbEverEnabled && gVfProtocolFault && !gVfDmaQuiesced" not in unmap:
        raise AssertionError(
            f"{path}: pre-CTB protocol failure cannot unwind DMA-free mappings")
    print(f"PASS: VF wrapper preserves native bridge/IOAccel lifecycle in {path}")


def main():
    if len(sys.argv) != 4:
        raise SystemExit(f"usage: {sys.argv[0]} kern_gen11.cpp TGL-production TGL-debug")
    source_contract(sys.argv[1])
    macho_inventory(sys.argv[2])
    macho_inventory(sys.argv[3])


if __name__ == "__main__":
    main()
