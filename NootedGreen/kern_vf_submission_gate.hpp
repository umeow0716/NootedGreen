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

} // namespace NGVfSubmission

#endif /* kern_vf_submission_gate_hpp */
