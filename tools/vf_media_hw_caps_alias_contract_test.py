#!/usr/bin/env python3
"""Pin the exact Tahoe media hw-caps producer and VF-only identity alias."""

import hashlib
import re
import struct
import sys
from pathlib import Path


MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_SEGMENT_64 = 0x19
GET_HW_CAPS = "__ZN13IGAccelDevice11get_hw_capsEP16_IntelHwCapsInfoS1_yPy"
GET_HW_CAPS_END = "__ZN13IGAccelDevice13debug_controlEP28IntelDeviceDebugControlInOutS1_yPy"
DEVICE_START = "__ZN13IGAccelDevice11deviceStartEv"
DEVICE_START_END = "__ZN13IGAccelDevice12get_wa_tableEP9_WA_TABLES1_yPy"
GET_METHOD = "__ZN13IGAccelDevice26getTargetAndMethodForIndexEPP9IOServicej"
GET_METHOD_END = "__ZN13IGAccelDevice9MetaClassD0Ev"
METHOD_DESCS = "__ZZN13IGAccelDevice11deviceStartEvE11methodDescs"
PROBE = "__ZN16IntelAccelerator5probeEP9IOServicePi"
PROBE_END = "__ZN16IntelAccelerator18encodeFailureStackE15IGFailureReason"
EXTENDED_READ = "__ZN11IOPCIDevice20extendedConfigRead32Ey"
VIDEO_GET_ACCEL_ID = "__ZN19IGAccelVideoContext22get_iosurface_accel_idEP37sIntelVideoMethodArgsGetAcceleratorIdPy"
VIDEO_UPDATE_PERF = "__ZN19IGAccelVideoContext22update_perf_capabilityEP37sIntelVideoMethodArgsPerfCapabilityIny"
VIDEO_SET_PRIORITY = "__ZN19IGAccelVideoContext20set_context_priorityEP39sIntelVideoMethodArgsSetContextPriorityy"
VIDEO_CONTEXT_START = "__ZN19IGAccelVideoContext12contextStartEv"


def parse_macho(data: bytes, label: str):
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != MH_MAGIC_64:
        raise AssertionError(f"{label}: not a little-endian 64-bit Mach-O")
    command_count, command_bytes = struct.unpack_from("<II", data, 16)
    if 32 + command_bytes > len(data):
        raise AssertionError(f"{label}: load commands escape file")
    cursor = 32
    symtab = dysymtab = None
    sections = []
    for _ in range(command_count):
        command, size = struct.unpack_from("<II", data, cursor)
        if size < 8 or cursor + size > 32 + command_bytes:
            raise AssertionError(f"{label}: malformed load command")
        if command == LC_SYMTAB:
            symtab = struct.unpack_from("<IIII", data, cursor + 8)
        elif command == LC_DYSYMTAB:
            dysymtab = struct.unpack_from("<18I", data, cursor + 8)
        elif command == LC_SEGMENT_64:
            section_count = struct.unpack_from("<I", data, cursor + 64)[0]
            section_cursor = cursor + 72
            if section_cursor + section_count * 80 > cursor + size:
                raise AssertionError(f"{label}: section table escapes segment command")
            for _ in range(section_count):
                address, section_size, file_offset = struct.unpack_from(
                    "<QQI", data, section_cursor + 32
                )
                sections.append((address, section_size, file_offset))
                section_cursor += 80
        cursor += size
    if cursor != 32 + command_bytes or not symtab or not dysymtab or not sections:
        raise AssertionError(f"{label}: incomplete Mach-O metadata")

    symbol_offset, symbol_count, string_offset, string_size = symtab
    if symbol_offset + symbol_count * 16 > len(data) or \
            string_offset + string_size > len(data):
        raise AssertionError(f"{label}: symbol table escapes file")
    names = []
    symbols = {}
    for index in range(symbol_count):
        entry = symbol_offset + index * 16
        string_index = struct.unpack_from("<I", data, entry)[0]
        value = struct.unpack_from("<Q", data, entry + 8)[0]
        if string_index >= string_size:
            raise AssertionError(f"{label}: symbol string escapes table")
        end = data.find(b"\0", string_offset + string_index,
                        string_offset + string_size)
        if end < 0:
            raise AssertionError(f"{label}: unterminated symbol")
        name = data[string_offset + string_index:end].decode("utf-8")
        names.append(name)
        if name:
            symbols.setdefault(name, []).append(value)

    relocation_offset, relocation_count = dysymtab[14], dysymtab[15]
    if relocation_offset + relocation_count * 8 > len(data):
        raise AssertionError(f"{label}: relocation table escapes file")
    relocations = {}
    for index in range(relocation_count):
        address, word = struct.unpack_from(
            "<iI", data, relocation_offset + index * 8
        )
        symbol_index = word & 0xFFFFFF
        external = (word >> 27) & 1
        if external:
            if symbol_index >= len(names):
                raise AssertionError(f"{label}: relocation symbol escapes table")
            if address in relocations:
                raise AssertionError(f"{label}: duplicate external relocation")
            relocations[address] = names[symbol_index]
    return symbols, sections, relocations


def single(symbols, name: str, label: str) -> int:
    values = symbols.get(name, [])
    if len(values) != 1:
        raise AssertionError(f"{label}: {name} is absent or non-unique")
    return values[0]


def vm_slice(data: bytes, sections, start: int, end: int, label: str) -> bytes:
    if end <= start:
        raise AssertionError(f"{label}: inverted VM range")
    for address, size, file_offset in sections:
        if address <= start and end <= address + size:
            begin = file_offset + start - address
            finish = file_offset + end - address
            if finish > len(data):
                raise AssertionError(f"{label}: VM range escapes file")
            return data[begin:finish]
    raise AssertionError(f"{label}: VM range is not in one section")


def verify_payload(data: bytes, label: str) -> None:
    symbols, sections, relocations = parse_macho(data, label)
    values = {
        name: single(symbols, name, label)
        for name in (
            GET_HW_CAPS, GET_HW_CAPS_END, DEVICE_START, DEVICE_START_END,
            GET_METHOD, GET_METHOD_END, METHOD_DESCS, PROBE, PROBE_END,
            VIDEO_GET_ACCEL_ID, VIDEO_UPDATE_PERF, VIDEO_SET_PRIORITY,
            VIDEO_CONTEXT_START,
        )
    }
    expected_addresses = {
        GET_HW_CAPS: 0x9CDE,
        GET_HW_CAPS_END: 0x9D78,
        DEVICE_START: 0x9C72,
        DEVICE_START_END: 0x9CB2,
        GET_METHOD: 0xA06A,
        GET_METHOD_END: 0xA0A2,
        METHOD_DESCS: 0xCB8A0,
        PROBE: 0x238E2,
        PROBE_END: 0x23C94,
        VIDEO_GET_ACCEL_ID: 0x7712A,
        VIDEO_UPDATE_PERF: 0x77142,
        VIDEO_SET_PRIORITY: 0x771AE,
        VIDEO_CONTEXT_START: 0x771C2,
    }
    if values != expected_addresses:
        raise AssertionError(f"{label}: changed media user-client symbol layout")

    device_start = vm_slice(
        data, sections, values[DEVICE_START], values[DEVICE_START_END], label
    )
    get_hw_caps = vm_slice(
        data, sections, values[GET_HW_CAPS], values[GET_HW_CAPS_END], label
    )
    get_method = vm_slice(
        data, sections, values[GET_METHOD], values[GET_METHOD_END], label
    )
    probe = vm_slice(data, sections, values[PROBE], values[PROBE_END], label)
    expected_hashes = (
        (device_start, "486048ebe9b0eab20c62ca2b4bacd2520748feedeab140f6fd0b963badacfbb6"),
        (get_hw_caps, "8ef5140043120f2f556960383d9b8f857394bb74bdef4b04853c7de0651a26ee"),
        (get_method, "bc7f5879c19ba320b7aad6b695c0fa46b1c72d01455bb5c3bacf5ec505b450ed"),
        (probe, "0676689ac7b27611fa8a6ef612d41c591a3010b5694a261938fe48342fbc35b7"),
    )
    for body, expected in expected_hashes:
        if hashlib.sha256(body).hexdigest() != expected:
            raise AssertionError(f"{label}: changed pinned producer body")

    video_methods = (
        (VIDEO_GET_ACCEL_ID, VIDEO_UPDATE_PERF,
         "c89b5778b54a4d03c497ec73fcf2ff80243860e8abc2eb39d6497232c054dd30"),
        (VIDEO_UPDATE_PERF, VIDEO_SET_PRIORITY,
         "b3bd53a52c19d92c4b8a0959957fa69eb3029b0d959a45f74be86a5b3f88b6aa"),
        (VIDEO_SET_PRIORITY, VIDEO_CONTEXT_START,
         "cd2c386e7d65969f73ebeabac67c223c64d0287cd38f42d887fdf10daa4912e1"),
    )
    for method, following, expected in video_methods:
        body = vm_slice(
            data, sections, values[method], values[following], label
        )
        if hashlib.sha256(body).hexdigest() != expected:
            raise AssertionError(f"{label}: changed pinned video selector body {method}")

    if b"\x48\x89\x83\x88\x01\x00\x00" not in device_start:
        raise AssertionError(f"{label}: device no longer stores local method table at +0x188")
    if get_method != bytes.fromhex(
        "554889e548893e83fa09770e488b05a3e00b005dffa05009000083fa18771483"
        "c2f6488d045248c1e00448038788010000eb0231c05dc390"
    ):
        raise AssertionError(f"{label}: selector 0xa..0x18 method-table dispatch changed")

    descriptors = vm_slice(
        data, sections, values[METHOD_DESCS], values[METHOD_DESCS] + 25 * 48,
        label,
    )
    if hashlib.sha256(descriptors).hexdigest() != \
            "3bd63462baecef3014d59e3cfa8b78d830b6922e8a2723e1d6330f2d129f2978":
        raise AssertionError(f"{label}: changed complete IGAccelDevice method table")
    selector_b = struct.unpack_from("<6Q", descriptors, 48)
    if selector_b != (0, values[GET_HW_CAPS], 0, 3, 0xFFFFFFFF, 0xFFFFFFFF):
        raise AssertionError(f"{label}: selector 0xb no longer targets get_hw_caps")

    if not get_hw_caps.startswith(bytes.fromhex(
            "554889e54156534989d64889fb49c7007c000000")):
        raise AssertionError(f"{label}: get_hw_caps no longer publishes 0x7c bytes")
    copy_contract = bytes.fromhex(
        "4881c610110000b91f0000004c89f7f3a531c0"
    )
    if get_hw_caps.count(copy_contract) != 1:
        raise AssertionError(
            f"{label}: get_hw_caps no longer copies 31 dwords from accelerator+0x1110"
        )

    producer = bytes.fromhex(
        "4889c3be020000004889c7e80000000041898618110000"
    )
    if probe.count(producer) != 1:
        raise AssertionError(
            f"{label}: probe no longer stores extendedConfigRead32(2) at accelerator+0x1118"
        )
    relocation = values[PROBE] + probe.index(producer) + 12
    if relocation != 0x2399E or relocations.get(relocation) != EXTENDED_READ:
        raise AssertionError(f"{label}: PCI identity producer relocation changed")
    native_identity = bytes.fromhex(
        "8b3e81ffee beafde7f1581ff8680409a742d".replace(" ", "")
    )
    if probe.count(native_identity) != 1:
        raise AssertionError(f"{label}: canonical native TGL probe identity changed")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
    raise AssertionError(f"unterminated function: {signature}")


def verify_source(source: str, header: str, label: str) -> None:
    virtual_identity = function_body(source, "bool ngVirtualGpuAccessAllowed()")
    expected_virtual_identity = (
        "bool ngVirtualGpuAccessAllowed()\n"
        "{\n"
        "\treturn vfIdentifyDevice() == VfIdentity::Virtual;\n"
        "}"
    )
    if virtual_identity != expected_virtual_identity:
        raise AssertionError(
            f"{label}: userspace bridge VF admission is not positive Virtual identity"
        )

    route = (
        '{"__ZN13IGAccelDevice11get_hw_capsEP16_IntelHwCapsInfoS1_yPy",\n'
        "\t\t\t vfGetHwCaps, this->oVfGetHwCaps},"
    )
    if source.count(route) != 1:
        raise AssertionError(f"{label}: missing or duplicate exact get_hw_caps route")
    route_at = source.index(route)
    vf_branch = source.rfind("\t\tif (vfActive) {", 0, route_at)
    rollback = source.rfind(
        "KernelPatcher::SolveRequest pageTableRollback[]", 0, route_at
    )
    requests = source.rfind("KernelPatcher::RouteRequest requests[]", 0, route_at)
    routed = source.index("patcher.routeMultiple(index, requests", route_at)
    if not 0 <= vf_branch < rollback < requests < route_at < routed:
        raise AssertionError(f"{label}: media alias route escaped VF-only route block")

    video_routes = (
        (VIDEO_GET_ACCEL_ID, "vfVideoGetIosurfaceAccelId",
         "oVfVideoGetIosurfaceAccelId"),
        (VIDEO_UPDATE_PERF, "vfVideoUpdatePerfCapability",
         "oVfVideoUpdatePerfCapability"),
        (VIDEO_SET_PRIORITY, "vfVideoSetContextPriority",
         "oVfVideoSetContextPriority"),
    )
    route_positions = []
    for symbol, wrapper, original in video_routes:
        token = '{"' + symbol + '",\n\t\t\t ' + wrapper + ", this->" + original + "},"
        if source.count(token) != 1:
            raise AssertionError(f"{label}: missing or duplicate video trace route {symbol}")
        route_positions.append(source.index(token))
    if not route_at < route_positions[0] < route_positions[1] < \
            route_positions[2] < routed:
        raise AssertionError(f"{label}: video trace routes escaped VF-only route table")

    expected_header = (
        "static IOReturn vfGetHwCaps(void *that, void *input, void *output,\n"
        "\t                           uint64_t inputSize, uint64_t *outputSize);\n"
        "\tmach_vm_address_t oVfGetHwCaps {};"
    )
    if header.count(expected_header) != 1:
        raise AssertionError(f"{label}: changed media alias ABI/original storage")

    header_contracts = (
        ("static IOReturn vfVideoGetIosurfaceAccelId(void *that, void *arguments,\n"
         "\t                                           uint64_t *outputSize);\n"
         "\tmach_vm_address_t oVfVideoGetIosurfaceAccelId {};"),
        ("static IOReturn vfVideoUpdatePerfCapability(void *that, void *arguments,\n"
         "\t                                            uint64_t inputSize);\n"
         "\tmach_vm_address_t oVfVideoUpdatePerfCapability {};"),
        ("static IOReturn vfVideoSetContextPriority(void *that, void *arguments,\n"
         "\t                                         uint64_t inputSize);\n"
         "\tmach_vm_address_t oVfVideoSetContextPriority {};"),
    )
    for contract in header_contracts:
        if header.count(contract) != 1:
            raise AssertionError(f"{label}: changed video trace ABI/original storage")

    body = function_body(source, "IOReturn Gen11::vfGetHwCaps(")
    required = (
        "FunctionCast(\n\t\tvfGetHwCaps, callback->oVfGetHwCaps)(",
        "if (result != kIOReturnSuccess)",
        "constexpr uint64_t kHwCapsSize = 0x7C;",
        "constexpr uint32_t kTigerLakeVfIdentity = 0x9A498086U;",
        "constexpr uint32_t kCanonicalTigerLakeIdentity = 0x9A408086U;",
        "if (!output || !outputSize || *outputSize != kHwCapsSize)",
        "if (words[2] != kTigerLakeVfIdentity)",
        "words[2] = kCanonicalTigerLakeIdentity;",
        "V340: exposed canonical TGL media hw-caps identity",
    )
    positions = []
    for token in required:
        if body.count(token) != 1:
            raise AssertionError(f"{label}: changed source contract: {token}")
        positions.append(body.index(token))
    if positions != sorted(positions):
        raise AssertionError(f"{label}: native/shape/identity/alias ordering changed")
    if body.count("return kIOReturnUnsupported;") != 2:
        raise AssertionError(f"{label}: shape and identity must both fail closed")
    if "vfIdentifyDevice" in body or "getOriginalDeviceId" in body:
        raise AssertionError(f"{label}: wrapper must rely on VF-only route installation")

    if source.count("NGVfRuntimePatch::spoofedSkuFind, r3") != 1 or \
            "0x81, 0xff, 0x86, 0x80, 0x49, 0x9a, 0x74, 0x2d" not in source:
        raise AssertionError(f"{label}: exact 0x9a40-to-0x9a49 kernel probe patch changed")

    wrapper_contracts = (
        ("IOReturn Gen11::vfVideoGetIosurfaceAccelId(",
         "callback->oVfVideoGetIosurfaceAccelId)(that, arguments, outputSize);",
         "V337: media selector 0x100 enter",
         "V341: media selector 0x100 return"),
        ("IOReturn Gen11::vfVideoUpdatePerfCapability(",
         "callback->oVfVideoUpdatePerfCapability)(that, arguments, inputSize);",
         "V337: media selector 0x101 enter",
         "V337: media selector 0x101 return"),
        ("IOReturn Gen11::vfVideoSetContextPriority(",
         "callback->oVfVideoSetContextPriority)(that, arguments, inputSize);",
         "V337: media selector 0x102 enter",
         "V337: media selector 0x102 return"),
    )
    for signature, native, enter_marker, return_marker in wrapper_contracts:
        wrapper = function_body(source, signature)
        for token in (native, enter_marker, return_marker, "return result;"):
            if wrapper.count(token) != 1:
                raise AssertionError(
                    f"{label}: changed observation-only selector wrapper: {token}"
                )
        if wrapper.index(enter_marker) > wrapper.index(native) or \
                wrapper.index(native) > wrapper.index(return_marker):
            raise AssertionError(f"{label}: changed selector enter/native/return ordering")
        if wrapper.count("FunctionCast(") != 1:
            raise AssertionError(f"{label}: selector wrapper must call native exactly once")
        for forbidden in (
                "vfMarkProtocolFault", "kIOReturnUnsupported", "getMember<",
                "vfIdentifyDevice", "gVfAccelerator"):
            if forbidden in wrapper:
                raise AssertionError(
                    f"{label}: observation wrapper changes behavior via {forbidden}"
                )

    accel_wrapper = function_body(
        source, "IOReturn Gen11::vfVideoGetIosurfaceAccelId("
    )
    accel_observation = (
        "const uint64_t observedOutputSize = outputSize ? *outputSize : 0;",
        "const bool readable = result == kIOReturnSuccess && arguments != nullptr;",
        "const uint32_t acceleratorId = readable ?",
        "*static_cast<const uint32_t *>(arguments) : 0;",
        "output-size=%llu accel-id=%u readable=%u",
    )
    observation_positions = []
    for token in accel_observation:
        if accel_wrapper.count(token) != 1:
            raise AssertionError(
                f"{label}: changed selector 0x100 result observation: {token}"
            )
        observation_positions.append(accel_wrapper.index(token))
    if observation_positions != sorted(observation_positions):
        raise AssertionError(
            f"{label}: selector 0x100 observation escaped post-native order"
        )


V342_APPLEGVA_PATCHES = (
    ("v342ScalerCount", "e81fbdfbff418b4f2831d2",
     "e81fbdfbff418b4f2031d2", 0x7C0F2),
    ("v342ScalerFirstMatch", "413944d72c", "413944d724", 0x7C102),
    ("v342ScalerFirstLoad", "4b8b44f72c", "4b8b44f724", 0x7C13C),
    ("v342ScalerSecondLoad",
     "49894528498b87300200004989453049c74538000000004983bf3004000000",
     "49894528498b87280200004989453049c74538000000004983bf3004000000",
     0x7C141),
    ("v342ScalerLinkCheck", "4983bf3004000000", "4983bf2804000000",
     0x7C158),
    ("v342ScalerLinkLoad", "498b8f30040000", "498b8f28040000",
     0x7C172),
    ("v342ScalerLinkedFirst", "f2430f1044f72c", "f2430f1044f724",
     0x7C1A7),
    ("v342ScalerLinkedSecond", "498b8f30020000", "498b8f28020000",
     0x7C1BD),
    ("v342EncoderFirstLoad", "f20f10482c", "f20f104824", 0x1BCA),
    ("v342EncoderSecondLoad", "488b803002000048898320f11e00",
     "488b802802000048898320f11e00", 0x1BD9),
    ("v342CapabilitySearchLink", "4c8b89300400004d85c9740641833910",
     "4c8b89280400004d85c9740641833910", 0x38349),
    ("v342FrameStatLinkCheck",
     "c783b49a2300050000004489a3a89a23004531ff4883b830040000000f8418ffffff",
     "c783b49a2300050000004489a3a89a23004531ff4883b828040000000f8418ffffff",
     0x1DB8),
    ("v342FrameStatLinkLoad",
     "488b8bf89b2300488b89300400008b118910488b5108",
     "488b8bf89b2300488b89280400008b118910488b5108", 0x1DED),
)


def cpp_byte_array(source: str, name: str, label: str) -> bytes:
    pattern = re.compile(
        rf"static const uint8_t {re.escape(name)}\[\] = \{{(.*?)\n\}};",
        re.DOTALL,
    )
    matches = pattern.findall(source)
    if len(matches) != 1:
        raise AssertionError(f"{label}: missing or duplicate byte array {name}")
    return bytes(int(value, 16) for value in re.findall(
        r"0x([0-9A-Fa-f]{2})", matches[0]
    ))


def verify_v342_user_bridge(source: str, label: str) -> None:
    for name, expected_find, expected_replace, _ in V342_APPLEGVA_PATCHES:
        find = cpp_byte_array(source, name + "Find", label)
        replace = cpp_byte_array(source, name + "Replace", label)
        if find != bytes.fromhex(expected_find) or \
                replace != bytes.fromhex(expected_replace):
            raise AssertionError(f"{label}: changed exact AppleGVA anchor {name}")
        differences = [
            index for index, pair in enumerate(zip(find, replace))
            if pair[0] != pair[1]
        ]
        if len(find) != len(replace) or len(differences) != 1:
            raise AssertionError(f"{label}: {name} is not a one-byte displacement patch")
        old, new = find[differences[0]], replace[differences[0]]
        if (old, new) not in ((0x2C, 0x24), (0x30, 0x28), (0x28, 0x20)):
            raise AssertionError(f"{label}: {name} changed a non-layout byte")

    table_start = source.index(
        "static UserPatcher::BinaryModPatch v342AppleGvaPatches[] = {"
    )
    table_end = source.index("\n};", table_start)
    table = source[table_start:table_end]
    expected_names = [item[0] for item in V342_APPLEGVA_PATCHES]
    table_names = re.findall(r"V342_LOCAL_PATCH\((v342\w+)\)", table)
    if table_names != expected_names:
        raise AssertionError(f"{label}: changed ordered 13-site AppleGVA patch table")

    macro = (
        "{CPU_TYPE_X86_64, UserPatcher::LocalOnly, name##Find, name##Replace, \\\n"
        "\t arrsize(name##Find), 0, 1, UserPatcher::SegmentTextText, "
        "V342AppleGvaSection}"
    )
    if source.count(macro) != 1:
        raise AssertionError(f"{label}: AppleGVA patches are not exact local-only text patches")

    apple_gva = (
        "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"
    )
    encoder = (
        "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/"
        "VTEncoderXPCService.xpc/Contents/MacOS/VTEncoderXPCService"
    )
    decoder = (
        "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/"
        "VTDecoderXPCService.xpc/Contents/MacOS/VTDecoderXPCService"
    )
    if source.count('"' + apple_gva + '"') != 1:
        raise AssertionError(f"{label}: AppleGVA binary path changed")
    for process in (encoder, decoder):
        if source.count('"' + process + '"') != 2:
            raise AssertionError(f"{label}: canonical VideoToolbox process suffix changed")
    if source.count(
            "V342AppleGvaSection, UserPatcher::ProcInfo::MatchSuffix}") != 2:
        raise AssertionError(f"{label}: media process admission is not canonical-suffix only")

    register = function_body(source, "void registerV342AppleGvaBridge()")
    for token in (
        "lilu.onProcLoadForce(",
        "v342MediaProcesses, arrsize(v342MediaProcesses)",
        "&v342AppleGvaBinary, 1",
        "V344: dispatched suffix-qualified local AppleGVA TGL capability-layout bridge path-len=%lu path=%s",
        "pathLength, path",
        "V342: armed exact Tahoe AppleGVA TGL capability-layout bridge with 13 local-only sites",
    ):
        if register.count(token) != 1:
            raise AssertionError(f"{label}: changed user-patcher registration: {token}")

    patcher = function_body(source, "void NGreen::processPatcher(KernelPatcher &patcher)")
    exact_gate = (
        "const bool exactTigerLakeVf = virtualAccess && this->deviceId == 0xA7A8 &&\n"
        "\t\tWIOKit::getOSDataValue(this->iGPU, \"device-id\", compatibilityDeviceId) &&\n"
        "\t\tcompatibilityDeviceId == 0x9A49;"
    )
    for token in (
        "const bool virtualAccess = ngVirtualGpuAccessAllowed();",
        "uint32_t compatibilityDeviceId = 0;",
        exact_gate,
        "if (exactTigerLakeVf) {",
        "V343: classified AppleGVA bridge VF physical=a7a8 compatibility=9a49",
        "registerV342AppleGvaBridge();",
    ):
        if patcher.count(token) != 1:
            raise AssertionError(f"{label}: AppleGVA bridge escaped exact dual-identity VF gate: {token}")
    gate_order = tuple(patcher.index(token) for token in (
        "const bool virtualAccess = ngVirtualGpuAccessAllowed();",
        "uint32_t compatibilityDeviceId = 0;",
        exact_gate,
        "if (exactTigerLakeVf) {",
        "registerV342AppleGvaBridge();",
    ))
    if gate_order != tuple(sorted(gate_order)):
        raise AssertionError(f"{label}: AppleGVA dual-identity VF gate ordering changed")
    init = function_body(source, "void NGreen::init()")
    if "registerV342AppleGvaBridge" in init:
        raise AssertionError(f"{label}: AppleGVA bridge registered before PF/VF classification")


def replace_once(value: str, before: str, after: str) -> str:
    if value.count(before) != 1:
        raise AssertionError(f"ambiguous mutation anchor: {before}")
    return value.replace(before, after, 1)


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: vf_media_hw_caps_alias_contract_test.py "
            "kern_gen11.cpp kern_gen11.hpp kern_green.cpp "
            "accelerator-a accelerator-b"
        )
    source_path, header_path, green_path, *payload_paths = map(Path, sys.argv[1:])
    source = source_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    green = green_path.read_text(encoding="utf-8")
    payloads = [(path.read_bytes(), str(path)) for path in payload_paths]
    for data, label in payloads:
        verify_payload(data, label)
    verify_source(source, header, f"{source_path}/{header_path}")
    verify_v342_user_bridge(green, str(green_path))

    source_mutations = (
        replace_once(
            source,
            "return vfIdentifyDevice() == VfIdentity::Virtual;",
            "return vfIdentifyDevice() != VfIdentity::Physical;",
        ),
        replace_once(source, "0x9A498086U", "0x9A488086U"),
        replace_once(source, "0x9A408086U", "0x9A488086U"),
        replace_once(source, "kHwCapsSize = 0x7C", "kHwCapsSize = 0x80"),
        replace_once(source, "words[2] != kTigerLakeVfIdentity",
                     "words[1] != kTigerLakeVfIdentity"),
        replace_once(source, "words[2] = kCanonicalTigerLakeIdentity",
                     "words[1] = kCanonicalTigerLakeIdentity"),
        replace_once(source, "result != kIOReturnSuccess",
                     "result == kIOReturnSuccess"),
        replace_once(
            source,
            "return kIOReturnUnsupported;\n\t}\n\n\tauto *words",
            "return kIOReturnSuccess;\n\t}\n\n\tauto *words",
        ),
        replace_once(source, GET_HW_CAPS, GET_HW_CAPS + "_changed"),
        replace_once(source, "NGVfRuntimePatch::spoofedSkuFind, r3",
                     "NGVfRuntimePatch::spoofedSkuFind, r2"),
        replace_once(source, VIDEO_GET_ACCEL_ID, VIDEO_GET_ACCEL_ID + "_changed"),
        replace_once(source, VIDEO_UPDATE_PERF, VIDEO_UPDATE_PERF + "_changed"),
        replace_once(source, VIDEO_SET_PRIORITY, VIDEO_SET_PRIORITY + "_changed"),
        replace_once(source, "V341: media selector 0x100 return",
                     "V341: media selector 0x100 exit"),
        replace_once(source, "V337: media selector 0x101 return",
                     "V337: media selector 0x101 exit"),
        replace_once(source, "V337: media selector 0x102 return",
                     "V337: media selector 0x102 exit"),
        replace_once(source,
                     "result == kIOReturnSuccess && arguments != nullptr",
                     "result != kIOReturnSuccess && arguments != nullptr"),
        replace_once(source, "*static_cast<const uint32_t *>(arguments)",
                     "*static_cast<const uint16_t *>(arguments)"),
    )
    for index, mutation in enumerate(source_mutations):
        try:
            verify_source(mutation, header, f"source-mutation-{index}")
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"escaped source mutation {index}")
    header_mutation = replace_once(
        header, "mach_vm_address_t oVfGetHwCaps {};",
        "mach_vm_address_t oVfGetHwCapsChanged {};",
    )
    try:
        verify_source(source, header_mutation, "header-mutation")
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("escaped header mutation")

    green_mutations = (
        replace_once(green, "UserPatcher::LocalOnly", "0"),
        replace_once(green, "this->deviceId == 0xA7A8",
                     "this->deviceId == 0xA7A9"),
        replace_once(green, "compatibilityDeviceId == 0x9A49",
                     "compatibilityDeviceId == 0x9A40"),
        replace_once(green, "virtualAccess && this->deviceId",
                     "!physicalAccess && this->deviceId"),
        replace_once(green, "&v342AppleGvaBinary, 1",
                     "&v342AppleGvaBinary, 0"),
        replace_once(green,
                     "V342_LOCAL_PATCH(v342CapabilitySearchLink)",
                     "V342_LOCAL_PATCH(v342ScalerLinkLoad)"),
        replace_once(green,
                     "V342: armed exact Tahoe AppleGVA TGL capability-layout bridge with 13 local-only sites",
                     "V342: armed exact Tahoe AppleGVA TGL capability-layout bridge with 12 local-only sites"),
        replace_once(green,
                     "0xE8, 0x1F, 0xBD, 0xFB, 0xFF, 0x41, 0x8B, 0x4F, 0x28, 0x31, 0xD2",
                     "0xE8, 0x1F, 0xBD, 0xFB, 0xFF, 0x41, 0x8B, 0x4F, 0x20, 0x31, 0xD2"),
        replace_once(green,
                     "V342AppleGvaSection, UserPatcher::ProcInfo::MatchSuffix},\n\t{\"/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/VTDecoderXPCService",
                     "V342AppleGvaSection, UserPatcher::ProcInfo::MatchExact},\n\t{\"/System/Library/Frameworks/VideoToolbox.framework/Versions/A/XPCServices/VTDecoderXPCService"),
        replace_once(green,
                     "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA",
                     "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/B/AppleGVA"),
    )
    for index, mutation in enumerate(green_mutations):
        try:
            verify_v342_user_bridge(mutation, f"green-mutation-{index}")
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"escaped V342 user-bridge mutation {index}")

    payload_offsets = (
        0x9CEE, 0x9D29, 0x9C98, 0xA079, 0xCB8D8, 0x2399E,
        0x7712A, 0x77142, 0x771AE,
    )
    for data, label in payloads:
        for index, offset in enumerate(payload_offsets):
            changed = bytearray(data)
            changed[offset] ^= 1
            try:
                verify_payload(bytes(changed), f"{label}-mutation-{index}")
            except AssertionError:
                continue
            raise AssertionError(f"{label}: escaped payload mutation {index}")

    print(
        "PASS: exact selector 0xb PCI identity producer, VF-only media alias, "
        "13-site process-local TGL capability bridge, 27 source and "
        "eighteen payload mutations"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
