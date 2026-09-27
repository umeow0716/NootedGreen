#!/usr/bin/env python3
"""Prove that the VF engine wrapper preserves Apple's IOAccel lifecycle tail."""

import pathlib
import struct
import sys


ENABLE = "__ZN22IOGraphicsAccelerator217enableAcceleratorEv"
DISABLE = "__ZN22IOGraphicsAccelerator218disableAcceleratorEv"
START = "__ZN16IntelAccelerator19startGraphicsEngineEv"
STOP = "__ZN16IntelAccelerator18stopGraphicsEngineEv"
BRIDGE_ENABLE = "__ZN17IGInterruptBridge6enableEv"
BRIDGE_DISABLE = "__ZN17IGInterruptBridge7disableEv"
EVENT_INIT = "__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent"
SCHEDULER_ENABLE = "__ZN12IGScheduler416enableInterruptsEv"
SCHEDULER_DISABLE = "__ZN12IGScheduler417disableInterruptsEv"
SCHEDULER_ERROR_ENABLE = "__ZN12IGScheduler421enableErrorInterruptsEv"
SCHEDULER_ERROR_DISABLE = "__ZN12IGScheduler422disableErrorInterruptsEv"
STREAMER_ERROR_ENABLE = "__ZN26IGHardwareCommandStreamer420enableErrorInterruptEv"
STREAMER_ERROR_DISABLE = "__ZN26IGHardwareCommandStreamer421disableErrorInterruptEv"
PCI_CONFIGURE_INTERRUPTS = "__ZN11IOPCIDevice19configureInterruptsEjjjj"
SCHEDULER_INIT_FIRMWARE = "__ZN11IGScheduler12initFirmwareEv"
REQUEST_ENABLE_CALLBACK = "__ZN17IGInterruptBridge21requestEnableCallbackEP8OSObjectPFvS1_zE"
GUC_INIT_INTERRUPTS = "__ZN13IGHardwareGuC14initInterruptsEv"


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
    )
    for token in required:
        if token not in source:
            raise AssertionError(f"{path}: missing lifecycle token {token}")

    pci_resolution = function_body(
        source,
        "bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)")
    if "index == kextIOPCIFamily.loadIndex" not in pci_resolution:
        raise AssertionError(f"{path}: PCI allocator is not resolved from IOPCIFamily")
    if ("KernelPatcher::KernelID" in pci_resolution and
            pci_resolution.index("KernelPatcher::KernelID") <
            pci_resolution.index(PCI_CONFIGURE_INTERRUPTS)):
        raise AssertionError(f"{path}: IOPCIFamily symbol is incorrectly resolved from KernelID")

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
    print(f"PASS: VF wrapper preserves native bridge/IOAccel lifecycle in {path}")


def main():
    if len(sys.argv) != 4:
        raise SystemExit(f"usage: {sys.argv[0]} kern_gen11.cpp TGL-production TGL-debug")
    source_contract(sys.argv[1])
    macho_inventory(sys.argv[2])
    macho_inventory(sys.argv[3])


if __name__ == "__main__":
    main()
