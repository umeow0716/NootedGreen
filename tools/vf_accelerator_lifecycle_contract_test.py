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
ACCELERATOR_STOP = "__ZN16IntelAccelerator4stopEP9IOService"
EVENT_FINISH_ALL = "__ZN19IGAccelEventMachine15finishAllStampsEj"
TRACE_DISABLE = "__ZN25IGAccelTraceStreamManager17disableCollectionE27TraceStreamCollectionChange"
TRACE_SHUTDOWN = "__ZN25IGAccelTraceStreamManager8shutdownEv"
UNREGISTER_SYSCTL = "__ZN16IntelAccelerator16unregisterSysctlEv"
INIT_HARDWARE_STATUS_MEMORY = "__ZN16IntelAccelerator28initHardwareStatusPageMemoryEv"
INIT_HARDWARE_STATUS_REGISTERS = "__ZN16IntelAccelerator31initHardwareStatusPageRegistersEv"
INIT_MODE_REGISTERS = "__ZN16IntelAccelerator17initModeRegistersEv"
SAFE_FORCE_WAKE = "__ZN16IntelAccelerator13SafeForceWakeEbj"
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
SCHEDULER4_SYSTEM_SLEEP = "__ZN12IGScheduler415systemWillSleepEv"
SCHEDULER4_SYSTEM_WAKE = "__ZN12IGScheduler413systemDidWakeEv"
BRIDGE_SYSTEM_SLEEP = "__ZN17IGInterruptBridge15systemWillSleepEv"
BRIDGE_SYSTEM_WAKE = "__ZN17IGInterruptBridge13systemDidWakeEv"
SET_POWER_STATE = "__ZN16IntelAccelerator13setPowerStateEmP9IOService"
SCHEDULER4_VTABLE = "__ZTV12IGScheduler4"
SCHEDULER5_VTABLE = "__ZTV12IGScheduler5"
SCHEDULER4_INIT = "__ZN12IGScheduler419initWithAcceleratorEP22IOGraphicsAccelerator2"
SCHEDULER4_LOAD_FIRMWARE = "__ZN12IGScheduler412loadFirmwareEv"
SCHEDULER4_IS_GPU_IDLE = "__ZNK12IGScheduler49isGpuIdleEv"
SCHEDULER5_IS_GPU_IDLE = "__ZNK12IGScheduler59isGpuIdleEv"
SCHEDULER_BASE_INIT = "__ZN11IGScheduler15initWithOptionsEjyP22IOGraphicsAccelerator2"
COMMAND_STREAMER_FACTORY = "__ZN26IGHardwareCommandStreamer423hardwareCommandStreamerEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler410IGHwCsType"
COMMAND_STREAMER_INIT = "__ZN26IGHardwareCommandStreamer44initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler410IGHwCsType"
COMMAND_STREAMER_REGISTER = "__ZN26IGHardwareCommandStreamer421registerForInterruptsEv"
REQUEST_ENABLE_CALLBACK = "__ZN17IGInterruptBridge21requestEnableCallbackEP8OSObjectPFvS1_zE"
GUC_INIT_INTERRUPTS = "__ZN13IGHardwareGuC14initInterruptsEv"
GUC_REGISTER_INTERRUPTS = "__ZN13IGHardwareGuC21registerForInterruptsEv"
BRIDGE_REGISTER_TYPE = "__ZN17IGInterruptBridge24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_"
GUC_WITH_OPTIONS = "__ZN13IGHardwareGuC11withOptionsEP16IntelAccelerator"
GUC_INIT_WITH_OPTIONS = "__ZN13IGHardwareGuC15initWithOptionsEP16IntelAccelerator"
GUC_FREE = "__ZN13IGHardwareGuC4freeEv"
GUC_INIT_SCHED_CONTROL = "__ZN13IGHardwareGuC16initSchedControlEv"
GUC_SETUP_CONTEXT_POOL = "__ZN13IGHardwareGuC16setupContextPoolEi"
GUC_SETUP_LOG_BUFFERS = "__ZN13IGHardwareGuC15setupLogBuffersEjiii"
GUC_SETUP_ADDITIONAL = "__ZN13IGHardwareGuC26setupAdditionalDataStructsEv"
GUC_INIT_WORK_HISTORY = "__ZN13IGHardwareGuC19initWorkItemHistoryEj"
GUC_INIT_DOORBELLS = "__ZN13IGHardwareGuC13initDoorbellsEv"
GUC_READ_DOORBELLS = "__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv"
CTB_WITH_OPTIONS = "__ZN21IGHardwareGuCCTBuffer11withOptionsEP22IOGraphicsAccelerator2"
CTB_INIT = "__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2"
CTB_FREE = "__ZN21IGHardwareGuCCTBuffer4freeEv"
SET_ASYNC_SLICE_COUNT = "__ZN16IntelAccelerator18setAsyncSliceCountE13IGSliceConfig"
DPSM_IDLE_TIMER = "__ZN16IntelAccelerator13dpsmIdleTimerEv"
INIT_LOCAL_CALLBACKS = "__ZN16IntelAccelerator24initLocalCallbackSupportEv"
ENABLE_COARSE_POWER_GATING = "__ZL24_enableCoarsePowerGatingv"
DPSM_NOTIFY = "__ZL11_dpsmNotifyPj"
LOCAL_SAFE_FORCE_WAKE = "__ZL14_SafeForceWakebj"
LOCAL_PAVP_CONTROL = "__ZL19_PAVPSessionControl27PAVPSessionControlCommand_tPv"
LOCAL_MEDIA_LOAD = "__ZL16_mediaKernelLoadb"
LOCAL_MEDIA_PREPARE = "__ZL19_mediaPrepareEncodePv"
LOCAL_CLIENT_NOTIFY = "__ZL13_clientNotify15IntelClientID_tb"
LOCAL_PM_NOTIFY = "__ZL9_pmNotifyjjPyPj"
LOCAL_GUC_WILL_LOAD = "__ZL17_accelWillLoadGuCyPv"
LOCAL_GUC_FAILED = "__ZL21_accelFailedToLoadGuCv"
LOCAL_GUC_DID_LOAD = "__ZL16_accelDidLoadGuCv"
GUC_LOAD_BINARY = "__ZN13IGHardwareGuC13loadGuCBinaryEv"
GUC_REGISTER_CTB = "__ZN13IGHardwareGuC31registerCommandTransportBuffersEv"
GUC_DEREGISTER_CTB = "__ZN13IGHardwareGuC33deregisterCommandTransportBuffersEv"
GUC_MMIO_ACTION = "__ZN13IGHardwareGuC19mmioHostToGuCActionEPKjjiPj"
CREATE_UK_CONTEXT = "__ZN13IGHardwareGuC15createUkContextEy25UK_GEN11_CONTEXT_PRIORITY"
MAPPED_WITH_OPTIONS = "__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj"
MAPPED_INIT = "__ZN20IGSharedMappedBuffer15initWithOptionsEP11IGAccelTaskmjj"
MAPPED_GET_MEMORY = "__ZNK14IGMappedBuffer9getMemoryEv"
SYS_MEMORY_PHYSICAL = "__ZN16IGAccelSysMemory18getPhysicalSegmentEyPy"
TRANSFER_OWNERSHIP = "__ZN16IntelAccelerator17transferOwnershipEPK20IGSharedMappedBufferi"
GLOBAL_MAP_RANGE = "__ZN25IGHardwareGlobalPageTable8mapRangeERK14IGAddressRangeyy"
GLOBAL_MAP_ROTATED = "__ZN25IGHardwareGlobalPageTable15mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory"
GLOBAL_UNMAP_RANGE = "__ZN25IGHardwareGlobalPageTable10unmapRangeERK14IGAddressRange"
GLOBAL_MAP_DUMMY = "__ZN25IGHardwareGlobalPageTable13mapRangeDummyERK14IGAddressRangey"
FENCE_ALLOCATE = "__ZN16IGFenceAllocator8allocateERK14IGAddressRangem19GFX3DSTATE_TILEMODE"
FENCE_INIT = "__ZN7IGFence15initWithOptionsEP16IGFenceAllocatormRK14IGAddressRangem19GFX3DSTATE_TILEMODE"
FENCE_FREE = "__ZN7IGFence4freeEv"
RESOURCE_ADD_APERTURE = "__ZN15IGAccelResource13addToApertureEv"
DISPLAY_ALLOC_SCANOUT = "__ZN18IGAccelDisplayPipe21allocateScanoutMemoryEP19IntelScaledModeDataj"
BLIT3D_BOUNDS_START = "__ZN23IGHardwareBlit2DContext10initializeEv"
BLIT3D_BOUNDS_END = "__ZN21IGAccelDisplayMachine9MetaClassC1Ev"
BLIT3D_GLOBAL_INIT = "__GLOBAL__sub_I_IGHardwareContext.cpp"
BLIT3D_SCRATCH_ANCHOR = bytes.fromhex(
    "48 8d 05 19 31 03 00 48 8b 00 48 89 05 df d6 0c 00")
# Error 0x215 clears the native start result and jumps directly to the final
# result/stack-check block, bypassing Tahoe's common virtual-stop cleanup.
DPSM_START_FAILURE_ANCHOR = bytes.fromhex(
    "be 15 02 00 00 45 31 f6 e9 41 ff ff ff")
ASYNC_SLICE_MMIO_ANCHOR = bytes.fromhex(
    "49 8b 86 40 12 00 00 89 98 04 a2 00 00")
HWS_ENGINE_MMIO_ANCHOR = bytes.fromhex(
    "49 8b 8e 40 12 00 00 42 89 04 21")
HWS_GLOBAL_MMIO_ANCHOR = bytes.fromhex(
    "49 8b 8e 40 12 00 00 89 81 80 80 01 00")
FENCE_INIT_MMIO_ANCHOR = bytes.fromhex(
    "48 8b 8f 40 12 00 00 89 1c 01 48 8b 45 c8 44 89 2c 01")
FENCE_FREE_MMIO_ANCHOR = bytes.fromhex(
    "48 8b 8f 40 12 00 00 89 1c 01 46 89 2c 31")
FENCE_RESOURCE_NULL_UNWIND = bytes.fromhex(
    "49 89 86 28 02 00 00 48 85 c0 74 60")
DPSM_SCHEDULER_IDLE_SLOT = bytes.fromhex(
    "48 8b 07 ff 90 60 01 00 00")
DPSM_NOTIFY_SLOT = bytes.fromhex(
    "48 8b 83 e8 0d 00 00 ff 50 38")
DPSM_COARSE_POWER_SLOT = bytes.fromhex(
    "49 8b 85 e8 0d 00 00 ff 10")
DPSM_NOTIFY_BODY = bytes.fromhex("55 48 89 e5 31 c0 5d c3")
VOID_NOOP_BODY = bytes.fromhex("55 48 89 e5 5d c3")
ZERO_NOOP_BODY = bytes.fromhex("55 48 89 e5 31 c0 5d c3")
UNSUPPORTED_NOOP_BODY = bytes.fromhex(
    "55 48 89 e5 b8 c7 02 00 e0 5d c3")


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

    # Native start installs a DPSM software timer after engine admission. Its
    # only scheduler decision must dispatch through the routed isGpuIdle slot;
    # the local callback-table endpoint is an exact no-op, not hardware PM.
    for vtable, idle in ((SCHEDULER4_VTABLE, SCHEDULER4_IS_GPU_IDLE),
                         (SCHEDULER5_VTABLE, SCHEDULER5_IS_GPU_IDLE)):
        idle_slot = struct.unpack_from(
            "<Q", image, value(vtable) + 16 + 0x160)[0]
        if idle_slot != value(idle):
            raise AssertionError(f"{path}: {vtable} idle virtual slot changed")
    scheduler4_vtable = value(SCHEDULER4_VTABLE)
    for slot, target in ((0x118, SCHEDULER4_SYSTEM_SLEEP),
                         (0x120, SCHEDULER4_SYSTEM_WAKE)):
        if struct.unpack_from(
                "<Q", image, scheduler4_vtable + 16 + slot)[0] != value(target):
            raise AssertionError(
                f"{path}: scheduler-4 power-state slot {slot:#x} changed")
    dpsm_start = value(DPSM_IDLE_TIMER)
    dpsm_end = next_symbol(dpsm_start)
    if image[dpsm_start:dpsm_end].count(DPSM_SCHEDULER_IDLE_SLOT) != 1 or \
            image[dpsm_start:dpsm_end].count(DPSM_NOTIFY_SLOT) != 1:
        raise AssertionError(f"{path}: DPSM timer dispatch contract changed")
    notify_start = value(DPSM_NOTIFY)
    notify_end = next_symbol(notify_start)
    if image[notify_start:notify_end] != DPSM_NOTIFY_BODY:
        raise AssertionError(f"{path}: DPSM notification is no longer an exact no-op")
    callback_start = value(INIT_LOCAL_CALLBACKS)
    callback_end = next_symbol(callback_start)
    callback_refs = []
    for candidate in range(callback_start, callback_end - 10):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 0d") or \
                image[candidate + 7:candidate + 11] != bytes.fromhex("48 89 48 38"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == notify_start:
            callback_refs.append(candidate)
    if len(callback_refs) != 1:
        raise AssertionError(f"{path}: local callback table no longer pins DPSM no-op")
    coarse_start = value(ENABLE_COARSE_POWER_GATING)
    coarse_end = next_symbol(coarse_start)
    if image[coarse_start:coarse_end] != VOID_NOOP_BODY:
        raise AssertionError(f"{path}: coarse-power callback is no longer an exact no-op")
    coarse_refs = []
    for candidate in range(callback_start, callback_end - 9):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 0d") or \
                image[candidate + 7:candidate + 10] != bytes.fromhex("48 89 08"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == coarse_start:
            coarse_refs.append(candidate)
    if len(coarse_refs) != 1 or \
            image[accelerator_start:accelerator_start_end].count(
                DPSM_COARSE_POWER_SLOT) != 1:
        raise AssertionError(f"{path}: native start coarse-power no-op dispatch changed")

    # Headless registration installs a local 0x60-byte callback table instead
    # of a framebuffer provider. Pin every initialized entry and its exact
    # software-only body, not only the two callbacks reached during start.
    local_callbacks = (
        (0x00, ENABLE_COARSE_POWER_GATING, VOID_NOOP_BODY),
        (0x08, LOCAL_SAFE_FORCE_WAKE, VOID_NOOP_BODY),
        (0x10, LOCAL_PAVP_CONTROL, VOID_NOOP_BODY),
        (0x20, LOCAL_MEDIA_LOAD, VOID_NOOP_BODY),
        (0x28, LOCAL_MEDIA_PREPARE, VOID_NOOP_BODY),
        (0x30, LOCAL_CLIENT_NOTIFY, VOID_NOOP_BODY),
        (0x38, DPSM_NOTIFY, ZERO_NOOP_BODY),
        (0x40, LOCAL_PM_NOTIFY, ZERO_NOOP_BODY),
        (0x48, LOCAL_GUC_WILL_LOAD, UNSUPPORTED_NOOP_BODY),
        (0x50, LOCAL_GUC_FAILED, VOID_NOOP_BODY),
        (0x58, LOCAL_GUC_DID_LOAD, VOID_NOOP_BODY),
    )
    for slot, name, expected_body in local_callbacks:
        target = value(name)
        if image[target:next_symbol(target)].rstrip(b"\x90") != expected_body:
            raise AssertionError(
                f"{path}: local callback {name} is no longer software-only")
        refs = []
        for candidate in range(callback_start, callback_end - 10):
            if image[candidate:candidate + 3] != bytes.fromhex("48 8d 0d"):
                continue
            displacement = struct.unpack_from("<i", image, candidate + 3)[0]
            if candidate + 7 + displacement != target:
                continue
            store = image[candidate + 7:candidate + 11]
            expected_store = (bytes.fromhex("48 89 08") if slot == 0 else
                              bytes((0x48, 0x89, 0x48, slot)))
            if store.startswith(expected_store):
                refs.append(candidate)
        if len(refs) != 1:
            raise AssertionError(
                f"{path}: local callback slot {slot:#x} no longer pins {name}")

    # Guest sleep/wake reaches the same routed engine boundaries. Scheduler
    # firmware initialization is explicitly idempotent: its +0x20 loaded byte
    # bypasses the +0x220 loadFirmware virtual call on every wake after the
    # first successful GuC/CTB construction. The bridge sleep/wake helpers
    # continue to converge on the already audited disable/enable methods.
    for target in (BRIDGE_SYSTEM_SLEEP, BRIDGE_SYSTEM_WAKE, START, STOP):
        if len(direct_branches(SET_POWER_STATE, target)) != 1:
            raise AssertionError(
                f"{path}: accelerator power-state edge to {target} changed")
    if len(direct_branches(BRIDGE_SYSTEM_SLEEP, BRIDGE_DISABLE)) != 1 or \
            len(direct_branches(BRIDGE_SYSTEM_WAKE, BRIDGE_ENABLE)) != 1:
        raise AssertionError(
            f"{path}: interrupt-bridge sleep/wake lifecycle changed")
    firmware_start = value(SCHEDULER_INIT_FIRMWARE)
    firmware_body = image[firmware_start:next_symbol(firmware_start)]
    firmware_steps = (
        bytes.fromhex("f6 47 0c 01"),
        bytes.fromhex("80 7f 20 00"),
        bytes.fromhex("ff 90 20 02 00 00"),
        bytes.fromhex("c6 43 20 01"),
    )
    positions = [firmware_body.find(step) for step in firmware_steps]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(
            f"{path}: scheduler firmware idempotence guard changed")

    # Accelerator feature +0x1190 bit 5 makes native start call this routine
    # before startGraphicsEngine. It writes raw MMIO 0xA204 under physical
    # force-wake, so source admission must reject that mode on a VF.
    async_calls = direct_branches(ACCELERATOR_START, SET_ASYNC_SLICE_COUNT)
    if len(async_calls) != 1:
        raise AssertionError(f"{path}: native async-slice start edge changed")
    async_call = async_calls[0]
    predicates = []
    cursor = accelerator_start
    while True:
        cursor = image.find(bytes.fromhex("41 f6 06 20 74"), cursor,
                            async_call)
        if cursor < 0:
            break
        skip = struct.unpack_from("<b", image, cursor + 5)[0]
        if cursor + 6 + skip == async_call + 5:
            predicates.append(cursor)
        cursor += 1
    if len(predicates) != 1:
        raise AssertionError(
            f"{path}: async-slice call is not gated by +0x1190 bit 5")
    async_start = value(SET_ASYNC_SLICE_COUNT)
    async_end = next_symbol(async_start)
    if image[async_start:async_end].count(ASYNC_SLICE_MMIO_ANCHOR) != 1:
        raise AssertionError(f"{path}: async-slice physical-MMIO anchor changed")
    engine_start_calls = direct_branches(ACCELERATOR_START, START)
    hws_init_calls = direct_branches(ACCELERATOR_START, INIT_HARDWARE_STATUS_MEMORY)
    if len(engine_start_calls) != 1 or len(hws_init_calls) != 1 or \
            len(direct_branches(INIT_HARDWARE_STATUS_MEMORY,
                                MAPPED_WITH_OPTIONS)) != 2:
        raise AssertionError(f"{path}: native start/HWS mapped-buffer graph changed")

    # The original engine-start body is intentionally never entered on a VF.
    # Pin representative physical descendants so that this safety boundary is
    # explicit: it acquires force-wake, initializes mode registers and writes
    # every engine/global HWS address through the accelerator's raw MMIO base.
    for target in (SAFE_FORCE_WAKE, INIT_MODE_REGISTERS,
                   INIT_HARDWARE_STATUS_REGISTERS):
        if not direct_branches(START, target):
            raise AssertionError(
                f"{path}: physical engine-start edge {START} -> {target} changed")
    hws_register_start = value(INIT_HARDWARE_STATUS_REGISTERS)
    hws_register_body = image[
        hws_register_start:next_symbol(hws_register_start)]
    if hws_register_body.count(HWS_ENGINE_MMIO_ANCHOR) != 1 or \
            hws_register_body.count(HWS_GLOBAL_MMIO_ANCHOR) != 1:
        raise AssertionError(
            f"{path}: physical HWS-register MMIO inventory changed")

    # Tiled/aperture resources reach the legacy fence allocator independently
    # of engine start. Its constructor and destructor each take physical
    # force-wake and write the 0x100000 fence-register bank through MMIO+0x1240.
    # A null allocator result is a native fail-closed boundary: both resource
    # and display callers check it and unwind without creating an IGFence whose
    # later free method would repeat the physical writes.
    if len(direct_branches(FENCE_ALLOCATE, FENCE_INIT)) != 1:
        raise AssertionError(f"{path}: fence allocator/init edge changed")
    if len(direct_branches(RESOURCE_ADD_APERTURE, FENCE_ALLOCATE)) != 1 or \
            len(direct_branches(DISPLAY_ALLOC_SCANOUT, FENCE_ALLOCATE)) != 1:
        raise AssertionError(f"{path}: legacy fence allocation callers changed")
    for owner, anchor in ((FENCE_INIT, FENCE_INIT_MMIO_ANCHOR),
                          (FENCE_FREE, FENCE_FREE_MMIO_ANCHOR)):
        body = image[value(owner):next_symbol(value(owner))]
        if body.count(anchor) != 1 or \
                len(direct_branches(owner, SAFE_FORCE_WAKE)) != 2:
            raise AssertionError(
                f"{path}: physical fence-register protocol changed in {owner}")
    resource_start = value(RESOURCE_ADD_APERTURE)
    resource_body = image[resource_start:next_symbol(resource_start)]
    if resource_body.count(FENCE_RESOURCE_NULL_UNWIND) != 1:
        raise AssertionError(
            f"{path}: aperture resource no longer unwinds a null fence")
    dpsm_refs = []
    for candidate in range(accelerator_start, accelerator_start_end - 6):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 35"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == dpsm_start:
            dpsm_refs.append(candidate)
    if len(dpsm_refs) != 1 or not engine_start_calls[0] < dpsm_refs[0]:
        raise AssertionError(f"{path}: DPSM timer is not the pinned post-engine callback")

    retained_edges = (
        (SCHEDULER4_INIT, SCHEDULER_BASE_INIT),
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
        (GUC_INIT_SCHED_CONTROL, GUC_SETUP_CONTEXT_POOL),
        (GUC_INIT_SCHED_CONTROL, GUC_SETUP_LOG_BUFFERS),
        (GUC_INIT_SCHED_CONTROL, GUC_SETUP_ADDITIONAL),
        (GUC_SETUP_CONTEXT_POOL, MAPPED_WITH_OPTIONS),
        (GUC_SETUP_CONTEXT_POOL, TRANSFER_OWNERSHIP),
        (GUC_SETUP_LOG_BUFFERS, MAPPED_WITH_OPTIONS),
        (GUC_SETUP_LOG_BUFFERS, TRANSFER_OWNERSHIP),
        (GUC_SETUP_ADDITIONAL, MAPPED_WITH_OPTIONS),
        (GUC_SETUP_ADDITIONAL, TRANSFER_OWNERSHIP),
        (GUC_INIT_WORK_HISTORY, MAPPED_WITH_OPTIONS),
        (GUC_INIT_DOORBELLS, GUC_READ_DOORBELLS),
        (MAPPED_WITH_OPTIONS, MAPPED_INIT),
        (CTB_WITH_OPTIONS, CTB_INIT),
        (CTB_INIT, MAPPED_WITH_OPTIONS),
        (CTB_INIT, TRANSFER_OWNERSHIP),
        (GUC_INIT_INTERRUPTS, REQUEST_ENABLE_CALLBACK),
        (GUC_REGISTER_INTERRUPTS, BRIDGE_REGISTER_TYPE),
        (GUC_REGISTER_CTB, GUC_MMIO_ACTION),
        (GUC_REGISTER_CTB, GUC_DEREGISTER_CTB),
        (GUC_DEREGISTER_CTB, GUC_MMIO_ACTION),
        (GUC_FREE, GUC_DEREGISTER_CTB),
        (GUC_FREE, TRANSFER_OWNERSHIP),
        (CTB_FREE, TRANSFER_OWNERSHIP),
    )
    for owner, target in retained_edges:
        if not direct_branches(owner, target):
            raise AssertionError(
                f"{path}: retained native bootstrap edge {owner} -> {target} changed")

    # The VF wrapper deliberately preserves native stop so Tahoe can finish its
    # software event lifecycle and release every scheduler-owned object.  Its
    # only engine boundary must remain the routed stopGraphicsEngine call, and
    # all trace/sysctl teardown must follow that DMA-quiescing boundary.
    finish_calls = direct_branches(ACCELERATOR_STOP, EVENT_FINISH_ALL)
    trace_disable_calls = direct_branches(ACCELERATOR_STOP, TRACE_DISABLE)
    engine_stop_calls = direct_branches(ACCELERATOR_STOP, STOP)
    trace_shutdown_calls = direct_branches(ACCELERATOR_STOP, TRACE_SHUTDOWN)
    unregister_calls = direct_branches(ACCELERATOR_STOP, UNREGISTER_SYSCTL)
    if (len(finish_calls) != 1 or len(trace_disable_calls) != 1 or
            len(engine_stop_calls) != 1 or len(trace_shutdown_calls) != 2 or
            len(unregister_calls) != 1):
        raise AssertionError(f"{path}: native accelerator-stop call inventory changed")
    engine_stop = engine_stop_calls[0]
    if not (finish_calls[0] < trace_disable_calls[0] < engine_stop and
            all(engine_stop < call for call in trace_shutdown_calls) and
            engine_stop < unregister_calls[0]):
        raise AssertionError(
            f"{path}: native accelerator stop no longer quiesces engine before teardown")
    accelerator_stop_end = next_symbol(value(ACCELERATOR_STOP))
    release_calls = []
    for pattern in (bytes.fromhex("ff 50 28"),
                    bytes.fromhex("ff 90 28 00 00 00")):
        cursor = value(ACCELERATOR_STOP)
        while True:
            cursor = image.find(pattern, cursor, accelerator_stop_end)
            if cursor < 0:
                break
            release_calls.append(cursor)
            cursor += 1
    if not release_calls or any(call < engine_stop for call in release_calls):
        raise AssertionError(
            f"{path}: native accelerator stop releases an object before VF DMA quiescence")

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
    normalized_pci_resolution = "".join(pci_resolution.split())
    if ('{"' + START + '",startGraphicsEngine},' not in
            normalized_pci_resolution):
        raise AssertionError(
            f"{path}: VF no longer replaces the complete physical engine-start body")
    for token in (
            GUC_INIT_SCHED_CONTROL,
            GUC_LOAD_BINARY,
            GUC_INIT_DOORBELLS,
            CTB_INIT,
            GUC_READ_DOORBELLS,
            REQUEST_ENABLE_CALLBACK,
            CREATE_UK_CONTEXT,
            GUC_MMIO_ACTION,
            MAPPED_WITH_OPTIONS,
            TRANSFER_OWNERSHIP,
            GLOBAL_MAP_RANGE,
            GLOBAL_MAP_ROTATED,
            GLOBAL_UNMAP_RANGE,
            GLOBAL_MAP_DUMMY):
        if token not in pci_resolution:
            raise AssertionError(
                f"{path}: retained native bootstrap descendant is not VF-routed: {token}")
    if FENCE_ALLOCATE not in pci_resolution:
        raise AssertionError(f"{path}: physical fence allocator is not VF-routed")
    if ('{"' + FENCE_ALLOCATE + '",vfRejectPhysicalFence},' not in
            normalized_pci_resolution):
        raise AssertionError(f"{path}: physical fence route mapping changed")
    fence_reject = function_body(
        source, "void *Gen11::vfRejectPhysicalFence(void *that,")
    if "return nullptr;" not in fence_reject:
        raise AssertionError(f"{path}: VF fence rejection no longer returns null")
    for forbidden in ("FunctionCast", "callback->", "0x1240", "SafeForceWake"):
        if forbidden in fence_reject:
            raise AssertionError(
                f"{path}: VF fence rejection re-enters hardware through {forbidden}")

    load_guc = function_body(source, "bool Gen11::loadGuCBinary(void *that)")
    for token in (
            "callback->orgInitSchedControl",
            "getMember<void *>(that, 0x50)",
            "getMember<void *>(that, 0x68)",
            "getMember<void *>(that, 0x60)",
            "getMember<void *>(that, 0x70)",
            "getMember<void *>(that, 0x78)",
            "getMember<void *>(that, 0x9E8)"):
        if token not in load_guc:
            raise AssertionError(
                f"{path}: native VF scheduler storage contract is incomplete: {token}")

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
    for forbidden in ("FunctionCast", "0x1240", "SafeForceWake",
                      "initModeRegisters", "initHardwareStatusPageRegisters"):
        if forbidden in start:
            raise AssertionError(
                f"{path}: VF engine-start re-enters physical state through {forbidden}")
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
    quiesce = stop.index("vfQuiesceDeviceForShutdown(gVfHardwareGuc)")
    bridge_disable = stop.index("vfInterruptBridgeDisable", quiesce)
    if not quiesce < bridge_disable:
        raise AssertionError(
            f"{path}: final VF DMA quiescence no longer precedes bridge disable")
    if stop.index("vfInterruptBridgeDisable") > stop.index(
            "ioGraphicsDisableAccelerator"):
        raise AssertionError(f"{path}: VF stop lifecycle order is reversed")

    accelerator_stop = function_body(
        source, "void Gen11::acceleratorStop(void *that, void *provider)")
    stopping = accelerator_stop.index(
        "OSCompareAndSwap(0, 1, &gVfDeviceStopping)")
    original_stop = accelerator_stop.index(
        "FunctionCast(acceleratorStop, callback->oAcceleratorStop)(that, provider)")
    if not stopping < original_stop:
        raise AssertionError(
            f"{path}: native stop begins before the VF device-stopping boundary")

    accelerator_start = function_body(source, "bool Gen11::start(void *that, void *provider)")
    legacy_reject = accelerator_start.index("kVfLegacyPageOwnershipFlag")
    ggtt_bootstrap = accelerator_start.index("vfBootstrapDirectGgtt()")
    configure = accelerator_start.index("callback->ioPciConfigureInterrupts)(")
    native_start = accelerator_start.index("FunctionCast(start, callback->ostart)")
    if not legacy_reject < ggtt_bootstrap < configure < native_start:
        raise AssertionError(
            f"{path}: VF legacy-MMIO rejection/GGTT/MSI order changed before native start")
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

    for wrapper in ("bool Gen11::wrapIGScheduler5IsGpuIdle(const void *that)",
                    "bool Gen11::wrapIGScheduler4IsGpuIdle(const void *that)"):
        if "return vfKnownIdleSnapshot();" not in function_body(source, wrapper):
            raise AssertionError(
                f"{path}: DPSM idle route no longer uses VF context state")

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
