#ifndef kern_vf_submission_gate_hpp
#define kern_vf_submission_gate_hpp

#include <stdint.h>

namespace NGVfSubmission {

struct State {
	bool virtualDevice;
	bool ggttReady;
	bool interruptReady;
	bool ctbCpuMapped;
	bool ctbGpuMapped;
	bool ctbEnabled;
	bool ctbStopped;
	bool submissionStopped;
	bool protocolFault;
};

constexpr bool ready(const State &state)
{
	return state.virtualDevice && state.ggttReady && state.interruptReady &&
		state.ctbCpuMapped && state.ctbGpuMapped && state.ctbEnabled &&
		!state.ctbStopped && !state.submissionStopped && !state.protocolFault;
}

// G2H consumers remain live after ordinary submission is stopped so teardown
// completions can drain. A synchronous poll may also finish after hardware IRQ
// delivery is disabled; an interrupt callback may not. Neither may enter before
// the complete CTB is published, after it is sealed, or after a protocol fault
// quarantines native CTB locks/backing.
constexpr bool consumerReady(const State &state, bool requireInterrupt = true)
{
	return state.virtualDevice && state.ggttReady &&
		(!requireInterrupt || state.interruptReady) &&
		state.ctbCpuMapped && state.ctbGpuMapped && state.ctbEnabled &&
		!state.ctbStopped && !state.protocolFault;
}

// Apple's task identity is authoritative except for the single VF bootstrap
// interval before IntelAccelerator has published its owned kernel task.
// Physical later-generation GPUs must never inherit this virtualization-only
// repair merely because they are not a native Tiger Lake device.
constexpr bool bootstrapKernelTask(bool nativeClassification,
	                               bool virtualDevice,
	                               bool taskPresent,
	                               bool acceleratorPresent,
	                               bool kernelTaskAssigned)
{
	return nativeClassification ||
		(virtualDevice && taskPresent && acceleratorPresent && !kernelTaskAssigned);
}

struct RingReservation {
	bool valid;
	uint32_t bytes;
};

struct RingCallerRequest {
	bool valid;
	uint32_t dwords;
};

// The UUID-pinned Tahoe caller inventory contains one producer
// (IntelAccelerator::submitSyncEvents) which requests count + 1 dwords but
// emits marker + count + marker before submitToRing adds its final alignment
// dword(s). Preserve the native ABI while adding the single missing payload
// dword at the common VF reservation boundary. This is deliberately VF-only;
// a future payload must pass the complete caller inventory before admission.
constexpr RingCallerRequest guardedRingCallerRequest(uint32_t requestedDwords)
{
	if (requestedDwords == UINT32_MAX)
		return {false, 0};
	return {true, requestedDwords + 1U};
}

// Tahoe reserves the caller payload plus a ring trailer, optional deferred
// TLB/AUX commands and one alignment dword when the current tail is not
// qword-aligned. Use 64-bit arithmetic so malformed requests cannot wrap the
// native 32-bit calculation into an apparently small reservation.
constexpr RingReservation ringReservation(uint32_t requestedDwords,
	                                       uint32_t ringBytes,
	                                       uint32_t cursor,
	                                       bool extendedRenderTrailer,
	                                       bool tlbPending,
	                                       uint32_t flushTlbDwords,
	                                       bool auxPending)
{
	if (ringBytes < 16 || (ringBytes & (ringBytes - 1U)) != 0 ||
	    cursor >= ringBytes || (cursor & 3U) != 0 ||
	    (!tlbPending && flushTlbDwords != 0) ||
	    (tlbPending && (flushTlbDwords == 0 || flushTlbDwords > 16)))
		return {false, 0};

	uint64_t dwords = requestedDwords +
		(extendedRenderTrailer ? 4ULL : 1ULL);
	if (tlbPending)
		dwords += flushTlbDwords;
	if (auxPending)
		dwords += 3;
	if ((cursor & 7U) != 0)
		dwords++;
	dwords = (dwords + 1ULL) & ~1ULL;
	const uint64_t bytes = dwords * sizeof(uint32_t);
	if (bytes > UINT32_MAX || bytes > static_cast<uint64_t>(ringBytes - 8U))
		return {false, 0};
	return {true, static_cast<uint32_t>(bytes)};
}

constexpr bool ringReservationSatisfied(const RingReservation &reservation,
	                                     uint32_t availableBytes,
	                                     uint32_t ringBytes)
{
	return reservation.valid && ringBytes >= 8 &&
		availableBytes <= ringBytes - 8U &&
		availableBytes >= reservation.bytes;
}

} // namespace NGVfSubmission

#endif /* kern_vf_submission_gate_hpp */
