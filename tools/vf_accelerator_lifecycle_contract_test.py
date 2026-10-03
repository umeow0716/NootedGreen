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
EVENT_TIMEOUT = "__ZN19IGAccelEventMachine12eventTimeoutEi"
SCHEDULER4_CHECK_PROGRESS = "__ZN12IGScheduler416checkForProgressE10IGHwCsType"
SCHEDULER4_PAUSE = "__ZN12IGScheduler45pauseEv"
SCHEDULER4_RESUME = "__ZN12IGScheduler46resumeEv"
SCHEDULER4_PREPARE_RESET = "__ZN12IGScheduler415prepareGPUResetE10IGHwCsType"
SCHEDULER4_GET_ACTIVE = "__ZN12IGScheduler417getActiveContextsE10IGHwCsTypePP17IGHardwareContextPyS3_"
GUC_PAUSE = "__ZN13IGHardwareGuC14pauseSchedulerEv"
GUC_RESUME = "__ZN13IGHardwareGuC15resumeSchedulerEv"
SCHEDULER_HALT = "__ZN11IGScheduler19haltCommandStreamerE10IGHwCsType"
SCHEDULER_RESUME = "__ZN11IGScheduler21resumeCommandStreamerE10IGHwCsType"
ENCODE_DEBUG = "__ZN16IntelAccelerator15encodeDebugInfoE15IGTimeoutReason"
GATHER_GUC = "__ZN16IntelAccelerator16gatherKeyGuCDataEv"
GATHER_RING = "__ZN16IntelAccelerator17gatherKeyRingDataE10IGHwCsType"
GET_INSTDONE = "__ZN16IntelAccelerator16getInstDoneSliceE10IGHwCsType"
RING_DO_HANG = "__ZN20IGHardwareRingBuffer14doHangAnalysisEv"
RING_DUMP_HANG = "__ZN20IGHardwareRingBuffer16dumpHangAnalysisEv"
RING_DEBUG_ENGINE = "__ZN20IGHardwareRingBuffer19debugGraphicsEngineEv"
FIFO_DIAGNOSIS = "__ZN18IGAccelFIFOChannel26getHardwareDiagnosisReportEPj"
RING_DUMP_STATUS = "__ZN20IGHardwareRingBuffer14dumpRingStatusEv"
RING_DUMP_REGISTERS = "__ZN20IGHardwareRingBuffer13dumpRegistersEv"
RING_DUMP_REGISTERS_RCS = "__ZN20IGHardwareRingBuffer16dumpRegistersRCSEv"
RING_VTABLE = "__ZTV20IGHardwareRingBuffer"
FIFO_VTABLE = "__ZTV18IGAccelFIFOChannel"
RING_RESET_GRAPHICS = "__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext"
FIFO_RESET_REPLAY = "__ZN18IGAccelFIFOChannel22resetHardwareAndReplayEv"
FIFO_SUBMIT_STAMP = "__ZN18IGAccelFIFOChannel18submitStampCommandEv"
RING_SLEEP_STAMP = "__ZN20IGHardwareRingBuffer13sleepForStampEPjjj"
RING_WRITE_STAMP = "__ZN20IGHardwareRingBuffer10writeStampEjb"
RING_MAIN_WRITE_STAMP = "__ZN24IGHardwareRingBufferMain10writeStampEjb"
RING_COMPUTE_WRITE_STAMP = "__ZN27IGHardwareRingBufferCompute10writeStampEjb"
RING_COMMIT_STAMP = "__ZN20IGHardwareRingBuffer18commitStampCommandEjb"
RING_MAIN_COMMIT_STAMP = "__ZN24IGHardwareRingBufferMain18commitStampCommandEjb"
RING_COMPUTE_COMMIT_STAMP = "__ZN27IGHardwareRingBufferCompute18commitStampCommandEjb"
RING_WRITE_BUFFER = "__ZN20IGHardwareRingBuffer11writeBufferEPjj"
RING_GTT_WRITE_MODE = "__ZN20IGHardwareRingBuffer15getGTTWriteModeEv"
TASK_SCRATCH_GPU_ADDRESS = "__ZNK11IGAccelTask27getScratchGPUVirtualAddressEv"
RING_WAIT_SPACE = "__ZN20IGHardwareRingBuffer12waitForSpaceEj"
RING_WAIT_TIMEOUT = "__ZN20IGHardwareRingBuffer11waitTimeoutEU13block_pointerFbvE"
SCHEDULER4_INIT_PRIVATE = "__ZN12IGScheduler421initSharedPrivateDataEP17IGHardwareContext"
SCHEDULER4_CLEANUP_PRIVATE = "__ZN12IGScheduler424cleanupSharedPrivateDataEP17IGHardwareContext"
GUC_ATTACH_DESC = "__ZN13IGHardwareGuC29AttachContextDescToGucContextERK21SGfxContextDescriptor"
GUC_DETACH_DESC = "__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor"
GUC_INVALIDATE_TLB = "__ZN13IGHardwareGuC13invalidateTLBEv"
CONTEXT_FREE = "__ZN17IGHardwareContext4freeEv"
CONTEXT_INIT = "__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh"
CONTEXT_RING_GPU_ADDRESS = "__ZN17IGHardwareContext25initRingGPUVirtualAddressEv"
TASK_STAMP_GPU_ADDRESS = "__ZNK11IGAccelTask25getStampGPUVirtualAddressEv"
TASK_STAMPS = "__ZNK11IGAccelTask9getStampsEv"
TASK_INIT_STAMPS = "__ZN11IGAccelTask24initStampAndScratchPagesEv"
TASK_FREE = "__ZN11IGAccelTask4freeEv"
TASK_RELEASE_STAMPS = "__ZN11IGAccelTask27releaseStampAndScratchPagesEv"
TASK_RELEASE = "__ZNK11IGAccelTask7releaseEv"
CONTEXT_NOTIFY_COMPLETE = "__ZN17IGHardwareContext14notifyCompleteEP12IOAccelEvent"
FIFO_NOTIFY_COMPLETE = "__ZN18IGAccelFIFOChannel14notifyCompleteEP12IOAccelEvent"
RING_NOTIFY_COMPLETE = "__ZN20IGHardwareRingBuffer14notifyCompleteEP12IOAccelEvent"
EVENT_MACHINE_VTABLE = "__ZTV19IGAccelEventMachine"
EVENT_MERGE = "__ZN24IOAccelEventMachineFast210mergeEventEP12IOAccelEventS1_"
GC_OBJECT_RELEASE = "__ZNK10IGGCObject7releaseEv"
GC_OBJECT_RELEASE_UNCHECKED = "__ZNK10IGGCObject14releaseNoCheckEv"
GC_ADD = "__ZN18IGGarbageCollector3addEP14IGGCQueueEntry"
GC_COLLECT = "__ZN18IGGarbageCollector7collectEv"
GC_FORCE_COLLECT = "__ZN18IGGarbageCollector12forceCollectEv"
GC_DRAIN = "__ZN18IGGarbageCollector5drainEv"
CONTEXT_CHECK = "__ZNK17IGHardwareContext5checkEv"
CONTEXT_VTABLE = "__ZTV17IGHardwareContext"
SCHEDULER4_CONTEXT_IDLE = "__ZNK12IGScheduler413isContextIdleEPK17IGHardwareContext"
GUC_KMD_CONTEXT_IDLE = "__ZN13IGHardwareGuC16isKmdContextIdleERK21SGfxContextDescriptor"
SHARED_BUFFER_CPU_ADDRESS = "__ZNK20IGSharedMappedBuffer17getVirtualAddressEv"
MAPPED_BUFFER_GPU_ADDRESS = "__ZNK14IGMappedBuffer20getGPUVirtualAddressEv"
MEMORY_MAP_VTABLE = "__ZTV16IGAccelMemoryMap"
MEMORY_MAP_GPU_ADDRESS = "__ZN16IOAccelMemoryMap20getGPUVirtualAddressEv"
MAPPED_BUFFER_INIT = "__ZN14IGMappedBuffer15initWithOptionsEP11IGAccelTaskmbj"
SYS_MEMORY_FACTORY = "__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jy"
PREPARE_MAPPING = "__ZN22IOGraphicsAccelerator220freeToPrepareMappingEP16IOAccelMemoryMap"
POPULATE_ACCEL_CONFIG = "__ZN16IntelAccelerator19populateAccelConfigEP13IOAccelConfig"
MAPPED_BUFFER_MAPPING_OPTIONS = "__ZNK14IGMappedBuffer17getMappingOptionsEv"
MAPPED_BUFFER_FREE = "__ZN14IGMappedBuffer4freeEv"
SHARED_BUFFER_FREE = "__ZN20IGSharedMappedBuffer4freeEv"
SHARED_BUFFER_UNLOCK = "__ZN20IGSharedMappedBuffer18unlockForCPUAccessEv"
MEMORY_MAP_COMPLETE = "__ZN16IOAccelMemoryMap8completeEv"
MEMORY_MAP_FINISH_EVENT = "__ZN16IOAccelMemoryMap11finishEventEv"
SYS_MEMORY_UNLOCK = "__ZN16IOAccelSysMemory18unlockForCPUAccessEP4task"
SHARED_BUFFER_CLONE = "__ZN20IGSharedMappedBuffer11cloneInTaskEP11IGAccelTask"
SHARED_BUFFER_FACTORY = "__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj"
SCHEDULER4_BIND = "__ZN12IGScheduler44bindE10IGHwCsTypeih"
SCHEDULER4_UNBIND = "__ZN12IGScheduler46unbindEP17IGHardwareContext"
GET_DEFAULT_RESET = "__ZN16IntelAccelerator20getDefaultResetValueEj"
TRACE_DISABLE = "__ZN25IGAccelTraceStreamManager17disableCollectionE27TraceStreamCollectionChange"
TRACE_SHUTDOWN = "__ZN25IGAccelTraceStreamManager8shutdownEv"
UNREGISTER_SYSCTL = "__ZN16IntelAccelerator16unregisterSysctlEv"
INIT_HARDWARE_STATUS_MEMORY = "__ZN16IntelAccelerator28initHardwareStatusPageMemoryEv"
INIT_HARDWARE_STATUS_REGISTERS = "__ZN16IntelAccelerator31initHardwareStatusPageRegistersEv"
INIT_MODE_REGISTERS = "__ZN16IntelAccelerator17initModeRegistersEv"
SAFE_FORCE_WAKE = "__ZN16IntelAccelerator13SafeForceWakeEbj"
SAFE_FORCE_WAKE_BOOL = "__ZN16IntelAccelerator13SafeForceWakeEb"
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
ACCELERATOR_VTABLE = "__ZTV16IntelAccelerator"
NEW_MEMORY_MANAGER = "__ZN16IntelAccelerator16newMemoryManagerEv"
TGL_MEMORY_METACLASS = "__ZN21IntelTGLMemoryManager9metaClassE"
TGL_MEMORY_VTABLE = "__ZTV21IntelTGLMemoryManager"
MEMORY_MANAGER_INIT = "__ZN15IGMemoryManager4initEP16IntelAcceleratorRK18IntelSharedMemInfoRK14_stolenMemInfo"
TGL_DETECT_EDRAM = "__ZN21IntelTGLMemoryManager11detectEDRAMEv"
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
MEMORY_EDRAM_ZERO_STATE = bytes.fromhex("66 41 89 46 20")
EDRAM_CAPABILITY_MMIO_READ = bytes.fromhex(
    "49 8b 46 18 8b 98 10 00 12 00")
EDRAM_CONTROL_MMIO_WRITES = bytes.fromhex(
    "49 8b 46 18 bb 00 00 00 b0 89 98 28 81 13 00 "
    "c7 80 24 81 13 00 12 10 59 80")
EDRAM_CONFIRM_MMIO_READ = bytes.fromhex(
    "49 8b 46 18 8b 80 10 59 14 00")
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

    # Tahoe's stamp-timeout state machine keeps useful software recovery, but
    # its non-virtual helpers assume ownership of physical engine registers.
    # Prove every Scheduler4 virtual used by eventTimeout first: progress is a
    # constant true result, pause/resume reach exact GuC no-ops, active-context
    # discovery clears every output, and reset preparation is also a no-op.
    # Consequently the classified VF normally reaches encodeDebugInfo directly;
    # the halt/resume routes remain a defensive boundary if that topology ever
    # changes without the pinned vtable and bytecode checks failing first.
    for slot, target in (
            (0x150, SCHEDULER4_CHECK_PROGRESS),
            (0x178, SCHEDULER4_PAUSE),
            (0x180, SCHEDULER4_RESUME),
            (0x188, SCHEDULER4_PREPARE_RESET),
            (0x198, SCHEDULER4_GET_ACTIVE)):
        if struct.unpack_from(
                "<Q", image, scheduler4_vtable + 16 + slot)[0] != value(target):
            raise AssertionError(
                f"{path}: timeout scheduler-4 slot {slot:#x} changed")
    check_progress = image[value(SCHEDULER4_CHECK_PROGRESS):
                           next_symbol(value(SCHEDULER4_CHECK_PROGRESS))]
    if check_progress != bytes.fromhex("55 48 89 e5 b0 01 5d c3"):
        raise AssertionError(f"{path}: scheduler-4 progress result changed")
    if len(direct_branches(SCHEDULER4_PAUSE, GUC_PAUSE)) != 1 or \
            len(direct_branches(SCHEDULER4_RESUME, GUC_RESUME)) != 1:
        raise AssertionError(f"{path}: scheduler-4 pause/resume graph changed")
    for target in (GUC_PAUSE, GUC_RESUME, SCHEDULER4_PREPARE_RESET):
        start = value(target)
        if image[start:next_symbol(start)] != VOID_NOOP_BODY:
            raise AssertionError(f"{path}: native timeout no-op changed: {target}")
    active_start = value(SCHEDULER4_GET_ACTIVE)
    active_body = image[active_start:next_symbol(active_start)]
    if active_body != bytes.fromhex(
            "55 48 89 e5 31 c0 48 89 02 48 89 01 49 89 00 5d c3 90"):
        raise AssertionError(
            f"{path}: scheduler-4 active-context zero result changed")

    timeout_start = value(EVENT_TIMEOUT)
    timeout_body = image[timeout_start:next_symbol(timeout_start)]
    for slot_bytes, count, label in (
            (bytes.fromhex("ff 90 50 01 00 00"), 1, "checkForProgress"),
            (bytes.fromhex("ff 90 78 01 00 00"), 1, "pause"),
            (bytes.fromhex("ff 90 98 01 00 00"), 1, "getActiveContexts"),
            (bytes.fromhex("ff 90 80 01 00 00"), 2, "resume"),
            (bytes.fromhex("ff 90 88 01 00 00"), 1, "prepareGPUReset")):
        if timeout_body.count(slot_bytes) != count:
            raise AssertionError(
                f"{path}: eventTimeout {label} dispatch inventory changed")
    debug_calls = direct_branches(EVENT_TIMEOUT, ENCODE_DEBUG)
    halt_calls = direct_branches(EVENT_TIMEOUT, SCHEDULER_HALT)
    resume_calls = direct_branches(EVENT_TIMEOUT, SCHEDULER_RESUME)
    if len(debug_calls) != 3 or len(halt_calls) != 1 or len(resume_calls) != 2 or \
            not (debug_calls[0] < halt_calls[0] < resume_calls[0] <
                 resume_calls[1] < debug_calls[1] < debug_calls[2]):
        raise AssertionError(
            f"{path}: eventTimeout physical-helper inventory/order changed")

    # encodeDebugInfo is not a read-only formatter. It force-wakes and captures
    # eight legacy GuC scratch registers, then gathers each active engine. Ring
    # gathering calls getInstDoneSlice four times; that helper writes the global
    # 0xFDC selector for six slices before restoring it. Pin the complete direct
    # graph and the destructive register inventory behind the VF route.
    if len(direct_branches(ENCODE_DEBUG, GATHER_GUC)) != 1 or \
            len(direct_branches(ENCODE_DEBUG, GATHER_RING)) != 1:
        raise AssertionError(f"{path}: debug-capture call graph changed")
    gather_guc_start = value(GATHER_GUC)
    gather_guc_body = image[gather_guc_start:next_symbol(gather_guc_start)]
    if len(direct_branches(GATHER_GUC, SAFE_FORCE_WAKE)) != 2 or any(
            gather_guc_body.count(struct.pack("<I", register)) != 1
            for register in range(0xC184, 0xC1A1, 4)):
        raise AssertionError(f"{path}: GuC scratch debug capture changed")
    if len(direct_branches(GATHER_RING, SAFE_FORCE_WAKE)) != 5 or \
            len(direct_branches(GATHER_RING, GET_INSTDONE)) != 4:
        raise AssertionError(f"{path}: ring debug-capture graph changed")
    instdone_start = value(GET_INSTDONE)
    instdone_body = image[instdone_start:next_symbol(instdone_start)]
    if instdone_body.count(struct.pack("<I", 0xFDC)) != 4 or any(
            instdone_body.count(struct.pack("<I", register)) != 1
            for register in (0x7100, 0xE160, 0xE164)) or \
            instdone_body.count(bytes.fromhex("48 81 ff 00 00 00 06")) != 1:
        raise AssertionError(
            f"{path}: destructive INSTDONE selector protocol changed")

    for owner, store in (
            (SCHEDULER_HALT, bytes.fromhex("42 c7 04 20 01 00 01 00")),
            (SCHEDULER_RESUME, bytes.fromhex("42 c7 04 20 00 00 01 00"))):
        start = value(owner)
        body = image[start:next_symbol(start)]
        if len(direct_branches(owner, SAFE_FORCE_WAKE)) != 2 or \
                body.count(bytes.fromhex("48 8b 87 40 12 00 00")) != 2 or \
                body.count(store) != 1 or \
                body.count(bytes.fromhex("bb 11 27 00 00")) != 1:
            raise AssertionError(
                f"{path}: physical timeout halt/resume protocol changed: {owner}")

    # IOAccel can request the same physical capture independently of the event
    # timeout. Both exported roots call doHangAnalysis and dumpHangAnalysis; the
    # latter reaches raw ring-status and full RCS register dumps through the
    # ring buffer's cached MMIO base at +0x50. Route both shared boundaries so a
    # diagnostic request cannot become a PF register probe on a VF.
    for owner in (RING_DEBUG_ENGINE, FIFO_DIAGNOSIS):
        if len(direct_branches(owner, RING_DO_HANG)) != 1 or \
                len(direct_branches(owner, RING_DUMP_HANG)) != 1:
            raise AssertionError(
                f"{path}: hardware-diagnosis root graph changed: {owner}")
    if len(direct_branches(RING_DO_HANG, GATHER_RING)) != 1 or \
            len(direct_branches(RING_DUMP_HANG, RING_DUMP_STATUS)) != 1 or \
            len(direct_branches(RING_DUMP_HANG, RING_DUMP_REGISTERS)) != 1:
        raise AssertionError(f"{path}: physical hang-diagnosis graph changed")
    if len(direct_branches(RING_DUMP_STATUS, SAFE_FORCE_WAKE)) != 2 or \
            len(direct_branches(RING_DUMP_REGISTERS, SAFE_FORCE_WAKE)) != 2:
        raise AssertionError(f"{path}: hang dump force-wake inventory changed")
    rcs_dump_start = value(RING_DUMP_REGISTERS_RCS)
    rcs_dump_body = image[rcs_dump_start:next_symbol(rcs_dump_start)]
    if rcs_dump_body.count(bytes.fromhex(
            "48 8b 43 50 44 8b 80 28 20 00 00")) != 1:
        raise AssertionError(f"{path}: raw RCS register-dump anchor changed")

    # IOAccel's FIFO reset virtual is a second error-recovery root. It invokes
    # the ring-buffer +0x168 reset virtual, resumes the scheduler and replays up
    # to two stamps. The concrete physical reset takes five force-wake paths,
    # writes engine-control registers, executes the 0x4A08/0x941C/0xCEC4 reset
    # sequence and replays the accelerator reset list. Both the root and the
    # lower primitive must remain behind classified-VF routes.
    ring_vtable = value(RING_VTABLE)
    fifo_vtable = value(FIFO_VTABLE)
    if struct.unpack_from(
            "<Q", image, ring_vtable + 16 + 0x168)[0] != value(
                RING_RESET_GRAPHICS):
        raise AssertionError(f"{path}: ring physical-reset virtual slot changed")
    if struct.unpack_from(
            "<Q", image, fifo_vtable + 16 + 0x200)[0] != value(
                FIFO_RESET_REPLAY):
        raise AssertionError(f"{path}: FIFO reset/replay virtual slot changed")
    replay_start = value(FIFO_RESET_REPLAY)
    replay_body = image[replay_start:next_symbol(replay_start)]
    for dispatch, count, label in (
            (bytes.fromhex("ff 90 68 01 00 00"), 1, "physical reset"),
            (bytes.fromhex("ff 90 80 01 00 00"), 2, "scheduler resume"),
            (bytes.fromhex("ff 90 d8 01 00 00"), 2, "stamp completion")):
        if replay_body.count(dispatch) != count:
            raise AssertionError(
                f"{path}: FIFO reset/replay {label} inventory changed")
    if len(direct_branches(FIFO_RESET_REPLAY, FIFO_SUBMIT_STAMP)) != 2 or \
            len(direct_branches(FIFO_RESET_REPLAY, RING_SLEEP_STAMP)) != 2:
        raise AssertionError(f"{path}: FIFO reset/replay submission graph changed")
    reset_start = value(RING_RESET_GRAPHICS)
    reset_body = image[reset_start:next_symbol(reset_start)]
    if len(direct_branches(RING_RESET_GRAPHICS, SAFE_FORCE_WAKE)) != 4 or \
            len(direct_branches(RING_RESET_GRAPHICS, SAFE_FORCE_WAKE_BOOL)) != 1 or \
            len(direct_branches(RING_RESET_GRAPHICS, GET_DEFAULT_RESET)) != 1:
        raise AssertionError(f"{path}: physical engine-reset call graph changed")
    for anchor, count, label in (
            (bytes.fromhex("48 8b 80 40 12 00 00"), 2, "raw MMIO base"),
            (bytes.fromhex("c7 80 08 4a 00 00 04 00 04 00"), 1,
             "engine reset assert"),
            (bytes.fromhex("c7 80 08 4a 00 00 00 00 04 00"), 1,
             "engine reset release"),
            (bytes.fromhex("c7 80 00 41 00 00 b1 b1 f0 f0"), 1,
             "fault register clear 0"),
            (bytes.fromhex("c7 80 04 41 00 00 b2 b2 f0 f0"), 1,
             "fault register clear 1"),
            (bytes.fromhex("ff 90 90 01 00 00"), 1,
             "scheduler reset completion")):
        if reset_body.count(anchor) != count:
            raise AssertionError(
                f"{path}: physical engine-reset {label} inventory changed")

    # Context shared-private lifecycle reaches routed GuC descriptor operations,
    # not the similarly named IGGuC shared-private allocation methods. Cleanup
    # invalidates translations before detaching the descriptor.
    if len(direct_branches(SCHEDULER4_INIT_PRIVATE, GUC_ATTACH_DESC)) != 1:
        raise AssertionError(f"{path}: shared-private attach dispatch changed")
    invalidate_edges = direct_branches(SCHEDULER4_CLEANUP_PRIVATE, GUC_INVALIDATE_TLB)
    detach_edges = direct_branches(SCHEDULER4_CLEANUP_PRIVATE, GUC_DETACH_DESC)
    if len(invalidate_edges) != 1 or len(detach_edges) != 1 or \
            invalidate_edges[0] >= detach_edges[0]:
        raise AssertionError(f"{path}: shared-private invalidate/detach order changed")
    for slot, target in ((0x128, SCHEDULER4_INIT_PRIVATE),
                         (0x138, SCHEDULER4_CLEANUP_PRIVATE),
                         (0x1c8, SCHEDULER4_BIND),
                         (0x1d8, SCHEDULER4_UNBIND)):
        if struct.unpack_from("<Q", image,
                              value(SCHEDULER4_VTABLE) + 16 + slot)[0] != value(target):
            raise AssertionError(f"{path}: context/ring ownership slot {slot:#x} changed")
    context_free_start = value(CONTEXT_FREE)
    context_free_body = image[context_free_start:next_symbol(context_free_start)]
    ownership_anchors = (
        bytes.fromhex("48 8b bb b0 00 00 00"),
        bytes.fromhex("48 8b bb b8 00 00 00"),
        bytes.fromhex("ff 90 38 01 00 00"),
        bytes.fromhex("48 8b bb a8 00 00 00"),
        bytes.fromhex("48 8b b3 98 00 00 00"),
        bytes.fromhex("48 8b 7b 58"))
    if [context_free_body.count(anchor) for anchor in ownership_anchors] != [1, 1, 1, 1, 2, 1] or \
            [context_free_body.index(anchor) for anchor in ownership_anchors] != sorted(
                context_free_body.index(anchor) for anchor in ownership_anchors):
        raise AssertionError(f"{path}: context detach/ring/image/task teardown order changed")
    address_start = value(CONTEXT_RING_GPU_ADDRESS)
    address_body = image[address_start:next_symbol(address_start)]
    if address_body.count(bytes.fromhex(
            "48 8b 87 b0 00 00 00 48 8b b8 80 00 00 00")) != 1:
        raise AssertionError(f"{path}: context DMA ring backing source changed")
    init_start = value(CONTEXT_INIT)
    init_body = image[init_start:next_symbol(init_start)]
    if init_body.count(bytes.fromhex(
            "ff 90 28 01 00 00 49 8b 7d 50")) != 1:
        raise AssertionError(f"{path}: native unchecked context attach call changed")

    # Both stamp address views borrow the task's same +0x288 buffer. Ordinary
    # contexts retain the task, while the flag-bit-0 path skips that retain;
    # the task's last-release path attempts notification of four owned contexts.
    # These local contracts do not prove external task ownership or completion.
    for getter, target in ((TASK_STAMP_GPU_ADDRESS, MAPPED_BUFFER_GPU_ADDRESS),
                           (TASK_STAMPS, SHARED_BUFFER_CPU_ADDRESS)):
        getter_start = value(getter)
        getter_body = image[getter_start:next_symbol(getter_start)]
        if getter_body[:12] != bytes.fromhex("55 48 89 e5 48 8b bf 88 02 00 00 5d") or \
                len(direct_branches(getter, target)) != 1:
            raise AssertionError(f"{path}: task stamp CPU/GPU backing identity changed")
    scratch_getter_start = value(TASK_SCRATCH_GPU_ADDRESS)
    scratch_getter_body = image[scratch_getter_start:next_symbol(scratch_getter_start)]
    if scratch_getter_body[:12] != bytes.fromhex("55 48 89 e5 48 8b bf 80 02 00 00 5d") or \
            len(direct_branches(TASK_SCRATCH_GPU_ADDRESS, MAPPED_BUFFER_GPU_ADDRESS)) != 1:
        raise AssertionError(f"{path}: task scratch backing provenance changed")
    if init_body.count(bytes.fromhex(
            "41 f6 45 6e 01 75 12 49 8b 7d 58 48 8b 07 ff 50 20")) != 1:
        raise AssertionError(f"{path}: context conditional task retain changed")
    stamp_init_start = value(TASK_INIT_STAMPS)
    stamp_init_body = image[stamp_init_start:next_symbol(stamp_init_start)]
    if len(direct_branches(TASK_INIT_STAMPS, SHARED_BUFFER_CLONE)) != 1 or \
            len(direct_branches(TASK_INIT_STAMPS, SHARED_BUFFER_FACTORY)) != 1 or \
            stamp_init_body.count(bytes.fromhex("be 00 30 00 00")) != 1 or \
            stamp_init_body.count(bytes.fromhex("48 89 83 88 02 00 00")) != 1:
        raise AssertionError(f"{path}: task stamp allocation/clone contract changed")
    task_free_start = value(TASK_FREE)
    task_free_body = image[task_free_start:next_symbol(task_free_start)]
    if task_free_body.count(bytes.fromhex("48 8b bb 88 02 00 00")) != 1 or \
            task_free_body.count(bytes.fromhex("48 c7 83 88 02 00 00 00 00 00 00")) != 1:
        raise AssertionError(f"{path}: task stamp final-release contract changed")
    release_start = value(TASK_RELEASE)
    release_body = image[release_start:next_symbol(release_start)]
    if len(direct_branches(TASK_RELEASE, CONTEXT_NOTIFY_COMPLETE)) != 4:
        raise AssertionError(f"{path}: task owned-context completion inventory changed")
    for offset in (0x2a0, 0x2a8, 0x298, 0x290):
        if release_body.count(b"\x48\xc7\x83" + struct.pack("<I", offset) + b"\0" * 4) != 1:
            raise AssertionError(f"{path}: task owned-context {offset:#x} cleanup changed")
    if len(direct_branches(CONTEXT_NOTIFY_COMPLETE, FIFO_NOTIFY_COMPLETE)) != 1 or \
            len(direct_branches(FIFO_NOTIFY_COMPLETE, RING_NOTIFY_COMPLETE)) != 1:
        raise AssertionError(f"{path}: task/context/FIFO notification graph changed")
    notify_start = value(CONTEXT_NOTIFY_COMPLETE)
    notify_body = image[notify_start:next_symbol(notify_start)]
    if notify_body.count(bytes.fromhex(
            "4d 85 ff 74 16 48 8b 7b 58 48 8b 07 ff 50 20 c6 83 c8 00 00 00 01")) != 1:
        raise AssertionError(f"{path}: asynchronous context notification task retain changed")
    # notifyComplete registers an event dependency, not GPU completion. The
    # inherited mergeEvent virtual is unresolved on disk: prove its external
    # relocation rather than interpreting the zero vtable word as a local call.
    merge_slot = value(EVENT_MACHINE_VTABLE) + 16 + 0x1b8
    merge_relocations = []
    for index in range(external_count):
        address, bits = struct.unpack_from("<iI", image, external_offset + index * 8)
        if address == merge_slot:
            merge_relocations.append((names[bits & 0xffffff],
                ((bits >> 24) & 1, (bits >> 25) & 3,
                 (bits >> 27) & 1, (bits >> 28) & 0xf)))
    if merge_relocations != [(EVENT_MERGE, (0, 3, 1, 0))] or \
            struct.unpack_from("<Q", image, merge_slot)[0] != 0:
        raise AssertionError(f"{path}: ring notification mergeEvent virtual changed")
    # The mapped-buffer getter delegates to an inherited mapping method. Its
    # unresolved vtable word is NOT a GGTT address or proof of address space.
    mapping_slot = value(MEMORY_MAP_VTABLE) + 16 + 0x128
    mapping_init = value(MAPPED_BUFFER_INIT)
    expected_mapping_relocations = {
        mapping_slot: (MEMORY_MAP_GPU_ADDRESS, (0, 3, 1, 0)),
        mapping_init + 0x71: (SYS_MEMORY_FACTORY, (1, 2, 1, 2)),
        mapping_init + 0xca: (PREPARE_MAPPING, (1, 2, 1, 2)),
        value(MEMORY_MAP_VTABLE) + 16 + 0x140:
            (MEMORY_MAP_COMPLETE, (0, 3, 1, 0)),
        value(MAPPED_BUFFER_FREE) + 0x13:
            (MEMORY_MAP_FINISH_EVENT, (1, 2, 1, 2)),
        value(SHARED_BUFFER_FREE) + 0x23:
            (SYS_MEMORY_UNLOCK, (1, 2, 1, 2)),
        value(SHARED_BUFFER_UNLOCK) + 0x1c:
            (SYS_MEMORY_UNLOCK, (1, 2, 1, 2)),
    }
    observed_mapping_relocations = {address: [] for address in expected_mapping_relocations}
    for index in range(external_count):
        address, bits = struct.unpack_from("<iI", image, external_offset + index * 8)
        if address in observed_mapping_relocations:
            observed_mapping_relocations[address].append((names[bits & 0xffffff],
                ((bits >> 24) & 1, (bits >> 25) & 3,
                 (bits >> 27) & 1, (bits >> 28) & 0xf)))
    for address, expected in expected_mapping_relocations.items():
        if observed_mapping_relocations[address] != [expected]:
            raise AssertionError(f"{path}: mapping provenance relocation {address:#x} changed")
    if struct.unpack_from("<Q", image, mapping_slot)[0] != 0:
        raise AssertionError(f"{path}: inherited mapping getter unexpectedly resolved")
    free_start = value(MAPPED_BUFFER_FREE)
    free_body = image[free_start:next_symbol(free_start)]
    for anchor in ("48 8b 7b 30 48 8b 07 ff 90 40 01 00 00",
                   "ff 50 28 48 c7 43 30 00 00 00 00"):
        if free_body.count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: mapped-buffer mapping teardown changed")
    # Explicit task cleanup only drops references, whereas CPU unlock clears
    # its mapping even on a retained object. Neither is a GPU idle proof.
    stamps_start = value(TASK_RELEASE_STAMPS)
    stamps_body = image[stamps_start:next_symbol(stamps_start)]
    if stamps_body.count(bytes.fromhex("ff 50 28")) != 2:
        raise AssertionError(f"{path}: explicit task stamp cleanup releases changed")
    for offset in (0x280, 0x288):
        if stamps_body.count(b"\x48\xc7\x83" + struct.pack("<I", offset) + b"\0" * 4) != 1:
            raise AssertionError(f"{path}: explicit task stamp cleanup {offset:#x} changed")
    unlock_start = value(SHARED_BUFFER_UNLOCK)
    unlock_body = image[unlock_start:next_symbol(unlock_start)]
    if unlock_body.count(bytes.fromhex("48 c7 43 38 00 00 00 00")) != 1 or \
            bytes.fromhex("ff 90 40 01 00 00") in unlock_body:
        raise AssertionError(f"{path}: CPU unlock/GPU mapping distinction changed")
    mapping_body = image[mapping_init:next_symbol(mapping_init)]
    for anchor in ("ff 91 38 01 00 00", "ff 90 38 01 00 00",
                   "84 c0 74 2d 4c 89 7b 30 4c 89 73 10 4c 89 6b 18"):
        if mapping_body.count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: mapping admission/publication contract changed")
    options_start = value(MAPPED_BUFFER_MAPPING_OPTIONS)
    if image[options_start:options_start + 11] != bytes.fromhex(
            "55 48 89 e5 b8 07 00 00 00 5d c3"):
        raise AssertionError(f"{path}: mapped-buffer mapping options changed")
    config_start = value(POPULATE_ACCEL_CONFIG)
    config_body = image[config_start:next_symbol(config_start)]
    # Pin the RIP-relative PPGTT property lookup, default 1, and the bit-8
    # assignment in the 64-bit feature word. The inherited option-bit meaning
    # is still unknown; do not turn this observation into a GGTT quota check.
    ppgtt_lookup = config_start + 0xe2
    if image[ppgtt_lookup:ppgtt_lookup + 3] != bytes.fromhex("48 8d 35"):
        raise AssertionError(f"{path}: PPGTT property lookup changed")
    ppgtt_name = ppgtt_lookup + 7 + struct.unpack_from("<i", image, ppgtt_lookup + 3)[0]
    if image[ppgtt_name:ppgtt_name + 6] != b"PPGTT\0" or \
            image[ppgtt_lookup + 7:ppgtt_lookup + 15] != bytes.fromhex(
                "4c 89 e7 ba 01 00 00 00"):
        raise AssertionError(f"{path}: PPGTT property/default changed")
    if config_body.count(bytes.fromhex(
            "21 d8 c1 e0 08 48 c7 c1 ff fe ff ff 49 23 8c 24 90 11 00 00 "
            "48 09 c1 49 89 8c 24 90 11 00 00")) != 1:
        raise AssertionError(f"{path}: PPGTT feature-bit publication changed")
    ring_notify_start = value(RING_NOTIFY_COMPLETE)
    ring_notify_body = image[ring_notify_start:next_symbol(ring_notify_start)]
    for anchor in (bytes.fromhex("83 7f 38 00 78 30"),
                   bytes.fromhex("b3 01 48 85 f6 74 28"),
                   bytes.fromhex("ff 90 b8 01 00 00")):
        if ring_notify_body.count(anchor) != 1:
            raise AssertionError(f"{path}: ring dependency-registration contract changed")
    if direct_branches(RING_NOTIFY_COMPLETE, RING_SLEEP_STAMP) or \
            direct_branches(RING_NOTIFY_COMPLETE, SAFE_FORCE_WAKE):
        raise AssertionError(f"{path}: ring notification gained a hardware wait")

    # Normal garbage collection checks the concrete context idle virtual.
    # Forced collection/drain intentionally bypass that check; descriptor
    # deregistration and retained DMA backing must remain independent barriers.
    for table, slot, target in ((CONTEXT_VTABLE, 0x128, CONTEXT_CHECK),
                                (CONTEXT_VTABLE, 0x118, GC_OBJECT_RELEASE_UNCHECKED),
                                (SCHEDULER4_VTABLE, 0x168, SCHEDULER4_CONTEXT_IDLE)):
        if struct.unpack_from("<Q", image, value(table) + 16 + slot)[0] != value(target):
            raise AssertionError(f"{path}: garbage collection virtual {slot:#x} changed")
    if len(direct_branches(GC_OBJECT_RELEASE, GC_ADD)) != 1 or \
            len(direct_branches(SCHEDULER4_CONTEXT_IDLE, GUC_KMD_CONTEXT_IDLE)) != 1:
        raise AssertionError(f"{path}: context GC/idle dispatch graph changed")
    for owner, anchor, count in (
            (GC_OBJECT_RELEASE, bytes.fromhex("ff 90 28 01 00 00"), 1),
            (CONTEXT_CHECK, bytes.fromhex("48 8b 80 68 01 00 00"), 1),
            (GC_COLLECT, bytes.fromhex("ff 50 28"), 1),
            (GC_FORCE_COLLECT, bytes.fromhex("ff 90 18 01 00 00"), 1),
            (GC_DRAIN, bytes.fromhex("ff 50 28"), 1),
            (GC_DRAIN, bytes.fromhex("ff 90 18 01 00 00"), 1)):
        owner_start = value(owner)
        if image[owner_start:next_symbol(owner_start)].count(anchor) != count:
            raise AssertionError(f"{path}: context GC release/check inventory changed in {owner}")

    # Normal producer backpressure polls shared context head/stamp memory.
    # Keep its timeout diagnostic behind the already isolated entry rather
    # than replacing normal waits or manufacturing completion.
    if len(direct_branches(RING_WAIT_SPACE, RING_WAIT_TIMEOUT)) != 2 or \
            len(direct_branches(RING_WAIT_TIMEOUT, RING_DEBUG_ENGINE)) != 1:
        raise AssertionError(f"{path}: ring backpressure timeout graph changed")
    wait_start = value(RING_WAIT_TIMEOUT)
    wait_body = image[wait_start:next_symbol(wait_start)]
    stamp_start = value(RING_SLEEP_STAMP)
    stamp_body = image[stamp_start:next_symbol(stamp_start)]
    # Slot +8 is CPU-published submitted bookkeeping, not the completed value
    # read at slot +0. Main/compute override the packet emitter but retain the
    # same software stamp bookkeeping. Never use the +8 write as GPU evidence.
    for owner in (RING_WRITE_STAMP, RING_MAIN_WRITE_STAMP, RING_COMPUTE_WRITE_STAMP):
        owner_start = value(owner)
        body = image[owner_start:next_symbol(owner_start)]
        for anchor in (bytes.fromhex("ff 90 40 01 00 00"),
                       bytes.fromhex("44 89 74 08 08 44 89 73 44 c6 43 48 01")):
            if body.count(anchor) != 1:
                raise AssertionError(f"{path}: submitted-stamp bookkeeping changed in {owner}")
    if stamp_body.count(bytes.fromhex(
            "48 c1 e2 06 8b 04 10 41 89 07")) != 1:
        raise AssertionError(f"{path}: completed-stamp slot-zero read changed")
    for table, commit in ((RING_VTABLE, RING_COMMIT_STAMP),
                          ("__ZTV24IGHardwareRingBufferMain", RING_MAIN_COMMIT_STAMP),
                          ("__ZTV27IGHardwareRingBufferCompute", RING_COMPUTE_COMMIT_STAMP)):
        if struct.unpack_from("<Q", image, value(table) + 16 + 0x140)[0] != value(commit):
            raise AssertionError(f"{path}: stamp packet encoder virtual changed")
    # Main/compute encode PIPE_CONTROL post-sync writes. Pin the destination
    # (stamp GPU base + index*64), sequence data, scratch prerequisite and
    # actual writeBuffer graph; source bookkeeping alone cannot prove execution.
    for owner in (RING_MAIN_COMMIT_STAMP, RING_COMPUTE_COMMIT_STAMP):
        start = value(owner)
        body = image[start:next_symbol(start)]
        if len(direct_branches(owner, RING_WRITE_BUFFER)) != 4 or \
                len(direct_branches(owner, TASK_SCRATCH_GPU_ADDRESS)) != 2 or \
                len(direct_branches(owner, RING_GTT_WRITE_MODE)) != 2:
            raise AssertionError(f"{path}: PIPE_CONTROL stamp encoder call graph changed")
        for anchor in (bytes.fromhex("48 b8 04 00 00 7a 98 44 10 01"),
                       bytes.fromhex("49 63 74 24 38 48 c1 e6 06 49 03 74 24 28"),
                       bytes.fromhex("8b 55 b4 49 89 55 10")):
            if body.count(anchor) != 1:
                raise AssertionError(f"{path}: PIPE_CONTROL stamp packet/data anchor changed")
    base_start = value(RING_COMMIT_STAMP)
    base_body = image[base_start:next_symbol(base_start)]
    if len(direct_branches(RING_COMMIT_STAMP, RING_WRITE_BUFFER)) != 3 or \
            base_body.count(bytes.fromhex("48 8d 84 02 03 40 00 13")) != 1:
        raise AssertionError(f"{path}: base MI_FLUSH_DW stamp encoder changed")
    for body, anchor, count, label in (
            (wait_body, bytes.fromhex("48 8b 43 18 8b 40 10"), 1,
             "shared context head"),
            (wait_body, bytes.fromhex("ff 90 50 01 00 00"), 1,
             "scheduler progress query"),
            (stamp_body, bytes.fromhex("48 8b 43 30"), 3,
             "shared stamp backing")):
        if body.count(anchor) != count:
            raise AssertionError(f"{path}: ring wait {label} inventory changed")
    for owner in (RING_WAIT_SPACE, RING_WAIT_TIMEOUT, RING_SLEEP_STAMP):
        if direct_branches(owner, SAFE_FORCE_WAKE) or \
                direct_branches(owner, RING_RESET_GRAPHICS):
            raise AssertionError(f"{path}: normal ring wait enters physical recovery")

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

    # IntelAccelerator's factory slot constructs IntelTGLMemoryManager. Its
    # base init zeroes the two eDRAM capability bytes and then unconditionally
    # dispatches virtual slot 0x148 to the TGL detector. That detector accesses
    # physical eDRAM capability/control registers before engine start, including
    # a property-dependent programming branch. Force-wake suppression alone is
    # not containment because every raw load/store remains reachable.
    accelerator_vtable = value(ACCELERATOR_VTABLE)
    if struct.unpack_from(
            "<Q", image, accelerator_vtable + 16 + 0xAF0)[0] != value(
                NEW_MEMORY_MANAGER):
        raise AssertionError(
            f"{path}: accelerator memory-manager factory slot changed")
    memory_vtable = value(TGL_MEMORY_VTABLE)
    if struct.unpack_from(
            "<Q", image, memory_vtable + 16 + 0x148)[0] != value(
                TGL_DETECT_EDRAM):
        raise AssertionError(f"{path}: TGL eDRAM virtual slot changed")
    factory_start = value(NEW_MEMORY_MANAGER)
    factory_end = next_symbol(factory_start)
    metaclass_refs = []
    for candidate in range(factory_start, factory_end - 6):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 05"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == value(TGL_MEMORY_METACLASS):
            metaclass_refs.append(candidate)
    if len(metaclass_refs) != 1:
        raise AssertionError(
            f"{path}: memory-manager factory no longer selects TGL metaclass")
    memory_init_start = value(MEMORY_MANAGER_INIT)
    memory_init_body = image[memory_init_start:next_symbol(memory_init_start)]
    if memory_init_body.count(MEMORY_EDRAM_ZERO_STATE) != 1 or \
            memory_init_body.count(bytes.fromhex("ff 90 48 01 00 00")) != 1:
        raise AssertionError(
            f"{path}: base memory-manager eDRAM init/dispatch changed")
    edram_start = value(TGL_DETECT_EDRAM)
    edram_body = image[edram_start:next_symbol(edram_start)]
    for anchor in (EDRAM_CAPABILITY_MMIO_READ, EDRAM_CONTROL_MMIO_WRITES,
                   EDRAM_CONFIRM_MMIO_READ):
        if edram_body.count(anchor) != 1:
            raise AssertionError(
                f"{path}: TGL physical eDRAM MMIO inventory changed")
    if len(direct_branches(TGL_DETECT_EDRAM, SAFE_FORCE_WAKE)) != 4:
        raise AssertionError(
            f"{path}: TGL eDRAM force-wake inventory changed")
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
        SCHEDULER_HALT,
        SCHEDULER_RESUME,
        ENCODE_DEBUG,
        RING_DO_HANG,
        RING_DUMP_HANG,
        FIFO_RESET_REPLAY,
        RING_RESET_GRAPHICS,
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

    timeout_routes_start = pci_resolution.rfind(
        "KernelPatcher::RouteRequest requests[]", 0,
        pci_resolution.index(FENCE_ALLOCATE))
    if timeout_routes_start < 0:
        raise AssertionError(f"{path}: missing VF accelerator route table")
    timeout_routes_end = pci_resolution.index(
        "Failed to route VF accelerator symbols", timeout_routes_start)
    timeout_routes = "".join(
        pci_resolution[timeout_routes_start:timeout_routes_end].split())
    if pci_resolution.rfind("if (vfActive)", 0, timeout_routes_start) < 0:
        raise AssertionError(f"{path}: timeout isolation routes are not VF-only")
    for symbol, wrapper in (
            (SCHEDULER_HALT, "vfSuppressTimeoutHardwareAction"),
            (SCHEDULER_RESUME, "vfSuppressTimeoutHardwareAction"),
            (ENCODE_DEBUG, "vfSuppressPhysicalDebugCapture"),
            (RING_DO_HANG, "vfSuppressHangAnalysis"),
            (RING_DUMP_HANG, "vfSuppressHangDump"),
            (FIFO_RESET_REPLAY, "vfRejectHardwareResetReplay"),
            (RING_RESET_GRAPHICS, "vfRejectPhysicalEngineReset")):
        if '{"' + symbol + '",' + wrapper + '},' not in timeout_routes:
            raise AssertionError(
                f"{path}: missing VF timeout route {symbol} -> {wrapper}")

    timeout_noop = function_body(
        source, "void Gen11::vfSuppressTimeoutHardwareAction(")
    debug_noop = function_body(
        source, "void Gen11::vfSuppressPhysicalDebugCapture(")
    hang_analysis = function_body(
        source, "uint32_t Gen11::vfSuppressHangAnalysis(")
    hang_dump = function_body(source, "void Gen11::vfSuppressHangDump(")
    for body, arguments, label in (
            (timeout_noop, ("(void)that;", "(void)engine;"), "timeout action"),
            (debug_noop, ("(void)that;", "(void)reason;"), "debug capture"),
            (hang_analysis, ("(void)that;", "return 0;"), "hang analysis"),
            (hang_dump, ("(void)that;",), "hang dump")):
        if any(token not in body for token in arguments):
            raise AssertionError(f"{path}: incomplete VF {label} replacement")
        for forbidden in ("FunctionCast", "callback->", "getMember", "0x1240",
                          "SafeForceWake", "MMIO"):
            if forbidden in body:
                raise AssertionError(
                    f"{path}: VF {label} replacement re-enters hardware through {forbidden}")

    reset_replay = function_body(
        source, "void Gen11::vfRejectHardwareResetReplay(")
    engine_reset = function_body(
        source, "bool Gen11::vfRejectPhysicalEngineReset(")
    if "vfMarkProtocolFault(" not in reset_replay or \
            "physical engine reset/replay requested on VF" not in reset_replay:
        raise AssertionError(f"{path}: VF reset/replay root does not quarantine transport")
    for token in ("(void)that;", "(void)context;", "vfMarkProtocolFault(",
                  "physical engine reset requested on VF", "return false;"):
        if token not in engine_reset:
            raise AssertionError(
                f"{path}: VF physical engine-reset rejection is incomplete: {token}")
    for body, label in ((reset_replay, "reset/replay"),
                        (engine_reset, "physical engine reset")):
        for forbidden in ("FunctionCast", "callback->", "getMember", "0x1240",
                          "SafeForceWake", "MMIO"):
            if forbidden in body:
                raise AssertionError(
                    f"{path}: VF {label} rejection re-enters hardware through {forbidden}")

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
    if ('{"' + TGL_DETECT_EDRAM + '",vfDisableEdramProbe},' not in
            normalized_pci_resolution):
        raise AssertionError(f"{path}: physical eDRAM detector is not VF-routed")
    edram_disable = function_body(
        source, "void Gen11::vfDisableEdramProbe(void *that)")
    for token in (
            "getMember<uint8_t>(that, 0x20) = 0",
            "getMember<uint8_t>(that, 0x21) = 0"):
        if token not in edram_disable:
            raise AssertionError(
                f"{path}: VF eDRAM capability state is not cleared: {token}")
    for forbidden in ("FunctionCast", "callback->", "SafeForceWake", "MMIO"):
        if forbidden in edram_disable:
            raise AssertionError(
                f"{path}: VF eDRAM route re-enters hardware through {forbidden}")

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
    if not attach.index("NGVfContextShutdown::validPacketBacking(stampIndex, stampBytes, scratchBytes)") < \
            attach.index("contextBacking->retain();") < attach.index("vfSendCtbFastAction(that, request,"):
        raise AssertionError(f"{path}: packet backing bounds are not checked before registration")
    for token in ("!ringBacking ||", "entry.ringBacking != ringBacking",
                  "kVfContextRingObjectOffset", "kVfRingMappedBufferOffset"):
        if token not in attach:
            raise AssertionError(f"{path}: direct context ring ownership lacks {token}")
    if not attach.index("ringBacking->retain();") < attach.index(
            "entry.ringBacking = ringBacking;") < attach.index(
                "entry.state = kVfGucContextRegistering;") < attach.index(
                    "vfSendCtbFastAction(that, request,"):
        raise AssertionError(f"{path}: ring DMA backing is not pinned before registration")
    partial_fault = 'vfMarkProtocolFault("failed to retire partially registered GuC context");'
    if not attach[attach.index(partial_fault) + len(partial_fault):].lstrip().startswith(
            'PANIC_COND(true, "ngreen",'):
        raise AssertionError(f"{path}: partially owned context can return into native initialization")
    retire = function_body(source, "void vfReleaseRetiredContextBacking(uint16_t gucId)")
    for token in ("!gVfProtocolFault", "entry.state == kVfGucContextTombstone",
                  "entry.refCount == 0"):
        if token not in retire:
            raise AssertionError(f"{path}: retired backing release lacks {token}")
    if not retire.index("ringBacking = entry.ringBacking;") < retire.index(
            "NGVfContextEvent::clearReleasedIdentity(entry);") < retire.index(
                "IOSimpleLockUnlockEnableInterrupt(") < retire.index(
                    "ringBacking->release();"):
        raise AssertionError(f"{path}: retired DMA ring release/lock order changed")
    for backing in ("stampBacking", "scratchBacking"):
        for token in ("!" + backing + " ||", "entry." + backing + " != " + backing):
            if token not in attach:
                raise AssertionError(f"{path}: context packet backing identity lacks {token}")
        if not attach.index(backing + "->retain();") < attach.index(
                "entry." + backing + " = " + backing + ";") < attach.index(
                    "entry.state = kVfGucContextRegistering;"):
            raise AssertionError(f"{path}: packet backing is not retained before registration")
        if not retire.index(backing + " = entry." + backing + ";") < retire.index(
                "NGVfContextEvent::clearReleasedIdentity(entry);") < retire.index(
                    "IOSimpleLockUnlockEnableInterrupt(") < retire.index(backing + "->release();"):
            raise AssertionError(f"{path}: packet backing release bypasses ownership boundary")
        for signature in ("int32_t vfFindContextLocked(uint32_t lrcaPage)",
                          "int32_t vfReserveContextLocked(uint32_t lrcaPage)",
                          "bool vfDirectContextTableUnowned()"):
            if backing not in function_body(source, signature):
                raise AssertionError(f"{path}: table reuse/ownership ignores {backing}")
    detach = function_body(source, "void Gen11::vfDetachContextDesc(void *that, const uint32_t *descriptor)")
    for reason in (
            "VF detach without valid context bookkeeping",
            "invalid VF context identity before detach",
            "context retirement without pinned H2G queue",
            "duplicate final context detach",
            "VF detach descriptor/backing identity mismatch",
            "VF detach has no direct GuC context record",
            "GuC context teardown timeout"):
        fault = 'vfMarkProtocolFault("' + reason + '");'
        following = detach[detach.index(fault) + len(fault):].lstrip()
        if not following.startswith('PANIC_COND(true, "ngreen",'):
            raise AssertionError(f"{path}: unsafe void detach can return after {reason}")

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
