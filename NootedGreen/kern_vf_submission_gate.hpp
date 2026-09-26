#ifndef kern_vf_submission_gate_hpp
#define kern_vf_submission_gate_hpp

namespace NGVfSubmission {

struct State {
	bool virtualDevice;
	bool ggttReady;
	bool memoryIrqReady;
	bool ctbCpuMapped;
	bool ctbGpuMapped;
	bool ctbEnabled;
	bool ctbStopped;
	bool submissionStopped;
	bool protocolFault;
};

constexpr bool ready(const State &state)
{
	return state.virtualDevice && state.ggttReady && state.memoryIrqReady &&
		state.ctbCpuMapped && state.ctbGpuMapped && state.ctbEnabled &&
		!state.ctbStopped && !state.submissionStopped && !state.protocolFault;
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

} // namespace NGVfSubmission

#endif /* kern_vf_submission_gate_hpp */
