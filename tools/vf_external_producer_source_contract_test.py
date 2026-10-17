#!/usr/bin/env python3
"""Static source contract for the counted VF external-producer boundary."""

import pathlib
import sys

FINAL_MEDIA_GUARD = ('PANIC_COND(!live(static_cast<uint32_t>(publication.preparation.words)) ||\n'
                     '\t\t!NgreenAvcGen12::matchesPublishedBytes(span.commands,span.availableWords,words,*entry->scratch,publication) ||\n'
                     '\t\t!live(static_cast<uint32_t>(publication.preparation.words)),"ngreen",\n'
                     '\t\t"Lost owned AVC mapping before native submission");')

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


def adapter_frame_contract(source):
    body = block(source, "struct NativeOtherFrameBackend {", "struct NativeLateFactoryBatch {")
    adoption = block(source, "struct NativeCodecBackend {", "selected->framesPublished=true;return 0;")
    for required in (
        "binding.frame==scope.image()+0x27724?",
        "publishAvcInputCallbacks(lease,hook,",
        "reinterpret_cast<uintptr_t>(&dispatchOwnedAvcSize)",
        "reinterpret_cast<uintptr_t>(&dispatchOwnedAvcBuilder),sameOwner",
    ):
        assert required in adoption, "adapter: exact AVC callback pair not published"
    for required in (
        "registry.promoteExclusive(lease,image,physical,generation,owner->context())",
        "if(!exclusive.get())return 5;",
        "dispatchAvc(exclusive,scope,image,physical,generation,input,output)",
        "registry.demoteExclusive(exclusive,image,physical,generation,owner->context())",
    ):
        assert required in body, "adapter: AVC frame owner transfer missing"
    assert "const size_t ownerIndex=current->items.ownerIndex(index);" in source and \
        "if(batch.items.ownerIndex(i)!=i)continue;" in source, "adapter: repeated-context batch pin not shared"
    gate = "if(!NgreenNativeAvcReservation::frameAdmission(image,entry,publicationReady,physical,generation,context,request))return 5;"
    assert body.count(gate) == 1, "adapter: exact AVC publication refusal missing or changed"
    before, after = body.split(gate)
    phase = "if(!NgreenNativeAvcReservation::framePhaseAdmission(image,owner->frameTable().nativeFrame(),physical,generation,owner->context(),input))return 5;"
    assert body.count(phase) == 1, "adapter: exact input-phase refusal missing"
    reserve = body.index("const auto reservation=owner->reserveNativeOtherFrame")
    for required in (
        "AvcInput::captureForContext(context,input,{},TglDarwinTypedCfReferenceApi{},&readMemory,admitted)",
        "outputs.capture(context,input,retained->originalPayload(),output,&readMemory,&writableDataMemory,",
        "self.input.matchesContext(id.context,&readMemory,admit) &&",
        "self.outputs.matches(&readMemory) && admit();",
        "if((phase.started() || result==0) && !phase.complete())_exit(70);",
        "if(retainedInput && !owner->retainNativeOtherFrameInput(lease,scope,reservation,*ownedInput))return 5;",
    ):
        assert required in body, "adapter: owned input/phase/output lifetime admission missing"
    assert body.index(phase) < reserve < body.index(gate), "adapter: phase refusal after reservation/native path"
    for required in (
        "callbacks[0x30/8]!=reinterpret_cast<uintptr_t>(&dispatchOwnedAvcSize)",
        "callbacks[0x60/8]!=reinterpret_cast<uintptr_t>(&dispatchOwnedAvcBuilder)",
        "owner->frameTable().nativeSize()!=image+0x2732c",
        "owner->frameTable().nativeBuilder()!=image+0x2737e",
    ):
        assert required in body[:reserve], "adapter: exact paired callback admission missing"
    assert "queryPublication(lease,active,image,physical,generation,publication)" in before, \
        "adapter: frame admission precedes readiness query"
    assert "invoke()" not in before, "adapter: native invocation precedes publication refusal"
    assert "if(publicationReady) {" in after, "adapter: lost ready-only reservation path"
    ready = after.split("if(publicationReady) {", 1)[1]
    reference_gate = "if(!NgreenNativeAvcReservation::framePreflight(request,&readMemory))return 5;"
    assert ready.lstrip().startswith(reference_gate), "adapter: native reference slot admission missing or late"
    assert "NGRN_RUNTIME_SNAPSHOT_ARM" not in body and "snapshotAttempted" not in body, \
        "adapter: untranslated snapshot fallback restored"


def adapter_frame_mutations(source):
    adapter_frame_contract(source)
    gate = "if(!NgreenNativeAvcReservation::frameAdmission(image,entry,publicationReady,physical,generation,context,request))return 5;"
    phase = "if(!NgreenNativeAvcReservation::framePhaseAdmission(image,owner->frameTable().nativeFrame(),physical,generation,owner->context(),input))return 5;"
    mutations = (
        source.replace("publishAvcInputCallbacks(lease,hook,", "publishFrame(lease,hook,", 1),
        source.replace("binding.frame==scope.image()+0x27724?", "false?", 1),
        source.replace("registry.promoteExclusive(lease,image,physical,generation,owner->context())", "decltype(exclusive){}", 1),
        source.replace("registry.demoteExclusive(exclusive,image,physical,generation,owner->context())", "decltype(lease){}", 1),
        source.replace("if(batch.items.ownerIndex(i)!=i)continue;", "", 1),
        source.replace(gate, "", 1),
        source.replace(gate, gate.replace("if(!", "if("), 1),
        source.replace(gate, gate.replace("entry,publicationReady", "entry,true"), 1),
        source.replace(gate, "return invoke();\n" + gate, 1),
        source.replace(gate, gate + '\n// NGRN_RUNTIME_SNAPSHOT_ARM', 1),
        source.replace("!NgreenNativeAvcReservation::framePreflight(request,&readMemory)", "false", 1),
        source.replace("framePreflight(request,&readMemory)", "framePreflight(context,&readMemory)", 1),
        source.replace("if(!NgreenNativeAvcReservation::framePreflight", "if(NgreenNativeAvcReservation::framePreflight", 1),
        source.replace(phase, "", 1),
        source.replace(phase, phase.replace("if(!", "if("), 1),
        source.replace(phase, phase.replace("physical,generation", "physical,0"), 1),
        source.replace(phase, phase.replace("owner->context(),input", "owner->context(),0"), 1),
        source.replace(phase, "", 1).replace(gate, phase+gate, 1),
        source.replace("callbacks[0x30/8]!=reinterpret_cast<uintptr_t>(&dispatchOwnedAvcSize)", "false", 1),
        source.replace("callbacks[0x60/8]!=reinterpret_cast<uintptr_t>(&dispatchOwnedAvcBuilder)", "false", 1),
        source.replace("owner->frameTable().nativeSize()!=image+0x2732c", "false", 1),
        source.replace("owner->frameTable().nativeBuilder()!=image+0x2737e", "false", 1),
    )
    for candidate in mutations:
        assert candidate != source, "adapter mutation did not change source"
        try:
            adapter_frame_contract(candidate)
        except (AssertionError, ValueError):
            continue
        raise AssertionError("adapter: publication refusal mutation escaped")
    print(f"PASS {len(mutations)} adapter frame-publication refusal mutations rejected")


def avc_phase_contract(header, reservation):
    preflight = function_body(reservation, "bool framePreflight(")
    assert "!read(request+0x20,&payload,sizeof(payload)) || !payload" in preflight
    assert ("return referenceIndices(request,read,payload) && quantizerIndex(request,read,payload) &&\n"
            "        activeReferenceCounts(request,read,payload);") in preflight
    for helper in ("referenceIndices", "quantizerIndex", "activeReferenceCounts"):
        assert "(expectedPayload && payload!=expectedPayload)" in function_body(reservation, "bool " + helper + "(")
    strict = function_body(header, "inline Result produce(")
    size = function_body(header, "inline PreRelocationSize sizeBeforeRelocation(")
    inspection = function_body(header, "inline Result inspectOrProduce(")
    commit = function_body(reservation, "CommitResult commit(")
    assert "inspectOrProduce<false>" in strict and "<true>" not in strict
    assert "inspectOrProduce<true>(input,count,width,height,nullptr,1536)" in size
    assert "legacyNv12Surface(input+at,surfaceWidth,surfaceHeight,!MeasureOnly)" in inspection
    branch = inspection.index("if(MeasureOnly) {")
    assert inspection.index('r.reason="reservation-size-only";return r;', branch) < inspection.index("size_t out=0;")
    assert "NgreenAvcGen12::sizeBeforeRelocation" in commit
    assert "NgreenAvcGen12::produce(" not in commit
    assert "if(!measured.bounded)" in commit and "plan(before,measured.outputWords)" in commit


def avc_phase_mutations(header, reservation):
    avc_phase_contract(header, reservation)
    variants = (
        (header.replace("return detail::inspectOrProduce<false>", "return detail::inspectOrProduce<true>", 1), reservation),
        (header.replace("inspectOrProduce<true>(input,count,width,height,nullptr,1536)", "inspectOrProduce<false>(input,count,width,height,nullptr,1536)", 1), reservation),
        (header.replace("surfaceHeight,!MeasureOnly)", "surfaceHeight,false)", 1), reservation),
        (header.replace("if(MeasureOnly) {", "if(false) {", 1), reservation),
        (header, reservation.replace("NgreenAvcGen12::sizeBeforeRelocation", "NgreenAvcGen12::produce", 1)),
        *((header, reservation.replace(helper + "(request,read,payload)", "true", 1))
          for helper in ("referenceIndices", "quantizerIndex", "activeReferenceCounts")),
        *((header, reservation.replace(helper + "(request,read,payload)", helper + "(request,read,0)", 1))
          for helper in ("referenceIndices", "quantizerIndex", "activeReferenceCounts")),
        (header, reservation.replace("(expectedPayload && payload!=expectedPayload)", "false", 1)),
    )
    for changed_header, changed_reservation in variants:
        assert (changed_header, changed_reservation) != (header, reservation)
        try:
            avc_phase_contract(changed_header, changed_reservation)
        except (AssertionError, ValueError):
            continue
        raise AssertionError("AVC relocation phase mutation escaped")
    print(f"PASS {len(variants)} AVC reservation/publication phase mutations rejected")


def contract(source, path="<source>"):
    provenance = function_body(source, 'static void vfObserveNativeBatchProvenance(')
    stable = provenance[provenance.index('const bool stable='):]
    for field in ('gpuMap', 'gpuBase', 'backingBytes', 'availableWords', 'reservedWords'):
        assert f'after.{field}==span.{field}' in stable, f'{path}: provenance lost {field} identity'
    assert 'getMember<void *>(map,0x18)==mapMemory' in stable, f'{path}: provenance lost backing identity'
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

    shared = function_body(source, "IOReturn Gen11::vfSharedExternalMethod(")
    owned = function_body(source, "static IOReturn vfReadOwnedDeviceObservation(")
    diagnostic_branch = shared[shared.index("if (selector == NGVfDeviceObservation::selector"):
                               shared.index("if (selector == NGVfAvcPublication::selector")]
    for token in ("selector == NGVfDeviceObservation::selector",
                  "gVfIdentity == VfIdentity::Virtual", "accelerator == gVfAccelerator",
                  'checkKernelArgument("-ngreenmediaadapter")',
                  "return vfReadOwnedDeviceObservation(accelerator, selector, arguments, dispatch, target, reference);"):
        assert token in diagnostic_branch, f"{path}: lost owned selector admission: {token}"
    assert shared.index("VfExternalProducerGuard guard") < shared.index("return vfReadOwnedDeviceObservation"), \
        f"{path}: owned query must hold native instance lease"
    avc_branch=shared[shared.index("if (selector == NGVfAvcPublication::selector"):
                      shared.index("// Observe the resource-create boundary")]
    for token in ('gVfIdentity == VfIdentity::Virtual','accelerator == gVfAccelerator',
                  'checkKernelArgument("-ngreenmediaadapter")','return vfReadOwnedAvcPublication('):
        assert token in avc_branch, f"{path}: lost AVC negotiation instance admission"
    avc_query=function_body(source,'static IOReturn vfReadOwnedAvcPublication(')
    for token in ('NGVfAvcPublication::inlineShape(',
                  'gVfMediaPublicationRouteReady && gVfMediaPublicationAbiVerified',
                  'checkKernelArgument("-ngreenavcgen12")', '!gVfSubmissionStopped', '!gVfProtocolFault'):
        assert token in avc_query, f"{path}: premature AVC readiness"
    assert 'static bool gVfMediaPublicationRouteReady = false;' in source
    for token in ("!gVfDeviceObservationAbiVerified", "return kIOReturnUnsupported;",
                  "selector != NGVfDeviceObservation::selector", "NGVfDeviceObservation::inlineShape(",
                  "return kIOReturnBadArgument;", "getRegistryEntryID()",
                  "NGVfDeviceObservation::collect(", "0x9DC", "0xDC8", "return kIOReturnBusy;"):
        assert token in owned, f"{path}: incomplete owned read-only query: {token}"
    assert "oVfSharedExternalMethod" not in owned and "structureInput" not in owned, \
        f"{path}: owned query must not enter native resource dispatch/read input"
    for token in ("if (selector == 0)", "while (used < 32)",
                  "OSCompareAndSwap(used, used + 1, &observedCreates)",
                  "const IOReturn result = FunctionCast(vfSharedExternalMethod,",
                  "return result;"):
        assert token in shared, f"{path}: lost bounded native resource observation: {token}"
    task_branch, resource_branch = shared.split("// Observe the resource-create boundary", 1)
    native_task_branch = task_branch[task_branch.index("if (selector == NGVfTaskCompletion::selector"):
                                   task_branch.index("if (selector == NGVfDeviceObservation::selector")]
    for token in ("gVfIdentity == VfIdentity::Virtual", "accelerator && accelerator == gVfAccelerator",
                  'checkKernelArgument("-ngreenmediaadapter")'):
        assert token in native_task_branch, f"{path}: task branch lost exact VF admission: {token}"
    assert resource_branch.count("callback->oVfSharedExternalMethod") == 1, \
        f"{path}: resource observation must call native exactly once"
    assert task_branch.count("FunctionCast(vfSharedExternalMethod,") == 1 and \
        "return FunctionCast(vfSharedExternalMethod," in task_branch, \
        f"{path}: task query must return directly through one native lease"
    for token in ("NGVfTaskCompletion::inlineShape(", "!gVfTaskCompletionAbiVerified",
                  "static const IOExternalMethodDispatch ownedDispatch", "&vfReadOwnedTaskCompletion",
                  "sizeof(NGVfTaskCompletion::Request)", "sizeof(NGVfTaskCompletion::Reply)",
                  "const mach_vm_address_t mappedBufferGetter=callback->vfSharedMappedBufferGetVirtualAddress;"):
        assert token in task_branch, f"{path}: task-query dispatch lost {token}"
    task = function_body(source, "static IOReturn vfReadOwnedTaskCompletion(OSObject *target, void *reference, IOExternalMethodArguments *args)\n{")
    for token in ("NGVfTaskCompletion::validRequest(request)", "VfContextOperationGuard operation;",
                  "getMember<void *>(client,0x100)", "void *task=getMember<void *>(shared,0x88);",
                  "entry.task!=task", "entry.submissionCoverage.coveredStamp!=expected",
                  "vfContextKnownIdle(entry,false,mappedBufferGetter)",
                  "entry.submissionCoverage.completionEvidence(", "entry.submissionCoverage.coveredSerial",
                  "IOSimpleLockLockDisableInterrupt", "IOSimpleLockUnlockEnableInterrupt",
                  "slot-first<16", "seen!=required || !live()", "!gVfProtocolFault", "0xDC8"):
        assert token in task, f"{path}: task-query evidence lost {token}"
    assert shared.count("if (ticket)") == 3, \
        f"{path}: both resource observation markers must be bounded"
    assert shared.count("while (used < 8)") == 3, \
        f"{path}: create/output/non-create failure budgets must all be bounded"
    for token in ("if (selector != 0)", "while (used < 16)",
                  "OSCompareAndSwap(used, used + 1, &observedOtherMethods)",
                  "if (selector != 0 && result != kIOReturnSuccess)",
                  "OSCompareAndSwap(used, used + 1, &observedOtherFailures)",
                  "V360: shared resource method enter", "V360: shared resource method return",
                  "V360: shared resource method failed"):
        assert token in shared, f"{path}: lost bounded non-create observation: {token}"
    assert shared.count("if (otherTicket)") == 2, \
        f"{path}: non-create enter/return must share one bounded ticket"
    for token in ("if (selector == 0 && result != kIOReturnSuccess)",
                  "static volatile UInt32 observedFailures = 0;", "while (used < 8)",
                  "OSCompareAndSwap(used, used + 1, &observedFailures)",
                  "V358: shared resource create failed"):
        assert token in shared, f"{path}: lost independent bounded failure observation: {token}"
    assert "static_cast" not in resource_branch.split("const IOReturn result")[0], \
        f"{path}: resource observation must not interpret user arguments"
    for token in ("if (selector == 0 && result == kIOReturnSuccess && arguments)",
                  "args->structureOutput && !args->structureOutputDescriptor",
                  "args->structureOutputSize >= 0x58 && args->structureOutputSize <= 0x1000",
                  "static_cast<const uint8_t *>(args->structureOutput) + 0x10",
                  "static_cast<const uint8_t *>(args->structureOutput) + 0x18",
                  "if (!inlineShape || !ro || !rw)",
                  "OSCompareAndSwap(used, used + 1, &observedOutputFailures)"):
        assert token in shared, f"{path}: lost bounded kernel output observation: {token}"
    assert "structureInput" not in shared and "scalarInput" not in shared, \
        f"{path}: resource observation must never read user input"
    assert shared.index("const auto *args") > shared.index("callback->oVfSharedExternalMethod"), \
        f"{path}: output read before native completion"

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
    for token in ("vfTargetsPublishedAccelerator(that)",
                  "NGVfAcceleratorStop::finalizeAction(phase)",
                  "NGVfAcceleratorStop::Phase::Idle",
                  "NGVfAcceleratorStop::Phase::Finalizing",
                  "vfCloseExternalProducerGateAndWait()",
                  "VfDeviceCacheRetirementScope retirement(that);",
                  "NGVfAcceleratorStop::ownerReturnAction(phase, false)",
                  "NGVfAcceleratorStop::Phase::Finalized",
                  "oVfFinalizeInterrupt"):
        assert token in finalize, \
            f"{path}: incomplete lifecycle-owned finalize wrapper: {token}"
    assert "VfExternalProducerGuard" not in finalize, \
        f"{path}: finalizer can deadlock while draining its own producer lease"
    claim = finalize.index("NGVfAcceleratorStop::Phase::Idle")
    close = finalize.index("vfCloseExternalProducerGateAndWait()", claim)
    scope = finalize.index("VfDeviceCacheRetirementScope retirement(that);", close)
    native = finalize.rindex("FunctionCast(vfFinalizeInterrupt")
    returned = finalize.index(
        "NGVfAcceleratorStop::ownerReturnAction(phase, false)", native)
    assert claim < close < scope < native < returned, \
        f"{path}: finalize owner/drain/thread-scope/native order changed"

    device_cache = function_body(source, "void Gen11::vfDeviceCacheControl(")
    for token in ("selector == 3 || selector == 4",
                  "vfOwnsDeviceCacheRetirement()",
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
                  "class VfDeviceCacheRetirementScope")
    normalized_guard = "".join(guard.split())
    predicate = ("route&&gVfIdentity==VfIdentity::Virtual&&accelerator&&"
                 "gVfAccelerator&&accelerator==gVfAccelerator")
    assert predicate in normalized_guard, f"{path}: receiver-scoped VF predicate changed"
    assert "OSSynchronizeIO();" in guard
    assert "admitted=!tracked||vfEnterExternalProducer();" in normalized_guard
    assert "if(tracked&&admitted)vfLeaveExternalProducer();" in normalized_guard
    retirement_scope = block(source, "class VfDeviceCacheRetirementScope",\
                             "static bool vfOwnsDeviceCacheRetirement()")
    normalized_retirement = "".join(retirement_scope.split())
    for token in ("gVfDeviceCacheRetirementDepth.get();",
                  "gVfDeviceCacheRetirementDepth.set(1)",
                  "gVfDeviceCacheRetirementDepth.erase()"):
        assert "".join(token.split()) in normalized_retirement, \
            f"{path}: unbalanced device-cache retirement scope: {token}"
    assert "OSIncrementAtomic" not in retirement_scope and \
        "OSDecrementAtomic" not in retirement_scope, \
        f"{path}: cache retirement reverted to cross-thread global depth"
    assert "ThreadLocal<UInt32, 1> gVfDeviceCacheRetirementDepth" in source, \
        f"{path}: missing single-owner thread-local retirement storage"

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
    phase_check = start.index("vfAcceleratorStopPhase()", gate_check)
    publish = start.index("gVfAccelerator = that")
    assert gate_check < phase_check < publish, \
        f"{path}: accelerator published before one-shot teardown-state check"
    stop = function_body(source, "void Gen11::acceleratorStop(")
    for token in (
            "NGVfAcceleratorStop::stopAction(",
            "NGVfAcceleratorStop::StopAction::BeginFinalization",
            "NGVfAcceleratorStop::Phase::Idle",
            "NGVfAcceleratorStop::Phase::Finalizing",
            "vfCloseExternalProducerGateAndWait()",
            "VfDeviceCacheRetirementScope retirement(that);",
            "NGVfAcceleratorStop::ownerReturnAction(phase, true)",
            "NGVfAcceleratorStop::Phase::NativeStopActive",
            "NGVfAcceleratorStop::Phase::NativeStopComplete",
            "vfOwnsDeviceCacheRetirement()",
            "Cross-thread VF stop collided with active cache finalizer"):
        assert token in stop, f"{path}: incomplete exactly-once stop protocol: {token}"
    run_native = stop.index("const auto runNativeStop")
    closed = stop.index("NGVfIrqGate::closed(gVfExternalProducerGate)", run_native)
    stopping = stop.index("OSCompareAndSwap(0, 1, &gVfDeviceStopping)", closed)
    native = stop.index("callback->oAcceleratorStop)(that, provider)", stopping)
    complete = stop.index("NGVfAcceleratorStop::Phase::NativeStopComplete", native)
    assert run_native < closed < stopping < native < complete, \
        f"{path}: producer-drain/device-stopping/native-stop/completion order changed"
    claim = stop.index("NGVfAcceleratorStop::StopAction::BeginFinalization", complete)
    close = stop.index("vfCloseExternalProducerGateAndWait()", claim)
    finalize_call = stop.index("FunctionCast(vfFinalizeInterrupt", close)
    owner_return = stop.index(
        "NGVfAcceleratorStop::ownerReturnAction(phase, true)", finalize_call)
    assert claim < close < finalize_call < owner_return, \
        f"{path}: stop owner no longer finalizes caches before native fallback"
    assert stop.count("callback->oAcceleratorStop)(that, provider)") == 2, \
        f"{path}: PF pass-through or sole VF native-stop call changed"
    assert "isLockedByCurrentThread" not in stop, \
        f"{path}: always-true Tahoe ownership stub used as a lock proof"
    media_abi = block(source,
        '// Read-only AVC ABI binding may precede TGL VF classification.',
        'KernelPatcher::SolveRequest lifecycle[]')
    for token in ('checkKernelArgument("-ngreenavcgen12")',
                  'gVfMediaPublicationAbiVerified = false;',
                  'const bool verified = gVfTaskCompletionAbiVerified && process && table && length',
                  'containsAnchor(process,0x57,sizeof(parseCall))',
                  'anchorMatches(process+0x57,parseCall,sizeof(parseCall))',
                  'containsAnchor(process,0x7b,sizeof(endCall))',
                  'anchorMatches(process+0x7b,endCall,sizeof(endCall))',
                  'anchorMatches(length,lengthLeaf,sizeof(lengthLeaf))',
                  'gVfMediaEndCaller = process + 0x81;',
                  'gVfMediaKernelMappedMemoryVtable = table + 16;',
                  'gVfMediaKernelMappedMemoryLength = length;'):
        assert token in media_abi, f"{path}: lost media publication ABI gate: {token}"
    for token in ('mapAddress && mapLength && mapDescriptor && mapOptions',
                  'anchorMatches(mapAddress,mapAddressLeaf,sizeof(mapAddressLeaf))',
                  'anchorMatches(mapLength,mapLengthLeaf,sizeof(mapLengthLeaf))',
                  'anchorMatches(mapDescriptor,mapDescriptorLeaf,sizeof(mapDescriptorLeaf))',
                  'anchorMatches(mapOptions,mapOptionsLeaf,sizeof(mapOptionsLeaf))',
                  'gVfMediaMapAddress = mapAddress;gVfMediaMapLength = mapLength;',
                  'gVfMediaMapDescriptor = mapDescriptor;gVfMediaMapOptions = mapOptions;'):
        assert token in media_abi, f"{path}: lost pure existing-alias getter provenance: {token}"
    assert media_abi.index('gVfMediaKernelMappedMemoryLength = length;') < \
        media_abi.index('OSSynchronizeIO();') < \
        media_abi.index('gVfMediaPublicationAbiVerified = verified;'), \
        f"{path}: media ABI readiness published before native handles"
    assert 'routeMultiple' not in media_abi and 'FunctionCast' not in media_abi, \
        f"{path}: binding-only stage installed/invoked a media submission route"
    for token in ('containsAnchor(submit,0xe2,sizeof(classBase))',
                  'anchorMatches(submit+0x106,usedStore,sizeof(usedStore))',
                  'anchorMatches(submit+0x13e,resourceBound,sizeof(resourceBound))',
                  'anchorMatches(submit+0x32a,nativeLock,sizeof(nativeLock))'):
        assert token in media_abi, f"{path}: unpinned native used-byte/lock ABI"
    media_install=block(source,'if (vfActive) {\n\t\t\tPANIC_COND(!this->ioPciConfigureInterrupts',
                        "// Tahoe's telemetry manager")
    for token in ('checkKernelArgument("-ngreenmediaadapter") && checkKernelArgument("-ngreenavcgen12")',
                  'gVfMediaPublicationRouteReady=false;',
                  'const bool qualified=gVfMediaPublicationAbiVerified && mediaEnd && videoEnd && videoTable',
                  'matches(mediaEnd,mediaThunk,sizeof(mediaThunk))',
                  'matches(videoEnd+0x124,descriptor,sizeof(descriptor))',
                  'matches(videoEnd+0x15f,countMinusEnd,sizeof(countMinusEnd))',
                  'matches(videoEnd+0x22e,nativeSubmit,sizeof(nativeSubmit))',
                  '0xA48)==videoEnd', '))==videoTable',
                  'PANIC_COND(!qualified', '!this->oVfMediaEnd',
                  'this->oVfMediaEnd=videoEnd;',
                  'gVfMediaPublicationRouteReady=true;'):
        assert token in media_install, f"{path}: unsafe AVC hook readiness: {token}"
    assert media_install.index('PANIC_COND(!qualified')<media_install.index('patcher.routeMultiple(index,mediaRoute')<\
        media_install.index('OSSynchronizeIO();')<media_install.index('gVfMediaPublicationRouteReady=true;')
    assert media_install.index('PANIC_COND(!qualified') < media_install.index('this->oVfMediaEnd=videoEnd;') < media_install.index('patcher.routeMultiple(index,mediaRoute')
    assert 'vfMediaEnd,this->oVfMediaEnd}' not in media_install, 'RIP-relative Media thunk must not be copied into a trampoline'
    media_submit = function_body(source, "IOReturn Gen11::vfContextSubmit(")
    assert media_submit.index('VfExternalProducerGuard guard') < \
        media_submit.index('VfMediaSubmissionScope mediaScope(that,accelerator,mediaEnabled);') < \
        media_submit.index('callback->oVfContextSubmit'), f"{path}: media scope escapes outer producer lease"
    for token in ('gVfIdentity == VfIdentity::Virtual', 'accelerator == gVfAccelerator',
                  'checkKernelArgument("-ngreenmediaadapter")', 'checkKernelArgument("-ngreenavcgen12")',
                  'if (!mediaScope)', 'return kIOReturnNoResources;'):
        assert token in media_submit, f"{path}: incomplete media outer scope gate: {token}"
    media_scope = function_body(source, 'VfMediaSubmissionScope(void *context, void *accelerator, bool enabled)')
    assert 'static VfMediaSubmission gVfMediaSubmissions[32] {};' in source, \
        f"{path}: unbounded media submission scratch slots"
    for token in ('if (!enabled) return;', '!gVfMediaPublicationAbiVerified', 'entry.thread == owner',
                  'OSCompareAndSwapPtr(nullptr,owner,&entry.thread)',
                  'IOMallocZero(sizeof(NgreenAvcGen12::PublishScratch))',
                  'static_cast<OSObject *>(context)->retain();',
                  'static_cast<OSObject *>(accelerator)->retain();'):
        assert token in media_scope, f"{path}: lost real kernel submission scope ownership: {token}"
    assert media_scope.index('if (!enabled) return;') < media_scope.index('current_thread()') < \
        media_scope.index('IOMallocZero('), f"{path}: PF/pass-through allocated media scratch"
    assert 'getMember<void *>(shared,' not in media_scope, f"{path}: shared followed before native lock"
    locked_media = function_body(source, 'static bool bindLocked(')
    for token in ('caller != gVfMediaEndCaller', 'entry.translating',
                  'stream != static_cast<uint8_t *>(entry.context)+0x650',
                  'entry.thread != reinterpret_cast<void *>(current_thread())',
                  'getMember<void *>(entry.context,0x578)',
                  'getMember<void *>(entry.context,0x5A0)',
                  'getMember<void *>(shared,0x80) != task', '!getMember<void *>(shared,0x88)',
                  'entry.gpuTask != gpuTask'):
        assert token in locked_media, f"{path}: missing locked media identity gate: {token}"
    media_exit = function_body(source, '~VfMediaSubmissionScope()')
    media_map=function_body(source,'static bool vfCaptureMediaMappedSpan(')
    latch='if (getMember<volatile uint8_t>(stream,0x3C)!=1U)return false;'
    assert latch in media_map, f'{path}: failed native stream can reach publication'
    assert media_map.index('getMember<void *>(entry.shared,0x88)!=entry.gpuTask)return false;') < \
        media_map.index(latch) < media_map.index('out.reason="class-ledger"'), \
        f'{path}: native stream latch must follow exact owner identity admission'
    for token in ('!gVfNativeBatchMapVtable || !gVfMediaSysMemoryVtable',
                  'getMember<mach_vm_address_t>(gpuMap,0)!=gVfNativeBatchMapVtable',
                  'getMember<void *>(gpuMap,0x90)!=entry.gpuTask',
                  'getMember<void *>(gpuMap,0x18)!=backing',
                  'getMember<mach_vm_address_t>(backing,0)!=gVfMediaSysMemoryVtable',
                  'getMember<void *>(backing,0xD0)!=descriptor'):
        assert token in media_map, f'{path}: lost CPU/GPU descriptor admission: {token}'
    assert media_map.index('getMember<mach_vm_address_t>(backing,0)!=gVfMediaSysMemoryVtable') < \
        media_map.index('getMember<void *>(backing,0xD0)!=descriptor')
    rotation='if (getMember<volatile uint8_t>(gpuMap,0x11C)!=0U)return false;'
    assert rotation in media_map, f'{path}: rotated command map admitted as linear'
    for token in ('!gVfNativeBatchMapGetter',
                  '(getMember<volatile uint32_t>(gpuMap,0x10)&0x40U)',
                  'getMember<volatile uint64_t>(gpuMap,0x98)',
                  'gpuBase>UINT64_MAX-bytes', 'bytes>backingBytes',
                  'getMember<volatile uint64_t>(backing,0x40)',
                  'out.gpuMap=gpuMap;out.gpuBase=gpuBase;'):
        assert token in media_map, f'{path}: missing GPU address-mode/span proof: {token}'
    assert media_map.index('getMember<void *>(backing,0xD0)!=descriptor') < \
        media_map.index(rotation) < media_map.index('out.reason="map-getters"'), \
        f'{path}: rotation read must follow exact map identity'
    assert 'matches(sysPhysical,sysPhysicalBody,sizeof(sysPhysicalBody))' in source
    assert 'matches(sysLength,sysLengthBody,sizeof(sysLengthBody))' in source
    assert 'getMember<mach_vm_address_t>(reinterpret_cast<void *>(sysTable+16U),0x120)==sysLength' in source
    assert '0x170U<=size-(sysTable-address)' in source
    assert 'getMember<mach_vm_address_t>(reinterpret_cast<void *>(sysTable+16U),0x158)==sysPhysical' in source
    assert 'if (getMember<uint32_t>(context,0x6DC)!=1)return false;' in media_map
    assert 'anchorMatches(submit+0x10b,optionalRecordSkip,sizeof(optionalRecordSkip))' in source
    assert 'anchorMatches(submit+0x168,nextRecord,sizeof(nextRecord))' in source
    for token in ('getMember<uint32_t>(context,0x6DC)!=1',
                  'getMember<void *>(context,0x708)!=resource',
                  'getMember<uint8_t>(resource,0x14)!=0x0A',
                  '!getMember<uint32_t>(mapped,0x38)',
                  'getMember<mach_vm_address_t>(mapped,0)!=gVfMediaKernelMappedMemoryVtable',
                  'getMember<mach_vm_address_t>(table,0x118)!=gVfMediaMapAddress',
                  'getMember<mach_vm_address_t>(table,0x128)!=gVfMediaMapLength',
                  'getMember<mach_vm_address_t>(table,0x138)!=gVfMediaMapDescriptor',
                  'getMember<mach_vm_address_t>(table,0x140)!=gVfMediaMapOptions',
                  'gVfMediaMapDescriptor)(map)!=descriptor',
                  'gVfMediaMapOptions)(map)&kIOMapReadOnly',
                  'getMember<uintptr_t>(context,0x658)!=base',
                  'used=getMember<uint32_t>(context,0x6F4)',
                  'bytes!=resourceBytes', 'offset>used || used>bytes',
                  'out.reservedWords=(used-offset)/4U'):
        assert token in media_map, f"{path}: missing native AVC mapped-span proof: {token}"
    assert 'getKernelData(' not in media_map and 'map(' not in media_map
    media_end=function_body(source,'void Gen11::vfMediaEnd(')
    for token in ('__builtin_return_address(0)',
                  'VfMediaSubmissionScope::bindLocked(*entry,stream,caller)',
                  'span.availableWords,span.reservedWords',
                  'words,span.count,1920,1080,*entry->scratch,live,same)',
                  'publication.mustStop', 'current.resource==span.resource',
                  'current.mapped==span.mapped', 'current.map==span.map',
                  'current.gpuMap==span.gpuMap', 'current.gpuBase==span.gpuBase',
                  'current.backingBytes==span.backingBytes',
                  'current.used==span.used', 'current.commands==span.commands'):
        assert token in media_end, f"{path}: missing native AVC publication ownership: {token}"
    assert media_end.index('bindLocked(')<media_end.index('NgreenAvcGen12::publish(')<\
        media_end.index('OSSynchronizeIO();')<media_end.rindex('FunctionCast(vfMediaEnd,'), \
        f"{path}: AVC publication/native submission ordering changed"
    assert 'if (entry->failed) return;' in media_end, f'{path}: lost sticky submission failure'
    assert media_end.index('VfMediaSubmissionScope::find(that)') < \
        media_end.index('if (entry->failed) return;') < media_end.index('bindLocked('), \
        f'{path}: failed submission can reach publication'
    rejected=media_end[media_end.index('if (!publication.published)'):media_end.index('OSSynchronizeIO();')]
    assert 'entry->failed=true;' in rejected and 'return;' in rejected and 'FunctionCast' not in rejected
    assert media_end.count('FunctionCast(vfMediaEnd,')==2
    for token in ('static volatile UInt32 publicationRecorded=0;',
                  'OSCompareAndSwap(0,1,&publicationRecorded)',
                  'const uint32_t sourceWords=span.count;',
                  'first<count;first+=48U', 'uint32_t block[48] {};',
                  'valid=count-first<48U?count-first:48U',
                  'block[i]=kind?span.commands[first+i]:entry->scratch->source[first+i]',
                  'NGVfTimeoutObservation::encodeRingPrefix(block,encoded)',
                  'V372 AVC_CPU_BEGIN', 'V372 AVC_CPU_BLOCK', 'V372 AVC_CPU_END'):
        assert token in media_end, f'{path}: missing bounded pinned CPU publication record: {token}'
    assert media_end.index('Lost owned AVC mapping after count publication') < media_end.index('V372 AVC_CPU_BEGIN') < media_end.index('V372 AVC_CPU_END') < media_end.rindex('FunctionCast(vfMediaEnd,')
    final_guard = FINAL_MEDIA_GUARD
    assert final_guard in media_end, 'missing post-diagnostic native-submit revalidation'
    final_tail = media_end[media_end.index('V372 AVC_CPU_END'):]
    assert final_tail.index('OSSynchronizeIO();') < final_tail.index(final_guard) < final_tail.rindex('FunctionCast(vfMediaEnd,')
    after_guard = final_tail[final_tail.index(final_guard) + len(final_guard):]
    assert after_guard.strip() == 'FunctionCast(vfMediaEnd,callback->oVfMediaEnd)(that,stream);\n}', \
        'native-submit revalidation must immediately precede native continuation'
    assert 'return mediaScope.failed()?kIOReturnBadArgument:result;' in media_submit
    assert 'slot->thread != owner || slot->translating' in media_exit
    assert 'releaseSlot();' in media_exit
    media_release = function_body(source, 'void releaseSlot()')
    for token in ('bzero(scratch',
                  'IOFree(scratch', 'OSCompareAndSwapPtr(owner,nullptr,&slot->thread)',
                  'static_cast<OSObject *>(context)->release();',
                  'static_cast<OSObject *>(accelerator)->release();'):
        assert token in media_release, f"{path}: incomplete media scope exit: {token}"
    assert 'entry.thread == owner || entry.context == context' in media_scope, \
        f"{path}: overlapping native used-bytes ledger ownership"
    assert media_scope.index('IOSimpleLockUnlockEnableInterrupt(') < media_scope.index('IOMallocZero(')
    assert media_release.index('IOSimpleLockUnlockEnableInterrupt(') < media_release.index('IOFree(')
    assert media_release.index('IOFree(') < media_release.index('OSCompareAndSwapPtr(owner,nullptr,')
    print("PASS counted VF external-producer source/route/teardown contract")


def mutation_contract(source, path):
    mutations = (
        ('if (getMember<volatile uint8_t>(stream,0x3C)!=1U)return false;', '', 1),
        ('if (getMember<volatile uint8_t>(stream,0x3C)!=1U)return false;',
         'if (getMember<volatile uint8_t>(stream,0x3C)==1U)return false;', 1),
        ('if (getMember<volatile uint8_t>(stream,0x3C)!=1U)return false;',
         'if (!getMember<volatile uint8_t>(stream,0x3C))return false;', 1),
        ('getMember<void *>(backing,0xD0)!=descriptor', 'false', 1),
        ('(getMember<volatile uint32_t>(gpuMap,0x10)&0x40U)', 'false', 1),
        ('gpuBase>UINT64_MAX-bytes', 'false', 1),
        ('bytes>backingBytes', 'false', 1),
        ('current.backingBytes==span.backingBytes', 'true', 1),
        ('matches(sysLength,sysLengthBody,sizeof(sysLengthBody))', 'true', 1),
        ('current.gpuBase==span.gpuBase', 'true', 1),
        ('current.gpuMap==span.gpuMap', 'true', 1),
        ('if (getMember<volatile uint8_t>(gpuMap,0x11C)!=0U)return false;', '', 1),
        ('if (getMember<volatile uint8_t>(gpuMap,0x11C)!=0U)return false;',
         'if (getMember<volatile uint8_t>(gpuMap,0x11C)==0U)return false;', 1),
        ('getMember<void *>(gpuMap,0x18)!=backing', 'false', 1),
        ('getMember<void *>(gpuMap,0x90)!=entry.gpuTask', 'false', 1),
        ('getMember<mach_vm_address_t>(backing,0)!=gVfMediaSysMemoryVtable', 'false', 1),
        ('matches(sysPhysical,sysPhysicalBody,sizeof(sysPhysicalBody))', 'true', 1),
        ('0x170U<=size-(sysTable-address)', 'true', 1),
        ('if (entry->failed) return;', '', 1),
        ('if (entry->failed) return;', 'if (entry->failed) {}', 1),
        ('"Lost owned AVC mapping before native submission"',
         '"missing final submit guard"', 1),
        (FINAL_MEDIA_GUARD, '', 1),
        (FINAL_MEDIA_GUARD,
         'PANIC_COND(false,"ngreen",\n\t\t"Lost owned AVC mapping before native submission");', 1),
        ('!NgreenAvcGen12::matchesPublishedBytes(', 'false && !NgreenAvcGen12::matchesPublishedBytes(', 1),
        ('words,span.count,1920,1080,*entry->scratch,live,same)',
         'words,words,1920,1080,*entry->scratch,live,same)', 1),
        ('"Lost owned AVC mapping before native submission");',
         '"Lost owned AVC mapping before native submission");\n\tSYSLOG("ngreen","late diagnostic");', 1),
        ('gVfMediaPublicationRouteReady=true;','gVfMediaPublicationRouteReady=false;',1),
        ('0xA48)==videoEnd','0xA48)!=videoEnd',1),
        ('matches(videoEnd+0x22e,nativeSubmit,sizeof(nativeSubmit))','true',1),
        ('getMember<void *>(context,0x708)!=resource','false',1),
        ('getMember<uint8_t>(resource,0x14)!=0x0A','getMember<uint8_t>(resource,0x14)!=0x0B',1),
        ('!getMember<uint32_t>(mapped,0x38)','false',1),
        ('used=getMember<uint32_t>(context,0x6F4)','used=getMember<uint32_t>(context,0x660)',1),
        ('span.availableWords,span.reservedWords','span.availableWords,span.availableWords',1),
        ('__builtin_return_address(0)','__builtin_return_address(1)',1),
        ('gVfMediaPublicationRouteReady && gVfMediaPublicationAbiVerified',
         'true && gVfMediaPublicationAbiVerified',1),
        ('static bool gVfMediaPublicationRouteReady = false;',
         'static bool gVfMediaPublicationRouteReady = true;',1),
        ('NGVfAvcPublication::inlineShape(args,kIOExternalMethodArgumentsCurrentVersion,',
         'NGVfDeviceObservation::inlineShape(args,kIOExternalMethodArgumentsCurrentVersion,',1),
        ('gVfMediaMapAddress = mapAddress;', 'gVfMediaMapAddress = mapLength;'),
        ('anchorMatches(mapOptions,mapOptionsLeaf,sizeof(mapOptionsLeaf))', 'true'),
        ('getMember<void *>(entry.context,0x578)', 'getMember<void *>(entry.context,0x580)'),
        ('getMember<void *>(shared,0x80) != task', 'false'),
        ('VfMediaSubmissionScope mediaScope(that,accelerator,mediaEnabled);',
         'VfMediaSubmissionScope mediaScope(nullptr,accelerator,mediaEnabled);'),
        ('IOMallocZero(sizeof(NgreenAvcGen12::PublishScratch))', 'nullptr'),
        ('caller != gVfMediaEndCaller', 'false'),
        ('stream != static_cast<uint8_t *>(entry.context)+0x650', 'false'),
        ('gVfMediaEndCaller = process + 0x81;', 'gVfMediaEndCaller = process + 0x80;'),
        ('gVfMediaKernelMappedMemoryVtable = table + 16;', 'gVfMediaKernelMappedMemoryVtable = table;'),
        ('checkKernelArgument("-ngreenavcgen12")', 'true'),
        ("selector == NGVfDeviceObservation::selector", "selector >= NGVfDeviceObservation::selector", 1),
        ("accelerator && accelerator == gVfAccelerator", "accelerator", 1),
        ('checkKernelArgument("-ngreenmediaadapter"))\n\t\treturn vfReadOwnedDeviceObservation',
         'true)\n\t\treturn vfReadOwnedDeviceObservation', 1),
        ("if (!gVfDeviceObservationAbiVerified)", "if (false)", 1),
        ("!NGVfDeviceObservation::inlineShape(args,", "!true /* shape bypass */ || (args,", 1),
        ("out.restartReports = getMember<UInt32>(accelerator, 0x9DC);",
         "out.restartReports = getMember<UInt32>(accelerator, 0x9D8);", 1),
        ("out.terminated = getMember<UInt32>(accelerator, 0xDC8);",
         "out.terminated = getMember<UInt32>(accelerator, 0xDC0);", 1),
        ("while (used < 16)", "while (true)", 1),
        ("if (selector != 0 && result != kIOReturnSuccess)", "if (otherTicket && result != kIOReturnSuccess)", 1),
        ("OSCompareAndSwap(used, used + 1, &observedOtherFailures)", "OSCompareAndSwap(used, used + 1, &observedOtherMethods)", 1),
        ("V360: shared resource method return", "REMOVED non-create return", 1),
        ("args->structureOutput && !args->structureOutputDescriptor", "args->structureOutput", 1),
        ("args->structureOutputSize >= 0x58 && args->structureOutputSize <= 0x1000", "true", 1),
        ("static_cast<const uint8_t *>(args->structureOutput) + 0x10", "static_cast<const uint8_t *>(args->structureOutput) + 0x08", 1),
        ("if (!inlineShape || !ro || !rw)", "if (ticket && (!inlineShape || !ro || !rw))", 1),
        ("if (selector == 0 && result != kIOReturnSuccess)", "if (ticket && result != kIOReturnSuccess)", 1),
        ("while (used < 8)", "while (true)", 1),
        ("OSCompareAndSwap(used, used + 1, &observedFailures)",
         "OSCompareAndSwap(used, used + 1, &observedCreates)", 1),
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
        ("vfOwnsDeviceCacheRetirement()",
         "true /* cross-thread retirement bypass */", 1),
        ("ThreadLocal<UInt32, 1> gVfDeviceCacheRetirementDepth",
         "volatile UInt32 gVfDeviceCacheRetirementDepth", 1),
        ("NGVfAcceleratorStop::ownerReturnAction(phase, true)",
         "NGVfAcceleratorStop::ownerReturnAction(phase, false)", 1),
        ("NGVfAcceleratorStop::Phase::NativeStopComplete",
         "NGVfAcceleratorStop::Phase::Finalized", 1),
        ("entry.task!=task", "false", 1),
        ("entry.submissionCoverage.coveredStamp!=expected", "false", 1),
        ("slot-first<16", "true", 1),
        ("seen!=required || !live()", "false", 1),
        ("NGVfTaskCompletion::validRequest(request)", "true", 1),
        ("getMember<void *>(shared,0x88)", "getMember<void *>(shared,0x80)", 1),
        ("!gVfTaskCompletionAbiVerified", "false", 1),
        ("entry.submissionCoverage.coveredSerial", "entry.submissionCoverage.serial", 1),
        ('if (getMember<uint32_t>(context,0x6DC)!=1)return false;',
         'if (getMember<uint32_t>(context,0x6DC)!=1 || !(getMember<uint8_t>(context,0x750)&1))return false;',1),
        ('anchorMatches(submit+0x10b,optionalRecordSkip,sizeof(optionalRecordSkip))','true',1),
        ('anchorMatches(submit+0x168,nextRecord,sizeof(nextRecord))','true',1),
        ('this->oVfMediaEnd=videoEnd;', 'this->oVfMediaEnd=mediaEnd;', 1),
        ('Info",vfMediaEnd}', 'Info",vfMediaEnd,this->oVfMediaEnd}', 1),
        ('OSCompareAndSwap(0,1,&publicationRecorded)', 'true', 1),
        ('first<count;first+=48U', 'first<count;first+=24U', 1),
        ('valid=count-first<48U?count-first:48U', 'valid=48U', 1),
        ('after.gpuMap==span.gpuMap', 'true', 1),
        ('after.gpuBase==span.gpuBase', 'true', 1),
        ('after.backingBytes==span.backingBytes', 'true', 1),
        ('after.availableWords==span.availableWords', 'true', 1),
        ('after.reservedWords==span.reservedWords', 'true', 1),
        ('getMember<void *>(map,0x18)==mapMemory', 'true', 1),
        ('block[i]=kind?span.commands[first+i]:entry->scratch->source[first+i]', 'block[i]=entry->scratch->produced[first+i]', 1),
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
    adapter = pathlib.Path(__file__).resolve().parent / "media_adapter/adapter.cpp"
    adapter_frame_mutations(adapter.read_text())
    repo = pathlib.Path(__file__).resolve().parent.parent
    avc_phase_mutations((repo / "NootedGreen/kern_avc_gen12_commands.hpp").read_text(),
                        (repo / "tools/media_adapter/native_avc_reservation.hpp").read_text())


if __name__ == "__main__":
    main()
