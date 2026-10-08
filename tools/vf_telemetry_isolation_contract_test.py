#!/usr/bin/env python3
"""Pin Tahoe's unsafe telemetry graph and the VF-only fail-closed replacement."""

import pathlib
import plistlib
import struct
import sys


ACCELERATOR_START = "__ZN16IntelAccelerator5startEP9IOService"
ACCELERATOR_STOP = "__ZN16IntelAccelerator4stopEP9IOService"
INIT_SYSCTL = "__ZN16IntelAccelerator10initSysctlEv"
UNREGISTER_SYSCTL = "__ZN16IntelAccelerator16unregisterSysctlEv"
INTEL_LONG_SYSCTL = "__ZL16intelLong_sysctlP10sysctl_oidPviP10sysctl_req"
IDVAR_SET_LOCKED = "___idvarSetParamLocked"
ACCELERATOR_LIST = "__ZL14l_accelerators"
TELEMETRY_CREATE = "__ZN16IntelAccelerator22telemetryCreateManagerEj"
HWS_MEMORY = "__ZN16IntelAccelerator28initHardwareStatusPageMemoryEv"
ENGINE_START = "__ZN16IntelAccelerator19startGraphicsEngineEv"
USAGE_FACTORY = "__ZN16IGTelemetryUsage28withAcceleratorAndStampIndexEP16IntelAcceleratori"
USAGE_INIT = "__ZN16IGTelemetryUsage32initWithAcceleratorAndStampIndexEP16IntelAcceleratori"
USAGE_ALLOC = "__ZN16IGTelemetryUsage13allocUsageMemEv"
USAGE_DEALLOC = "__ZN16IGTelemetryUsage15deallocUsageMemEv"
MANAGER_INIT = "__ZN18IGTelemetryManager19initWithAcceleratorEP16IntelAcceleratorj"
SAFE_FORCE_WAKE = "__ZN16IntelAccelerator13SafeForceWakeEb"
OA_INITIALIZE = "__ZN14IGSupportMDAPI18initializeOaBufferE23MDAPIOABufferReportTypejjPj24MDAPIOABufferOverrunModeb"
OA_FINALIZE = "__ZN14IGSupportMDAPI16finalizeOaBufferEbPj"
OA_ENABLE = "__ZN14IGSupportMDAPI24enableOaBufferCollectionEPj"
OA_DISABLE = "__ZN14IGSupportMDAPI25disableOABufferCollectionEPj"
OA_READ = "__ZN14IGSupportMDAPI12readOABufferEP21MDAPIReadOABufferOpInP22MDAPIReadOABufferOpOut"
OA_MAP = "__ZN14IGSupportMDAPI17mapOABufferMemoryEP27IntelDeviceMapStatsMemInOutP4task"
TRACE_INIT = "__ZN25IGAccelTraceStreamManager4initEP16IntelAccelerator"
TRACE_STATE_CHANGED = "__ZN25IGAccelTraceStreamManager18ktraceStateChangedEiy"
TRACE_TURN_ON = "__ZN25IGAccelTraceStreamManager6turnOnEv"
TRACE_RESYNC = "__ZN25IGAccelTraceStreamManager16resyncTimeTuplesE27TraceStreamCollectionChange"
TRACE_READ_TICS = "__ZN25IGAccelTraceStreamManager21readFullTicTimestampsERyS0_"
TRACE_READ_GPU_TIME = "__ZN25IGAccelTraceStreamManager34gpuReadTimeTupleWithSampleUsageGPUER23TelemetrySampleUsageGPURy"
PERF_CONFIG_SQ_FULL = "__ZN19IGPerfCounterConfig22configPerfcntForSQFullEP16IntelAccelerator"
MANAGER_SETUP_ONLY_OA = "__ZN18IGTelemetryManager18setupOnlyOA_ENABLEEj"
USAGE_REPORT_GLOBAL = "__ZN16IGTelemetryUsage17reportGlobalUsageEv"
MANAGER_DTOR = "__ZN18IGTelemetryManagerD1Ev"
MANAGER_OPERATION = "__ZN18IGTelemetryManager9operationEyxP18TelemetryOperationP19TelemetryConnectionP4task"
MANAGER_PREPARE = "__ZN18IGTelemetryManager16prepareTelemetryEj"
MANAGER_SAMPLE = "__ZN18IGTelemetryManager13sampleToShmemEy"
MANAGER_READ_EDRAM = "__ZN18IGTelemetryManager17readEDRAMCountersEPy"
MANAGER_CALC_GLOBAL = "__ZN18IGTelemetryManager15calcGlobalUsageEy"
MANAGER_INIT_GLOBAL = "__ZN18IGTelemetryManager15initGlobalUsageEv"
MANAGER_PATCH_CONTEXT = "__ZN18IGTelemetryManager20patchContextImageRCSEP27SGfxHardwareContextImageRCS"
MANAGER_PATCH_HELPERS = (
    "__ZN18IGTelemetryManager23configureFlexEUCountersEPji",
    "__ZN18IGTelemetryManager21patchOAContextControlEPji",
    "__ZN18IGTelemetryManager24patchSaveRestoreOARStateEPji",
)
MANAGER_STOP = "__ZN18IGTelemetryManager16onConnectionStopER19TelemetryConnectionP4task"
MANAGER_RETAIN = "__ZN18IGTelemetryManager15telemetryRetainEv"
MANAGER_RELEASE = "__ZN18IGTelemetryManager16telemetryReleaseEv"
MANAGER_INIT_OA = "__ZN18IGTelemetryManager21telemetryInitOaBufferER19TelemetryConnectionP19MDAPIInitOABufferOpS3_yPy"
MANAGER_READ_OA = "__ZN18IGTelemetryManager21telemetryReadOaBufferEP21MDAPIReadOABufferOpInP22MDAPIReadOABufferOpOutPyR19TelemetryConnection"
MANAGER_MAP_OA = "__ZN18IGTelemetryManager26telemetryMapOaBufferMemoryEP27IntelDeviceMapStatsMemInOutS1_yPyP4task"

ROUTES = {
    "__ZN18IGTelemetryManager14printDashboardEy": "vfTelemetryPrintDashboard",
    MANAGER_INIT: "vfTelemetryInitWithAccelerator",
    MANAGER_RETAIN: "vfTelemetryRetain",
    MANAGER_RELEASE: "vfTelemetryRelease",
    MANAGER_CALC_GLOBAL: "vfTelemetryCalcGlobalUsage",
    MANAGER_OPERATION: "vfTelemetryOperation",
    MANAGER_PATCH_CONTEXT: "vfTelemetryPatchContextImage",
    MANAGER_STOP: "vfTelemetryOnConnectionStop",
    MANAGER_INIT_OA: "vfTelemetryInitOaBuffer",
    MANAGER_READ_OA: "vfTelemetryReadOaBuffer",
    MANAGER_MAP_OA: "vfTelemetryMapOaBufferMemory",
    USAGE_ALLOC: "vfTelemetryUsageAlloc",
    "__ZN16IGTelemetryUsage8logUsageEP17IGHardwareContexty": "vfTelemetryUsageLog",
    USAGE_REPORT_GLOBAL: "vfTelemetryUsageReportGlobal",
    "__ZN16IGTelemetryUsage11startSampleEP20IGHardwareRingBufferjjyyyy":
        "vfTelemetryUsageStartSample",
    "__ZN16IGTelemetryUsage10stopSampleEP20IGHardwareRingBufferjj":
        "vfTelemetryUsageStopSample",
    "__ZN16IGTelemetryUsage16frameCalcGPUBusyEP16IntelAcceleratorj":
        "vfTelemetryUsageFrameCalc",
    INIT_SYSCTL: "vfDisableDebugSysctl",
    UNREGISTER_SYSCTL: "vfDisableDebugSysctl",
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def macho_inventory(path):
    image = pathlib.Path(path).read_bytes()
    require(len(image) >= 32, f"{path}: truncated Mach-O")
    header = struct.unpack_from("<8I", image)
    require(header[0] == 0xFEEDFACF, f"{path}: not little-endian Mach-O 64")
    symtab = None
    cursor = 32
    for _ in range(header[4]):
        command, size = struct.unpack_from("<2I", image, cursor)
        require(size >= 8 and cursor + size <= len(image),
                f"{path}: malformed load command")
        if command == 0x2:
            require(symtab is None and size == 24,
                    f"{path}: malformed/duplicate LC_SYMTAB")
            symtab = struct.unpack_from("<4I", image, cursor + 8)
        cursor += size
    require(symtab is not None, f"{path}: no symbol table")
    symbol_offset, symbol_count, string_offset, string_size = symtab
    require(symbol_offset + symbol_count * 16 <= len(image),
            f"{path}: symbol table out of bounds")
    require(string_offset + string_size <= len(image),
            f"{path}: string table out of bounds")
    symbols = {}
    values = []
    for index in range(symbol_count):
        string_index, _, _, _, value = struct.unpack_from(
            "<IBBHQ", image, symbol_offset + index * 16)
        values.append(value)
        if string_index == 0:
            continue
        require(string_index < string_size, f"{path}: bad symbol string index")
        start = string_offset + string_index
        end = image.find(b"\0", start, string_offset + string_size)
        require(end >= 0, f"{path}: unterminated symbol")
        name = image[start:end].decode("utf-8")
        symbols.setdefault(name, []).append(value)

    def value(name):
        matches = [candidate for candidate in symbols.get(name, []) if candidate]
        require(len(matches) == 1, f"{path}: expected one defined {name}")
        return matches[0]

    def next_symbol(address):
        following = sorted(candidate for candidate in values if candidate > address)
        require(following, f"{path}: no symbol after {address:#x}")
        return following[0]

    def body(name):
        start = value(name)
        end = next_symbol(start)
        require(end <= len(image), f"{path}: {name} escapes file-backed image")
        return image[start:end]

    def branches(owner, target):
        start = value(owner)
        end = next_symbol(start)
        destination = value(target)
        result = []
        for candidate in range(start, end - 4):
            if image[candidate] not in (0xE8, 0xE9):
                continue
            displacement = struct.unpack_from("<i", image, candidate + 1)[0]
            if candidate + 5 + displacement == destination:
                result.append(candidate)
        return result

    for symbol in ROUTES:
        value(symbol)

    start = value(ACCELERATOR_START)
    create_calls = branches(ACCELERATOR_START, TELEMETRY_CREATE)
    hws_calls = branches(ACCELERATOR_START, HWS_MEMORY)
    engine_calls = branches(ACCELERATOR_START, ENGINE_START)
    require(len(create_calls) == len(hws_calls) == len(engine_calls) == 1 and
            start < create_calls[0] < hws_calls[0] < engine_calls[0],
            f"{path}: pre-engine telemetry creation order changed")
    require(len(branches(TELEMETRY_CREATE, MANAGER_INIT)) == 1,
            f"{path}: telemetry manager init edge changed")
    require(len(branches(TELEMETRY_CREATE, USAGE_FACTORY)) == 1,
            f"{path}: per-stamp telemetry factory edge changed")

    # The debug sysctl surface is installed after engine start and removed only
    # during the paired stop path. TelemetryDisable is checked after all 55 OID
    # registrations, so it cannot prevent publication of this PF debug control
    # plane on a VF.
    require(len(branches(ACCELERATOR_START, INIT_SYSCTL)) == 1,
            f"{path}: accelerator start sysctl edge changed")
    require(len(branches(ACCELERATOR_STOP, UNREGISTER_SYSCTL)) == 1,
            f"{path}: accelerator stop sysctl edge changed")
    sysctl_init = body(INIT_SYSCTL)
    register_offsets = [
        offset for offset in range(len(sysctl_init) - 11)
        if sysctl_init[offset:offset + 3] == bytes.fromhex("48 8d 3d") and
        sysctl_init[offset + 7:offset + 12] == bytes.fromhex("e8 00 00 00 00")
    ]
    require(len(register_offsets) == 55,
            f"{path}: native debug sysctl registration count changed")
    telemetry_gate = sysctl_init.find(bytes.fromhex("f6 83 93 11 00 00 20"))
    require(telemetry_gate > register_offsets[-1],
            f"{path}: TelemetryDisable/debug-sysctl ordering changed")
    init_start = value(INIT_SYSCTL)
    accelerator_list = value(ACCELERATOR_LIST)
    list_publications = [
        offset for offset in range(len(sysctl_init) - 10)
        if sysctl_init[offset:offset + 3] == bytes.fromhex("48 8d 0d") and
        init_start + offset + 7 + struct.unpack_from(
            "<i", sysctl_init, offset + 3)[0] == accelerator_list and
        sysctl_init[offset + 7:offset + 11] == bytes.fromhex("48 89 1c c1")
    ]
    require(len(list_publications) == 1,
        f"{path}: native accelerator sysctl publication changed")
    require(len(branches(INIT_SYSCTL, MANAGER_SETUP_ONLY_OA)) == 1,
            f"{path}: native sysctl OA bootstrap edge changed")
    require(body(UNREGISTER_SYSCTL).count(bytes.fromhex("e8 00 00 00 00")) == 55,
            f"{path}: native debug sysctl teardown count changed")

    handler = value(INTEL_LONG_SYSCTL)
    leaf_oids = range(0x146750, 0x147741, 0x50)
    require(len(leaf_oids) == 52 and all(
        struct.unpack_from("<Q", image, oid + 0x30)[0] == handler
        for oid in leaf_oids),
        f"{path}: writable debug sysctl handler table changed")
    require(len(branches(INTEL_LONG_SYSCTL, IDVAR_SET_LOCKED)) == 1,
            f"{path}: debug sysctl write dispatch changed")
    for target in (MANAGER_SETUP_ONLY_OA, TRACE_STATE_CHANGED):
        require(len(branches(IDVAR_SET_LOCKED, target)) == 1,
                f"{path}: idvar hardware edge to {target} changed")
    require(len(branches(TRACE_STATE_CHANGED, TRACE_TURN_ON)) == 1 and
            len(branches(TRACE_TURN_ON, TRACE_RESYNC)) == 1 and
            len(branches(TRACE_TURN_ON, PERF_CONFIG_SQ_FULL)) == 1 and
            len(branches(TRACE_RESYNC, TRACE_READ_TICS)) == 1 and
            len(branches(TRACE_READ_TICS, TRACE_READ_GPU_TIME)) == 1,
            f"{path}: debug trace hardware graph changed")
    trace_turn_on = body(TRACE_TURN_ON)
    require(trace_turn_on.count(bytes.fromhex("f6 80 93 11 00 00 20")) == 1,
            f"{path}: trace turn-on TelemetryDisable gate changed")
    gpu_time = body(TRACE_READ_GPU_TIME)
    require(len(branches(TRACE_READ_GPU_TIME, SAFE_FORCE_WAKE)) == 2 and
            gpu_time.count(bytes.fromhex("8b 80 5c 23 00 00")) == 2 and
            gpu_time.count(bytes.fromhex("8b 80 58 23 00 00")) == 1,
            f"{path}: trace timestamp MMIO path changed")
    perf_config = body(PERF_CONFIG_SQ_FULL)
    require(perf_config.count(bytes.fromhex(
        "48 8b 87 40 12 00 00 c7 80 bc 91 00 00 00 00 50 e0")) == 1,
        f"{path}: trace performance-counter MMIO path changed")
    create = body(TELEMETRY_CREATE)
    for anchor, expected, label in (
        (bytes.fromhex("4d 89 a7 88 02 00 00"), 2,
         "performance-config pointer"),
        (bytes.fromhex("49 8d bf e0 02 00 00 ba 38 00 00 00 31 f6"),
         1, "embedded OA state"),
        (bytes.fromhex("49 c7 87 30 02 00 00 01 00 00 00"),
         1, "manager base reference"),
    ):
        require(create.count(anchor) == expected,
                f"{path}: telemetry constructor no longer zero-initializes {label}")

    init = body(MANAGER_INIT)
    for anchor, label in (
        (bytes.fromhex("48 8b 80 40 12 00 00 8b 80 98 59 14 00"),
         "RP_STATE_CAP read"),
        (bytes.fromhex("49 8b 84 24 40 12 00 00 44 89 a8 60 23 00 00"),
         "OACONTROL write"),
    ):
        require(init.count(anchor) == 1,
                f"{path}: native telemetry {label} changed")
    require(len(branches(MANAGER_INIT, SAFE_FORCE_WAKE)) == 4,
            f"{path}: native telemetry force-wake shape changed")
    require(len(branches(MANAGER_INIT, OA_INITIALIZE)) == 1 and
            len(branches(MANAGER_INIT, OA_ENABLE)) == 1,
            f"{path}: native telemetry OA bootstrap changed")

    # Every hardware-facing manager descendant must remain behind one of the
    # exact symbols routed above. This pins the complete direct call graph, not
    # merely the currently observed register constants.
    for owner, target, count in (
        (MANAGER_CALC_GLOBAL, MANAGER_INIT_GLOBAL, 1),
        (MANAGER_OPERATION, MANAGER_PREPARE, 1),
        (MANAGER_OPERATION, MANAGER_SAMPLE, 1),
        (MANAGER_OPERATION, OA_FINALIZE, 1),
        (MANAGER_OPERATION, OA_ENABLE, 1),
        (MANAGER_OPERATION, OA_DISABLE, 1),
        (MANAGER_SAMPLE, MANAGER_READ_EDRAM, 1),
        (MANAGER_STOP, OA_FINALIZE, 1),
        (MANAGER_STOP, MANAGER_RELEASE, 1),
        (MANAGER_INIT_OA, MANAGER_RETAIN, 1),
        (MANAGER_INIT_OA, OA_INITIALIZE, 1),
        (MANAGER_READ_OA, MANAGER_RETAIN, 1),
        (MANAGER_READ_OA, OA_READ, 1),
        (MANAGER_MAP_OA, OA_MAP, 1),
    ):
        require(len(branches(owner, target)) == count,
                f"{path}: telemetry edge {owner} -> {target} changed")
    for target in MANAGER_PATCH_HELPERS:
        require(len(branches(MANAGER_PATCH_CONTEXT, target)) == 1,
                f"{path}: context telemetry edge to {target} changed")

    usage_report = body(USAGE_REPORT_GLOBAL)
    require(usage_report.count(bytes.fromhex(
        "48 8b 87 40 12 00 00 8b 80 48 59 14 00")) == 1,
        f"{path}: usage global-frequency MMIO edge changed")

    trace = body(TRACE_INIT)
    require(trace.count(bytes.fromhex("81 e2 00 00 00 20")) == 1,
            f"{path}: TelemetryDisable trace gate changed")
    for symbol in MANAGER_PATCH_HELPERS:
        candidate = body(symbol)
        gates = sum(candidate.count(bytes.fromhex(anchor)) for anchor in (
            "f6 80 93 11 00 00 20",  # accelerator held in rax
            "f6 87 93 11 00 00 20",  # accelerator held in rdi
        ))
        require(gates == 1,
                f"{path}: {symbol} lost its TelemetryDisable gate")

    require(len(branches(MANAGER_DTOR, OA_FINALIZE)) == 1,
            f"{path}: telemetry destructor no longer finalizes embedded OA state")
    finalizer = body(OA_FINALIZE)
    zero_fast_path = bytes.fromhex(
        "8b 47 34 8b 57 38 89 d1 f7 d9 39 c8 0f 84")
    require(finalizer.startswith(bytes.fromhex("55 48 89 e5 41 56 53 49 89 d6") +
                                 zero_fast_path),
            f"{path}: OA zero-reference destructor fast path changed")

    require(len(branches(USAGE_INIT, USAGE_ALLOC)) == 1,
            f"{path}: usage allocation edge changed")
    dealloc = body(USAGE_DEALLOC)
    for anchor, label in (
        (bytes.fromhex("8b 47 78 48 8b 4f 18 48 8b 89 10 0f 00 00"),
         "GPU allocation accounting"),
        (bytes.fromhex("8b 87 90 00 00 00 48 29 81 48 02 00 00"),
         "metadata allocation accounting"),
        (bytes.fromhex("48 8b 7b 68 48 85 ff 74"), "GPU buffer release guard"),
        (bytes.fromhex("48 8b bb 88 00 00 00 48 85 ff 74"),
         "metadata release guard"),
    ):
        require(dealloc.count(anchor) == 1,
                f"{path}: usage zero-state teardown changed: {label}")


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
    source = pathlib.Path(path).read_text(encoding="utf-8")
    process = function_body(
        source,
        "bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)")
    route_start = process.index("KernelPatcher::RouteRequest telemetryRoutes[]")
    route_end = process.index("Failed to isolate VF telemetry and OA hardware paths",
                              route_start)
    routes = process[route_start:route_end]
    require(process.rfind("if (vfActive)", 0, route_start) >= 0,
            f"{path}: telemetry routes are not VF-only")
    for symbol, wrapper in ROUTES.items():
        require(f'{{"{symbol}",' in routes and wrapper in routes,
                f"{path}: missing VF telemetry route {symbol} -> {wrapper}")

    init = function_body(source, "int Gen11::vfTelemetryInitWithAccelerator(")
    for token in (
        "getMember<void *>(that, 0x258) = accelerator",
        "getMember<void *>(that, 0x2D8) = accelerator",
        "getMember<void *>(that, 0x228) = getMember<void *>(accelerator, 0xE00)",
        "getMember<uint32_t>(that, 0x2A0) = 0",
        "getMember<uint32_t>(that, 0x10) = 1",
        "getMember<uint32_t>(that, 0x0C) = options",
        "getMember<uint64_t>(that, 0x230) = 1",
        "getMember<uint32_t>(that, 0x30C) = 0",
        "getMember<uint32_t>(that, 0x310) = 0",
    ):
        require(token in init, f"{path}: incomplete safe manager init: {token}")
    for forbidden in ("FunctionCast", "0x1240", "SafeForceWake"):
        require(forbidden not in init,
                f"{path}: safe manager init re-enters hardware through {forbidden}")

    usage_alloc = function_body(source, "void Gen11::vfTelemetryUsageAlloc(")
    for offset in ("0x68", "0x70", "0x78", "0x80", "0x88", "0x90"):
        require(offset in usage_alloc,
                f"{path}: usage allocation failure state misses {offset}")
    require("return false;" in function_body(
        source, "bool Gen11::vfTelemetryUsageStartSample("),
        f"{path}: VF usage sampling is not rejected")
    for name in ("vfTelemetryOperation", "vfTelemetryInitOaBuffer",
                 "vfTelemetryReadOaBuffer", "vfTelemetryMapOaBufferMemory"):
        require("kIOReturnUnsupported" in function_body(source, f"Gen11::{name}("),
                f"{path}: {name} does not fail closed")

    sysctl = function_body(source, "void Gen11::vfDisableDebugSysctl(")
    require("(void)that;" in sysctl,
            f"{path}: VF debug sysctl replacement lost its no-op body")
    for forbidden in ("FunctionCast", "getMember", "setProperty", "0x1240"):
        require(forbidden not in sysctl,
                f"{path}: VF debug sysctl replacement is not hardware-free")

    start = function_body(source, "bool Gen11::start(void *that, void *provider)")
    require('service->setProperty("TelemetryDisable", one)' in start and
            "!telemetryDisabled" in start,
            f"{path}: native start does not require the VF telemetry property")
    inject = function_body(
        source, "bool Gen11::injectAcceleratorPersonality(const char *bundleId)")
    for token in (
        'source->getObject("IOGVAH264EncodeCapabilities")',
        "OSDictionary::withDictionary(sourceDevelopment)",
        'development->setObject("TelemetryDisable", telemetryDisabled)',
        'dict->setObject("Development", development)',
        "if (!telemetryReady)",
    ):
        require(token in inject, f"{path}: unsafe shallow personality edit: {token}")


def plist_contract(path):
    with pathlib.Path(path).open("rb") as stream:
        root = plistlib.load(stream)
    development = root["IOKitPersonalities"]["Gen7"]["Development"]
    require(development.get("TelemetryDisable") == 0,
            f"{path}: source PF personality telemetry policy changed")


def main():
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: vf_telemetry_isolation_contract_test.py SOURCE PLIST-A "
            "PLIST-B MACHO-A MACHO-B")
    source_contract(sys.argv[1])
    for path in sys.argv[2:4]:
        plist_contract(path)
    for path in sys.argv[4:]:
        macho_inventory(path)
    print("PASS: Tahoe VF telemetry/OA/debug-sysctl MMIO is isolated with native teardown ABI preserved")


if __name__ == "__main__":
    main()
