#!/usr/bin/env python3
"""Source contract for owner-scoped VF command-pool repairs."""
import sys
from pathlib import Path


REQUIRED = (
    "NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid",
    "ngVfCommandPoolInitBridge, gIOAccelCommandPoolInit",
    "getMember<uint64_t>(pool, NGIOAccelCommandPool::recordOffset) = 0;",
    '"testb %al, %al\\n\\t"',
    '"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv", growthStart',
    "NGIOAccelCommandPool::hasReviewedGrowthContract(",
    "NGIOAccelCommandPool::hasReviewedGetBufferContract(",
    '"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv",\n\t\t\t vfAllocMoreCommandBuffers, this->oIOAccelAllocMoreCommandBuffers',
    '"__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj",\n\t\t\t vfGetCommandBufferPtrNoInc, this->oIOAccelGetCommandBufferPtrNoInc',
    "callback->oIOAccelAllocMoreCommandBuffers)(pool)",
    "NGIOAccelCommandPool::completedGrowth(",
    "NGIOAccelCommandPool::hasReviewedExtendedInitContract(",
    "NGIOAccelCommandPool::extendedInitReplace, 1",
    "NGIOAccelCommandPool::hasReviewedRectListCapacity(",
    "NGIOAccelCommandPool::rectListCapacityReplace, 1",
    "NGIOAccelCommandPool::hasReviewedResolveHizCapacity(",
    "NGIOAccelCommandPool::resolveHizCapacityReplace, 1",
    '"V268: rejecting incomplete VF command-pool growth old=%u new=%u current=%d"',
    '"V269: guarded VF extended-context pool construction"',
    '"V270: bounded VF rect-list command requests to 0xfff8 bytes"',
    '"V271: require full usable VF resolve-HIZ command capacity"',
    '"V272: guarded VF command-pool growth and returned capacity"',
    "NGIOAccelCommandPool::hasReturnedCapacity(",
    'vfMarkProtocolFault("VF command-pool getter returned inadequate capacity")',
    '"V272: refusing inadequate VF command buffer request=%u current=%d count=%u"',
    "gVfAccelerator = that;",
)

OWNER = (
    "const bool target = pool && gVfIdentity == VfIdentity::Virtual &&",
    "getMember<void *>(pool, NGIOAccelCommandPool::acceleratorOffset) ==\n\t\t\tgVfAccelerator",
)


def accepts(source: str) -> bool:
    if any(fragment not in source for fragment in REQUIRED):
        return False
    io_start = source.index("if (index == kextIOAcceleratorFamily2.loadIndex)")
    io_end = source.index("const bool physicalFramebuffer", io_start)
    io = source[io_start:io_end]
    if not (io.index("ioAcceleratorTahoe25G229Uuid") <
            io.index("hasReviewedGrowthContract") <
            io.index("hasReviewedGetBufferContract") <
            io.index("commandPoolRoutes") < io.index("routeMultiple")):
        return False
    if io.index("ngVfCommandPoolInitBridge") > io.index("vfAllocMoreCommandBuffers"):
        return False
    helper_start = source.index("bool ngVfCommandPoolInitHelper(")
    helper_end = source.index("bool ngVfCommandPoolInitBridge(", helper_start)
    helper = source[helper_start:helper_end]
    if helper.index("recordOffset") > helper.index("gIOAccelCommandPoolInit)("):
        return False
    wrapper_start = source.index("bool Gen11::vfAllocMoreCommandBuffers(void *pool)")
    wrapper_end = source.index("void *Gen11::vfGetCommandBufferPtrNoInc(", wrapper_start)
    wrapper = source[wrapper_start:wrapper_end]
    if any(fragment not in wrapper for fragment in OWNER) or not (
            wrapper.index("previousCount") < wrapper.index("FunctionCast(") <
            wrapper.index("publishedCount") < wrapper.index("completedGrowth(")):
        return False
    getter_start = source.index("void *Gen11::vfGetCommandBufferPtrNoInc(")
    getter_end = source.index("bool Gen11::IGMemoryManagerInitSegments", getter_start)
    getter = source[getter_start:getter_end]
    if any(fragment not in getter for fragment in OWNER) or not (
            getter.index("const bool target") < getter.index("FunctionCast(") <
            getter.index("if (!target)") < getter.index("hasReturnedCapacity(") <
            getter.index("vfMarkProtocolFault(") < getter.index("PANIC_COND(true")):
        return False
    start_start = source.index("bool Gen11::start(void *that, void *provider)")
    start_end = source.index("void Gen11::acceleratorStop", start_start)
    start = source[start_start:start_end]
    if start.index("gVfAccelerator = that;") > start.index("FunctionCast(start"):
        return False
    driver_start = source.rfind("if (vfActive) {", 0, source.index("bufferAccessors[]"))
    if driver_start < 0:
        return False
    driver_end = source.index("// Tahoe's GuC factory", driver_start)
    driver = source[driver_start:driver_end]
    return (driver.index("hasReviewedExtendedInitContract") <
            driver.index("extendedInitPatch.apply") <
            driver.index("hasReviewedRectListCapacity") <
            driver.index("rectListCapacityPatch.apply") <
            driver.index("hasReviewedResolveHizCapacity") <
            driver.index("resolveHizCapacityPatch.apply"))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: vf_command_pool_growth_source_contract_test.py kern_gen11.cpp")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    assert accepts(source), "missing or reordered VF command-pool contract"
    for fragment in REQUIRED:
        assert source.count(fragment) == 1, f"ambiguous source contract: {fragment}"
        assert not accepts(source.replace(fragment, "MUTATED", 1)), \
            f"mutation escaped source contract: {fragment}"
    for fragment in OWNER:
        assert source.count(fragment) == 2, f"changed dual owner contract: {fragment}"
        first = source.find(fragment)
        second = source.find(fragment, first + len(fragment))
        for offset in (first, second):
            mutated = source[:offset] + "MUTATED" + source[offset + len(fragment):]
            assert not accepts(mutated), f"owner mutation escaped source contract: {fragment}"
    mutations = len(REQUIRED) + 2 * len(OWNER)
    print(f"PASS: owner-scoped VF command-pool routes and {mutations} rejected mutations")


if __name__ == "__main__":
    main()
