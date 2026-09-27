// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.

#include "kern_telemetry.hpp"

#include <Headers/kern_util.hpp>
#include <kern/energy_perf.h>

namespace {
using GpuAccumulateTime = uint64_t (*)(uint32_t, uint32_t, uint32_t,
                                       uint64_t, uint64_t);
using GpuDescribe = void (*)(gpu_descriptor_t);

GpuAccumulateTime kernelGpuAccumulateTime {nullptr};
GpuDescribe kernelGpuDescribe {nullptr};
}

bool ngResolveKernelTelemetry(KernelPatcher &patcher)
{
	// KernelID restricts lookup to the running XNU image, so these cannot
	// resolve back to NootedGreen's own exported forwarding symbols.
	kernelGpuAccumulateTime = reinterpret_cast<GpuAccumulateTime>(
		patcher.solveSymbol(KernelPatcher::KernelID, "_gpu_accumulate_time"));
	kernelGpuDescribe = reinterpret_cast<GpuDescribe>(
		patcher.solveSymbol(KernelPatcher::KernelID, "_gpu_describe"));
	return kernelGpuAccumulateTime && kernelGpuDescribe;
}

extern "C" EXPORT uint64_t gpu_accumulate_time(
	uint32_t scope, uint32_t gpuId, uint32_t gpuDomain,
	uint64_t accumulatedNs, uint64_t timestampNs)
{
	// processPatcher resolves both functions before any dependent accelerator
	// can load. Keep a deterministic fallback for defensive early invocation.
	return kernelGpuAccumulateTime ?
		kernelGpuAccumulateTime(scope, gpuId, gpuDomain, accumulatedNs, timestampNs) : 0;
}

extern "C" EXPORT void gpu_describe(gpu_descriptor_t descriptor)
{
	if (kernelGpuDescribe)
		kernelGpuDescribe(descriptor);
}
