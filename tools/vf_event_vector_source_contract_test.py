#!/usr/bin/env python3
"""Source/API contract for all exact-address VF event-vector growth guards."""
import sys
from pathlib import Path
from route_symbol_contract_test import macho_symbols, single_symbol


GEN11_REQUIRED = (
    '"__ZN16IGAccel2DContext8blitCopyEP12IOAccelEventP16IOAccelResource2S3_P22IOAccel2DBlitRectStrucj", twoDEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccel2DContext.cpp", twoDEventOwnerEnd',
    '"__ZN19IGBlockFenceManagerD0Ev", acceleratorEventOwnerStart',
    '"__ZL18kDisplayVar_sysctlP10sysctl_oidPviP10sysctl_req", acceleratorEventOwnerEnd',
    '"__Z25process_MSAABufferResolveR26IGAccelSegmentResourceListR24IGAccelCommandDescriptorR22IOGraphicsAccelerator2RKN14IntelMTLRender20sResolveResourceDescES8_NS5_14eResolveFilterE10BufferTypeR12IOAccelEvent", renderEventOwnerStart',
    '"__Z40surfaceStateFillMemoryObjectControlStateR22SGfxRenderSurfaceStateP15IGAccelResourceP15IGMemoryManager", renderEventOwnerEnd',
    '"__ZN21IntelMTLBlitFunctions7executeEN12IntelMTLBlit7eTokensER19IGAccelCommandQueueR26IGAccelSegmentResourceListRK20IOAccelKernelCommandR24IGAccelCommandDescriptorR13IGHeapsAccessR22IOGraphicsAccelerator2R17IGHardwareContextR12IOAccelEvent", blitEventOwnerStart',
    '"__ZN14IGTelemetryKMD4initEP16IntelAcceleratory", blitEventOwnerEnd',
    '"__ZN16IGAccelGLContext32process_token_ResolveDepthBufferER24IOAccelCommandStreamInfo", glEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccelGLContext.cpp", glEventOwnerEnd',
    '"__ZN15IGAccelResource22updateMappingCacheTypeEj", resourceEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccelResource.cpp", resourceEventOwnerEnd',
    '"__ZN23IGAccelSharedUserClient9MetaClassD0Ev", sharedEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccelSharedUserClient.cpp", sharedEventOwnerEnd',
    '"__ZN14IGAccelSurface9MetaClassD0Ev", surfaceEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccelSurface.cpp", surfaceEventOwnerEnd',
    "twoDEventOwnerStart, twoDEventOwnerEnd, 0xA74",
    "acceleratorEventOwnerStart, acceleratorEventOwnerEnd, 0x244",
    "renderEventOwnerStart, renderEventOwnerEnd, 0x764",
    "blitEventOwnerStart, blitEventOwnerEnd, 0x16C8",
    "glEventOwnerStart, glEventOwnerEnd, 0x4AC8",
    "resourceEventOwnerStart, resourceEventOwnerEnd, 0x60C",
    "sharedEventOwnerStart, sharedEventOwnerEnd, 0xA",
    "surfaceEventOwnerStart, surfaceEventOwnerEnd, 0xA",
    "twoDEventGrow, vf2DEventVectorGrow, this->oVf2DEventVectorGrow",
    "acceleratorEventGrow, vfAcceleratorEventVectorGrow,",
    "renderEventGrow, vfRenderEventVectorGrow, this->oVfRenderEventVectorGrow",
    "blitEventGrow, vfBlitEventVectorGrow, this->oVfBlitEventVectorGrow",
    "glEventGrow, vfGLEventVectorGrow, this->oVfGLEventVectorGrow",
    "resourceEventGrow, vfResourceEventVectorGrow,",
    "sharedEventGrow, vfSharedEventVectorGrow, this->oVfSharedEventVectorGrow",
    "surfaceEventGrow, vfSurfaceEventVectorGrow, this->oVfSurfaceEventVectorGrow",
    "PANIC_COND(!routeExactMultiple(patcher, eventGrowRoutes)",
    '"V274: guarded all eight VF event-vector growth copies"',
    "if (gVfIdentity == VfIdentity::Virtual) {\n\t\tconst size_t vectorSize =",
    "NGEventVector::hasConsistentState(",
    "NGEventVector::hasRepresentableRequest(requested)",
    'vfMarkProtocolFault("VF event-vector growth received unsafe pre-state")',
    '"V273: refusing unsafe VF event vector size=%llu capacity=%llu request=%llu"',
    "const bool nativeResult = native(vector, requested);",
    "if (gVfIdentity != VfIdentity::Virtual)",
    "NGEventVector::hasCapacity(",
    'vfMarkProtocolFault("VF event-vector growth did not publish requested capacity")',
    '"V273: refusing incomplete VF event vector size=%llu capacity=%llu request=%llu"',
)

WRAPPERS = (
    "vf2DEventVectorGrow",
    "vfAcceleratorEventVectorGrow",
    "vfRenderEventVectorGrow",
    "vfBlitEventVectorGrow",
    "vfGLEventVectorGrow",
    "vfResourceEventVectorGrow",
    "vfSharedEventVectorGrow",
    "vfSurfaceEventVectorGrow",
)

PATCHER_HEADER_REQUIRED = (
    "struct ExactRouteRequest",
    "const char *identity {nullptr};",
    "mach_vm_address_t from {0};",
    "mach_vm_address_t *org {nullptr};",
    "bool routeExactMultiple(KernelPatcher &patcher, ExactRouteRequest *requests,",
)

PATCHER_SOURCE_REQUIRED = (
    "if (!requests && count)",
    "if (!requests[i].identity || !requests[i].from || !requests[i].to ||",
    "if (requests[i].from == requests[j].from)",
    "patcher.clearError();",
    "const mach_vm_address_t wrapper = patcher.routeFunction(",
    "requests[i].from, requests[i].to, true, true, true);",
    "patcher.getError() != KernelPatcher::Error::NoError",
    "*requests[i].org = wrapper;",
)

GROW_SYMBOL = "__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm"
OWNER_LAYOUTS = (
    ("__ZN16IGAccel2DContext8blitCopyEP12IOAccelEventP16IOAccelResource2S3_P22IOAccel2DBlitRectStrucj", "__GLOBAL__sub_I_IGAccel2DContext.cpp", 0xA74),
    ("__ZN19IGBlockFenceManagerD0Ev", "__ZL18kDisplayVar_sysctlP10sysctl_oidPviP10sysctl_req", 0x244),
    ("__Z25process_MSAABufferResolveR26IGAccelSegmentResourceListR24IGAccelCommandDescriptorR22IOGraphicsAccelerator2RKN14IntelMTLRender20sResolveResourceDescES8_NS5_14eResolveFilterE10BufferTypeR12IOAccelEvent", "__Z40surfaceStateFillMemoryObjectControlStateR22SGfxRenderSurfaceStateP15IGAccelResourceP15IGMemoryManager", 0x764),
    ("__ZN21IntelMTLBlitFunctions7executeEN12IntelMTLBlit7eTokensER19IGAccelCommandQueueR26IGAccelSegmentResourceListRK20IOAccelKernelCommandR24IGAccelCommandDescriptorR13IGHeapsAccessR22IOGraphicsAccelerator2R17IGHardwareContextR12IOAccelEvent", "__ZN14IGTelemetryKMD4initEP16IntelAcceleratory", 0x16C8),
    ("__ZN16IGAccelGLContext32process_token_ResolveDepthBufferER24IOAccelCommandStreamInfo", "__GLOBAL__sub_I_IGAccelGLContext.cpp", 0x4AC8),
    ("__ZN15IGAccelResource22updateMappingCacheTypeEj", "__GLOBAL__sub_I_IGAccelResource.cpp", 0x60C),
    ("__ZN23IGAccelSharedUserClient9MetaClassD0Ev", "__GLOBAL__sub_I_IGAccelSharedUserClient.cpp", 0xA),
    ("__ZN14IGAccelSurface9MetaClassD0Ev", "__GLOBAL__sub_I_IGAccelSurface.cpp", 0xA),
)


def accepts_gen11(source: str) -> bool:
    if any(fragment not in source for fragment in GEN11_REQUIRED):
        return False
    if source.count(GROW_SYMBOL) != 8:
        return False
    if any(stale in source for stale in (
        "eventGrowSymbol", "resourceEventHelpers", "sharedEventHelpers",
        "resourceEventGrowRoute", "sharedEventGrowRoute",
    )):
        return False
    begin = source.index("mach_vm_address_t twoDEventOwnerStart")
    end = source.index("KernelPatcher::RouteRequest workQueueInitRoute", begin)
    install = source[begin:end]
    if not (install.index("eventOwnerBounds") <
            install.index("locateReviewedGrow(") <
            install.index("Changed VF event-vector growth contracts") <
            install.index("ExactRouteRequest eventGrowRoutes") <
            install.index("routeExactMultiple(patcher, eventGrowRoutes)")):
        return False
    helper_begin = source.index("static bool vfEventVectorGrowChecked(")
    helper_end = source.index("bool Gen11::vf2DEventVectorGrow", helper_begin)
    helper = source[helper_begin:helper_end]
    if not (helper.index("hasConsistentState(") <
            helper.index("VF event-vector growth received unsafe pre-state") <
            helper.index("refusing unsafe VF event vector") <
            helper.index("native(vector, requested)") <
            helper.index("gVfIdentity != VfIdentity::Virtual") <
            helper.index("hasCapacity(") <
            helper.index("VF event-vector growth did not publish requested capacity") <
            helper.index("refusing incomplete VF event vector")):
        return False
    for wrapper in WRAPPERS:
        begin = source.index(f"bool Gen11::{wrapper}(")
        end = source.index("\n}", begin)
        body = source[begin:end]
        original = "oVf" + wrapper[2:]
        if f"reinterpret_cast<Grow>(callback->{original})" not in body:
            return False
    return True


def accepts_patcher(header: str, source: str, lilu: str) -> bool:
    if any(fragment not in header for fragment in PATCHER_HEADER_REQUIRED):
        return False
    if any(fragment not in source for fragment in PATCHER_SOURCE_REQUIRED):
        return False
    normalized = " ".join(lilu.split())
    ranged_lookup = (
        "inline T solveSymbol(size_t id, const char *symbol, mach_vm_address_t start, "
        "size_t size, bool crash=false) { auto addr = solveSymbol(id, symbol);"
    )
    return ranged_lookup in normalized


def verify_payload(path: Path) -> None:
    symbols = macho_symbols(path)
    grows = sorted(symbols.get(GROW_SYMBOL, []))
    resolved = []
    for start_name, end_name, offset in OWNER_LAYOUTS:
        start = single_symbol(symbols, start_name, path)
        end = single_symbol(symbols, end_name, path)
        target = start + offset
        assert start < target and target + 0x78 <= end, \
            f"{path}: event-vector target escapes unique owner bounds"
        resolved.append(target)
    assert sorted(resolved) == grows, \
        f"{path}: exact owner offsets do not cover all event-vector grow copies"


def main() -> None:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: vf_event_vector_source_contract_test.py "
            "kern_gen11.cpp kern_patcherplus.hpp kern_patcherplus.cpp "
            "kern_patcher.hpp accelerator_payload accelerator_payload"
        )
    gen11_path, plus_header_path, plus_source_path, lilu_path = map(Path, sys.argv[1:5])
    gen11 = gen11_path.read_text(encoding="utf-8")
    plus_header = plus_header_path.read_text(encoding="utf-8")
    plus_source = plus_source_path.read_text(encoding="utf-8")
    lilu = lilu_path.read_text(encoding="utf-8")
    assert accepts_gen11(gen11), "missing or reordered VF event-vector contract"
    assert accepts_patcher(plus_header, plus_source, lilu), \
        "missing exact-route or Lilu duplicate-symbol API contract"
    for payload in map(Path, sys.argv[5:]):
        verify_payload(payload)
    mutations = 0
    for fragment in GEN11_REQUIRED:
        assert gen11.count(fragment) == 1, f"ambiguous source contract: {fragment}"
        assert not accepts_gen11(gen11.replace(fragment, "MUTATED", 1)), \
            f"mutation escaped source contract: {fragment}"
        mutations += 1
    for fragment in PATCHER_HEADER_REQUIRED:
        assert plus_header.count(fragment) == 1, f"ambiguous patcher header: {fragment}"
        assert not accepts_patcher(
            plus_header.replace(fragment, "MUTATED", 1), plus_source, lilu
        ), f"mutation escaped patcher header contract: {fragment}"
        mutations += 1
    for fragment in PATCHER_SOURCE_REQUIRED:
        assert plus_source.count(fragment) == 1, f"ambiguous patcher source: {fragment}"
        assert not accepts_patcher(
            plus_header, plus_source.replace(fragment, "MUTATED", 1), lilu
        ), f"mutation escaped patcher source contract: {fragment}"
        mutations += 1
    print(
        f"PASS: eight exact VF event-vector routes and {mutations} rejected mutations"
    )


if __name__ == "__main__":
    main()
