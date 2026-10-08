#!/usr/bin/env python3
"""Pin the PF/VF PCI Bus Master admission boundary and reject regressions."""

import pathlib
import sys


def function_body(source, signature):
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


def contract(green, green_header, gen11, label):
    patcher = function_body(green, "void NGreen::processPatcher(KernelPatcher &patcher)")
    pci_id = patcher.index("this->deviceId = WIOKit::readPCIConfigValue(")
    sriov = patcher.index("NGGpuCapabilities::sriov(this->deviceId)", pci_id)
    early_stop = patcher.index("this->iGPU->setBusMasterEnable(false);", sriov)
    memory = patcher.index("this->iGPU->setMemoryEnable(true);", early_stop)
    identity = patcher.index(
        "const bool physicalAccess = ngPhysicalGpuAccessAllowed();", memory)
    identity_state = patcher.index(
        "this->iGPU->setBusMasterEnable(physicalAccess);", identity)
    barrier = patcher.index("OSSynchronizeIO();", identity_state)
    readback = patcher.index(
        "this->iGPU->configRead16(kIOPCIConfigCommand)", barrier)
    check = patcher.index("busMasterEnabled != physicalAccess", readback)
    routes = patcher.index("KernelPatcher::routeVirtual(this->iGPU", check)
    sink_route = patcher.index("kSetBusMasterEnableVirtualOffset", routes)
    sink_original = patcher.index("&orgSetBusMasterEnable", sink_route)
    if not pci_id < sriov < early_stop < memory < identity < identity_state < \
            barrier < readback < check < routes < sink_route < sink_original:
        raise AssertionError(f"{label}: PF/VF PCI identity/command ordering changed")
    if "sriovCapability != NGGpuCapabilities::Sriov::Absent" not in \
            patcher[sriov:memory]:
        raise AssertionError(f"{label}: an SR-IOV-capable/unknown function can reach BAR0 with Bus Master on")
    if "setBusMasterEnable(true)" in patcher or \
            patcher.count("setBusMasterEnable(") != 2:
        raise AssertionError(f"{label}: processPatcher gained an unconditional Bus Master transition")
    if "kSetBusMasterEnableVirtualOffset = 0x11B" not in green_header:
        raise AssertionError(f"{label}: pinned Tahoe IOPCIDevice Bus Master vtable slot changed")
    for token in (
            "OSCompareAndSwap(0, 1, &vfBusMasterAdmission)",
            "OSCompareAndSwap(1, 0, &vfBusMasterAdmission)",
            "return vfBusMasterAdmission != 0"):
        if token not in green_header:
            raise AssertionError(f"{label}: VF Bus Master admission state lost {token}")

    sink = function_body(green, "bool NGreen::setBusMasterEnable(")
    if "if (device != owner->iGPU || ngPhysicalGpuAccessAllowed() || !enable ||\n\t    owner->vfBusMasterAdmissionOpen())" not in sink:
        raise AssertionError(f"{label}: exact VF Bus Master sink admission predicate changed")
    target = sink.index("device != owner->iGPU")
    physical = sink.index("ngPhysicalGpuAccessAllowed()", target)
    enable = sink.index("!enable", physical)
    admission = sink.index("owner->vfBusMasterAdmissionOpen()", enable)
    passthrough = sink.index("owner->orgSetBusMasterEnable(device, enable)", admission)
    previous = sink.index("device->configRead16(kIOPCIConfigCommand)", passthrough)
    close = sink.index("owner->orgSetBusMasterEnable(device, false)", previous)
    close_barrier = sink.index("OSSynchronizeIO()", close)
    close_readback = sink.index("device->configRead16(kIOPCIConfigCommand)", close_barrier)
    count = sink.index("OSIncrementAtomic(&owner->vfEarlyBusMasterSuppressions)", close_readback)
    marker = sink.index("V322: suppressed premature VF PCI Bus Master", count)
    result = sink.index("return previous", marker)
    if not target < physical < enable < admission < passthrough < previous < \
            close < close_barrier < close_readback < count < marker < result:
        raise AssertionError(f"{label}: identity-scoped Bus Master sink ordering changed")
    if sink.count("orgSetBusMasterEnable(device, enable)") != 1 or \
            sink.count("orgSetBusMasterEnable(device, false)") != 1:
        raise AssertionError(f"{label}: VF sink no longer has one pass-through and one fail-close call")

    start = function_body(gen11, "bool Gen11::start(void *that, void *provider)")
    classified = start.index("const bool vfActive = identity == VfIdentity::Virtual")
    provider = start.index(
        "IOPCIDevice, reinterpret_cast<OSObject *>(provider)", classified)
    close_admission = start.index(
        "NGreen::callback->closeVfBusMasterAdmission()", provider)
    close_check = start.index(
        "Cannot close stale VF PCI Bus Master admission", close_admission)
    stop = start.index("pciDevice->setBusMasterEnable(false)", close_check)
    start_barrier = start.index("OSSynchronizeIO()", stop)
    start_readback = start.index(
        "pciDevice->configRead16(kIOPCIConfigCommand)", start_barrier)
    start_check = start.index(
        "VF PCI bus mastering remained enabled before native start", start_readback)
    owner = start.index("gVfAccelerator = that", start_check)
    bootstrap = start.index("vfBootstrapDirectGgtt()", owner)
    quiesce = start.index("vfQuiesceVirtualInterruptsBeforeMsi()", bootstrap)
    native = start.index("FunctionCast(start, callback->ostart)", quiesce)
    if not classified < provider < close_admission < close_check < stop < \
            start_barrier < start_readback < \
            start_check < owner < bootstrap < quiesce < native:
        raise AssertionError(f"{label}: VF Bus Master stop escaped the pre-native boundary")
    if start.count("setBusMasterEnable(") != 1 or \
            "setBusMasterEnable(true)" in start:
        raise AssertionError(f"{label}: pre-native wrapper gained another Bus Master transition")
    if "ioPciConfigureInterrupts" in start:
        raise AssertionError(f"{label}: MSI allocation escaped into pre-HWS native start")

    engine = function_body(gen11, "bool Gen11::startGraphicsEngine(void *that)")
    mask = engine.index(
        "const uint64_t engineMask = getMember<uint64_t>(that, 0x1300)")
    mask_validation = engine.index(
        "engineMask == 0 || (engineMask & ~0x3FULL) != 0", mask)
    hws = engine.index("for (size_t index = 0; index < 6; index++)")
    global_hws = engine.index("getMember<OSObject *>(that, 0x1438)", hws)
    precheck = engine.index("VF PCI bus mastering escaped", global_hws)
    open_admission = engine.index(
        "NGreen::callback->openVfBusMasterAdmission()", precheck)
    open_marker = engine.index(
        "V322: opened VF PCI Bus Master admission", open_admission)
    configure = engine.index("callback->ioPciConfigureInterrupts)(", open_marker)
    failed_configuration_close = engine.index(
        "NGreen::callback->closeVfBusMasterAdmission()", configure)
    failed_configuration_revoke = engine.index(
        "provider->setBusMasterEnable(false)", failed_configuration_close)
    register_source = engine.index(
        "callback->oVfCreateFilterInterruptEventSource)(interruptBridge)",
        failed_configuration_revoke)
    failed_admission_close = engine.index(
        "NGreen::callback->closeVfBusMasterAdmission()", register_source)
    failed_admission_revoke = engine.index(
        "provider->setBusMasterEnable(false)", failed_admission_close)
    postcheck = engine.index(
        "VF deferred MSI source did not establish its bus-master boundary",
        failed_admission_revoke)
    if not mask < mask_validation < hws < global_hws < precheck < \
            open_admission < open_marker < configure < \
            failed_configuration_close < failed_configuration_revoke < \
            register_source < failed_admission_close < \
            failed_admission_revoke < postcheck:
        raise AssertionError(f"{label}: late HWS/MSI Bus Master admission changed")
    if "provider, kIOInterruptTypePCIMessaged, 1, 1, 0" not in engine:
        raise AssertionError(f"{label}: late VF MSI request is not exactly one vector")
    if engine.count("setBusMasterEnable(") != 2 or \
            "setBusMasterEnable(true)" in engine:
        raise AssertionError(f"{label}: late MSI boundary gained an unsafe Bus Master transition")
    for token in ("(engineMask & (1ULL << index)) == 0",
                  "VF inactive engine has an HWS mapping",
                  "VF engine HWS mapping incomplete before bus mastering"):
        if token not in engine:
            raise AssertionError(
                f"{label}: native active HWS-mask validation lost {token}")

    stop_engine = function_body(gen11, "bool Gen11::stopGraphicsEngine(void *that)")
    quiesce = stop_engine.index("vfQuiesceDeviceForShutdown(gVfHardwareGuc)")
    bridge_disable = stop_engine.index(
        "callback->vfInterruptBridgeDisable)(", quiesce)
    dma_proof = stop_engine.index("if (gVfDmaQuiesced)", bridge_disable)
    stop_provider = stop_engine.index(
        "IOPCIDevice, static_cast<IOService *>(that)->getProvider())", dma_proof)
    final_close = stop_engine.index(
        "NGreen::callback->closeVfBusMasterAdmission()", stop_provider)
    final_stop = stop_engine.index(
        "provider->setBusMasterEnable(false)", final_close)
    final_barrier = stop_engine.index("OSSynchronizeIO()", final_stop)
    final_readback = stop_engine.index(
        "provider->configRead16(kIOPCIConfigCommand)", final_barrier)
    final_check = stop_engine.index(
        "VF PCI bus mastering remained enabled after DMA quiescence",
        final_readback)
    accel_disable = stop_engine.index(
        "callback->ioGraphicsDisableAccelerator)(that)", final_check)
    if not quiesce < bridge_disable < dma_proof < stop_provider < final_close < \
            final_stop < final_barrier < final_readback < final_check < accel_disable:
        raise AssertionError(
            f"{label}: final DMA-quiesced Bus Master revocation changed")


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise AssertionError(f"ambiguous mutation anchor: {before}")
    return source.replace(before, after, 1)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            f"usage: {sys.argv[0]} kern_green.cpp kern_green.hpp kern_gen11.cpp")
    green_path, header_path, gen11_path = map(pathlib.Path, sys.argv[1:])
    green = green_path.read_text(encoding="utf-8")
    green_header = header_path.read_text(encoding="utf-8")
    gen11 = gen11_path.read_text(encoding="utf-8")
    contract(green, green_header, gen11,
             f"{green_path}/{header_path}/{gen11_path}")

    mutations = (
        (replace_once(green, "this->iGPU->setBusMasterEnable(false);",
                      "this->iGPU->setBusMasterEnable(true);"), green_header, gen11),
        (replace_once(green,
                      "sriovCapability != NGGpuCapabilities::Sriov::Absent",
                      "sriovCapability == NGGpuCapabilities::Sriov::Absent"), green_header, gen11),
        (replace_once(green, "this->iGPU->setBusMasterEnable(physicalAccess);",
                      "this->iGPU->setBusMasterEnable(true);"), green_header, gen11),
        (replace_once(green, "busMasterEnabled != physicalAccess",
                      "busMasterEnabled == physicalAccess"), green_header, gen11),
        (replace_once(green, "device != owner->iGPU",
                      "device == owner->iGPU"), green_header, gen11),
        (replace_once(green, "owner->vfBusMasterAdmissionOpen()",
                      "!owner->vfBusMasterAdmissionOpen()"), green_header, gen11),
        (green, replace_once(green_header,
                             "kSetBusMasterEnableVirtualOffset = 0x11B",
                             "kSetBusMasterEnableVirtualOffset = 0x11A"), gen11),
        (green, green_header, replace_once(gen11, "pciDevice->setBusMasterEnable(false);",
                             "pciDevice->setBusMasterEnable(true);")),
        (green, green_header, replace_once(gen11,
                             "VF PCI bus mastering remained enabled before native start",
                             "missing VF Bus Master readback")),
        (green, green_header, replace_once(gen11,
                             "getMember<OSObject *>(that, 0x1438)",
                             "getMember<OSObject *>(that, 0x1430)")),
        (green, green_header, replace_once(gen11,
                             "NGreen::callback->openVfBusMasterAdmission()",
                             "NGreen::callback->vfBusMasterAdmissionOpen()")),
        (green, green_header, replace_once(gen11,
                             "if (gVfDmaQuiesced) {",
                             "if (!gVfDmaQuiesced) {")),
        (green, green_header, replace_once(gen11,
                             "VF PCI bus mastering remained enabled after DMA quiescence",
                             "missing final VF Bus Master readback")),
    )
    for index, (changed_green, changed_header, changed_gen11) in enumerate(mutations):
        try:
            contract(changed_green, changed_header, changed_gen11,
                     f"mutation-{index}")
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"escaped PCI Bus Master mutation {index}")
    print("PASS: identity-scoped PF/VF PCI Bus Master boundary and thirteen negative mutations")


if __name__ == "__main__":
    main()
