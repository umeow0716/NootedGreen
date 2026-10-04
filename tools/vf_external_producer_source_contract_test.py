#!/usr/bin/env python3
"""Static source contract for the counted VF external-producer boundary."""

import pathlib
import sys


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start:offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


def block(source, marker, end_marker):
    start = source.index(marker)
    end = source.index(end_marker, start)
    return source[start:end]


def contract(source, path="<source>"):
    route_symbols = (
        "__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs",
        "__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy",
        "__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits",
        "__ZN17IOAccel2DContext26finishEj",
        "__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy",
        "__ZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN17IOAccelGLContext214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN27IOAccelGLDrawableUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN29IOAccelDisplayPipeUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
        "__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_",
        "__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei",
        "__ZN22IOGraphicsAccelerator218finalize_interruptEP22IOInterruptEventSourcei",
        "__ZN22IOGraphicsAccelerator218deviceCacheControlEP20IOSurfaceDeviceCachejyy",
        "__ZN22IOGraphicsAccelerator220emitFirstFlushEventsEv",
    )
    routes = block(source, "KernelPatcher::RouteRequest externalProducerRoutes[]",\
                   "PANIC_COND(!patcher.routeMultiple(")
    assert routes.count('{"__') == len(route_symbols), \
        f"{path}: external-producer route cardinality changed"
    for symbol in route_symbols:
        assert routes.count(symbol) == 1, \
            f"{path}: missing/duplicate counted outer route: {symbol}"
    for forbidden in ("prepare", "load", "unload", "pageon", "pageoff",
                      "submitBlit", "submitWorkItem"):
        assert forbidden not in routes, \
            f"{path}: retirement/shared bridge was incorrectly outer-gated: {forbidden}"

    wrappers = {
        "IOReturn Gen11::vfCommandQueueSubmit(": ("0x5C0", "oVfCommandQueueSubmit"),
        "IOReturn Gen11::vfContextSubmit(": ("0x5A8", "oVfContextSubmit"),
        "IOReturn Gen11::vf2DSetSurface(": ("0x5A8", "oVf2DSetSurface"),
        "IOReturn Gen11::vf2DFinish(": ("0x5A8", "oVf2DFinish"),
        "IOReturn Gen11::vf2DBlit(": ("0x5A8", "oVf2DBlit"),
        "IOReturn Gen11::vfSurfaceExternalMethod(": ("0x12C8", "oVfSurfaceExternalMethod"),
        "IOReturn Gen11::vfSharedExternalMethod(": ("0xF8", "oVfSharedExternalMethod"),
        "IOReturn Gen11::vfGLDrawableExternalMethod(": ("0xF8", "oVfGLDrawableExternalMethod"),
        "IOReturn Gen11::vfSurfaceMtlExternalMethod(": ("0x2F8", "oVfSurfaceMtlExternalMethod"),
        "IOReturn Gen11::vfMemoryInfoExternalMethod(": ("0xE0", "oVfMemoryInfoExternalMethod"),
        "IOReturn Gen11::vfDisplayPipeExternalMethod(": ("0xD8", "oVfDisplayPipeExternalMethod"),
        "IOReturn Gen11::vfDisplayChangeHandler(": ("0x88", "oVfDisplayChangeHandler"),
    }
    for signature, (owner_offset, original) in wrappers.items():
        body = function_body(source, signature)
        for token in (f"getMember<void *>(that, {owner_offset})",
                      "VfExternalProducerGuard guard(accelerator);",
                      "if (!guard)", "return kIOReturnOffline;", original):
            assert token in body, f"{path}: incomplete counted wrapper {signature}: {token}"
        assert body.index("VfExternalProducerGuard guard") < body.index(original), \
            f"{path}: original entered before lease: {signature}"

    gl = function_body(source, "IOReturn Gen11::vfGLContextExternalMethod(")
    for token in ("selector >= 0x100 && selector <= 0x105",
                  "getMember<void *>(that, 0x5A8)",
                  "VfExternalProducerGuard guard(accelerator, specialized);",
                  "oVfGLContextExternalMethod"):
        assert token in gl, f"{path}: lost non-nested GL selector admission: {token}"

    for signature, original in (
            ("void Gen11::vfGartCollector(", "oVfGartCollector"),
            ("void Gen11::vfEmitFirstFlushEvents(", "oVfEmitFirstFlushEvents")):
        body = function_body(source, signature)
        for token in ("VfExternalProducerGuard guard(that);", "if (!guard)", original):
            assert token in body, f"{path}: incomplete direct-receiver callback gate: {signature}"
        guard_index = body.index("VfExternalProducerGuard guard(that);")
        assert "getMember" not in body[:guard_index] and \
            guard_index < body.index(original), \
            f"{path}: direct callback dereferenced/entered receiver before lease: {signature}"

    finalize = function_body(source, "void Gen11::vfFinalizeInterrupt(")
    for token in ("VfExternalProducerGuard guard(that);", "if (!guard)",
                  "VfDeviceCacheRetirementScope retirement(that);",
                  "oVfFinalizeInterrupt"):
        assert token in finalize, \
            f"{path}: incomplete counted finalize wrapper: {token}"
    assert finalize.index("VfExternalProducerGuard guard(that);") < \
        finalize.index("VfDeviceCacheRetirementScope retirement(that);") < \
        finalize.index("oVfFinalizeInterrupt"), \
        f"{path}: finalize retirement scope entered outside counted lease"

    device_cache = function_body(source, "void Gen11::vfDeviceCacheControl(")
    for token in ("selector == 3 || selector == 4",
                  "gVfDeviceCacheRetirementDepth > 0",
                  "VfExternalProducerGuard guard(that, !nestedRetirement);",
                  "if (!guard)", "oVfDeviceCacheControl"):
        assert token in device_cache, \
            f"{path}: incomplete device-cache retirement admission: {token}"
    cache_guard = device_cache.index("VfExternalProducerGuard guard")
    assert "getMember" not in device_cache[:cache_guard] and \
        cache_guard < device_cache.index("oVfDeviceCacheControl"), \
        f"{path}: device-cache callback dereferenced/entered receiver before admission"

    sleep = function_body(source, "IOReturn Gen11::vfDisplaySleepCallback(")
    for token in ("VfExternalProducerGuard guard(that);", "if (!guard)",
                  "return kIOReturnOffline;", "oVfDisplaySleepCallback"):
        assert token in sleep, f"{path}: incomplete DisplaySleep admission: {token}"
    sleep_guard = sleep.index("VfExternalProducerGuard guard(that);")
    assert "getMember" not in sleep[:sleep_guard] and \
        sleep_guard < sleep.index("oVfDisplaySleepCallback"), \
        f"{path}: DisplaySleep dereferenced/entered receiver before lease"
    sleep_symbol = "__ZN16IntelAccelerator20DisplaySleepCallbackE17DisplaySleepCmd_tjj"
    assert source.count(sleep_symbol) == 1, \
        f"{path}: missing/duplicate DisplaySleep route"

    guard = block(source, "class VfExternalProducerGuard",\
                  "static bool vfCloseExternalProducerGateAndWait()")
    normalized_guard = "".join(guard.split())
    predicate = ("route&&gVfIdentity==VfIdentity::Virtual&&accelerator&&"
                 "gVfAccelerator&&accelerator==gVfAccelerator")
    assert predicate in normalized_guard, f"{path}: receiver-scoped VF predicate changed"
    assert "OSSynchronizeIO();" in guard
    assert "admitted=!tracked||vfEnterExternalProducer();" in normalized_guard
    assert "if(tracked&&admitted)vfLeaveExternalProducer();" in normalized_guard
    retirement_scope = block(source, "class VfDeviceCacheRetirementScope",\
                             "static bool vfCloseExternalProducerGateAndWait()")
    normalized_retirement = "".join(retirement_scope.split())
    for token in ("OSIncrementAtomic(&gVfDeviceCacheRetirementDepth);",
                  "OSDecrementAtomic(&gVfDeviceCacheRetirementDepth);"):
        assert "".join(token.split()) in normalized_retirement, \
            f"{path}: unbalanced device-cache retirement scope: {token}"

    fault = function_body(source, "void vfMarkProtocolFault(")
    assert fault.index("NGVfIrqGate::close") < fault.index("gVfProtocolFault"), \
        f"{path}: protocol fault no longer seals external producers first"
    drain = function_body(source, "static bool vfCloseExternalProducerGateAndWait(")
    for token in ("vfCanUseSleepingLock()", "NGVfIrqGate::close",
                  "NGVfIrqGate::drained", "IOSleep(1)",
                  "timed out draining VF external producers"):
        assert token in drain, f"{path}: incomplete bounded producer drain: {token}"
    assert "kVfExternalProducerDrainTimeoutMs = 5000" in source

    start = function_body(source, "bool Gen11::start(void *that, void *provider)")
    gate_check = start.index("gVfExternalProducerGate != 0")
    publish = start.index("gVfAccelerator = that")
    assert gate_check < publish, f"{path}: accelerator published before one-shot gate check"
    stop = function_body(source, "void Gen11::acceleratorStop(")
    close = stop.index("vfCloseExternalProducerGateAndWait()")
    finalize = stop.index("callback->oVfFinalizeInterrupt")
    balanced = stop.index("gVfDeviceCacheRetirementDepth != 0")
    stopping = stop.index("gVfDeviceStopping")
    native = stop.index("callback->oAcceleratorStop")
    assert close < finalize < balanced < stopping < native, \
        f"{path}: close/drain/cache-finalize/device-stopping/native-stop order changed"
    assert "isLockedByCurrentThread" not in stop, \
        f"{path}: always-true Tahoe ownership stub used as a lock proof"
    print("PASS counted VF external-producer source/route/teardown contract")


def mutation_contract(source, path):
    mutations = (
        ("0x12C8", "0x12D0"),
        ("selector >= 0x100 && selector <= 0x105", "selector >= 0x100"),
        ("VfExternalProducerGuard guard(accelerator);", "/* lease removed */", 1),
        ("PANIC_COND(!vfCloseExternalProducerGateAndWait(),",
         "PANIC_COND(!true,", 1),
        ("route && gVfIdentity == VfIdentity::Virtual", "gVfIdentity == VfIdentity::Virtual", 1),
        ("if (tracked && admitted)", "if (admitted)", 1),
        ("__ZN16IntelAccelerator20DisplaySleepCallbackE17DisplaySleepCmd_tjj",
         "__ZN16IntelAccelerator20DisplaySleepCallback_REMOVED", 1),
        ("__ZN22IOGraphicsAccelerator218finalize_interruptEP22IOInterruptEventSourcei",
         "__ZN22IOGraphicsAccelerator218finalize_interrupt_REMOVED", 1),
        ("__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs",
         "__ZN19IOAccelCommandQueue22submit_command_buffers_REMOVED", 1),
        ("VfExternalProducerGuard guard(that);",
         "getMember<void *>(that, 0x88);\n\tVfExternalProducerGuard guard(that);", 1),
        ("gVfDeviceCacheRetirementDepth > 0",
         "gVfDeviceCacheRetirementDepth >= 0", 1),
    )
    for mutation in mutations:
        old, new = mutation[:2]
        count = mutation[2] if len(mutation) == 3 else -1
        candidate = source.replace(old, new, count)
        try:
            contract(candidate, path)
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"{path}: mutation escaped contract: {old!r} -> {new!r}")
    print(f"PASS {len(mutations)} external-producer source mutations rejected")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: vf_external_producer_source_contract_test.py kern_gen11.cpp")
    path = pathlib.Path(sys.argv[1])
    source = path.read_text()
    contract(source, str(path))
    mutation_contract(source, str(path))


if __name__ == "__main__":
    main()
