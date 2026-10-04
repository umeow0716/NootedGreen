#!/usr/bin/env python3
"""Source contract for the owner-scoped System-KC pool-growth wrapper."""
import sys
from pathlib import Path


REQUIRED = (
    "NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid",
    '"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv", growthStart',
    "NGIOAccelCommandPool::hasReviewedGrowthContract(",
    '"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv",\n\t\t\t vfAllocMoreCommandBuffers, this->oIOAccelAllocMoreCommandBuffers',
    "pool && gVfIdentity == VfIdentity::Virtual &&",
    "getMember<void *>(pool, NGIOAccelCommandPool::acceleratorOffset) ==\n\t\t\tgVfAccelerator",
    "callback->oIOAccelAllocMoreCommandBuffers)(pool)",
    "NGIOAccelCommandPool::completedGrowth(",
    "gVfAccelerator = that;",
)


def accepts(source: str) -> bool:
    if any(fragment not in source for fragment in REQUIRED):
        return False
    io_start = source.index("if (index == kextIOAcceleratorFamily2.loadIndex)")
    io_end = source.index("const bool physicalFramebuffer", io_start)
    io = source[io_start:io_end]
    if not (io.index("ioAcceleratorTahoe25G229Uuid") <
            io.index("hasReviewedGrowthContract") <
            io.index("commandPoolRoutes") < io.index("routeMultiple")):
        return False
    wrapper_start = source.index("bool Gen11::vfAllocMoreCommandBuffers(void *pool)")
    wrapper_end = source.index("bool Gen11::IGMemoryManagerInitSegments", wrapper_start)
    wrapper = source[wrapper_start:wrapper_end]
    if not (wrapper.index("previousCount") < wrapper.index("FunctionCast(") <
            wrapper.index("publishedCount") < wrapper.index("completedGrowth(")):
        return False
    start_start = source.index("bool Gen11::start(void *that, void *provider)")
    start_end = source.index("void Gen11::acceleratorStop", start_start)
    start = source[start_start:start_end]
    return start.index("gVfAccelerator = that;") < start.index("FunctionCast(start")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: vf_command_pool_growth_source_contract_test.py kern_gen11.cpp")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    assert accepts(source), "missing or reordered VF pool-growth contract"
    for fragment in REQUIRED:
        assert source.count(fragment) == 1, f"ambiguous source contract: {fragment}"
        assert not accepts(source.replace(fragment, "MUTATED", 1)), \
            f"mutation escaped source contract: {fragment}"
    print(f"PASS: owner-scoped VF pool-growth route and {len(REQUIRED)} rejected mutations")


if __name__ == "__main__":
    main()
