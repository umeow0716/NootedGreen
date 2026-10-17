#ifndef kern_vf_cpu_range_hpp
#define kern_vf_cpu_range_hpp
#include <stdint.h>

namespace NGVfCpuRange {
// Historical native-return receipts, NOT native mapping/lifetime leases.
// No address returned here may authorize a read without an independent,
// current native lifetime scope. Storage synchronization belongs to caller.
struct Receipt {
	uint64_t owner=0, memory=0, epoch=0, serial=0, cpu=0, bytes=0;
};
enum class Result : uint32_t { Missing, Unique, Ambiguous, Invalid };
struct Match { Result result=Result::Missing; uint32_t index=0; uint64_t offset=0; };

inline bool kernelRange(uint64_t base,uint64_t bytes) {
	return bytes && (base>>47)==0x1ffffU && base<=UINT64_MAX-bytes &&
	       ((base+bytes-1U)>>47)==0x1ffffU;
}
inline bool valid(const Receipt &r) {
	return r.owner && r.memory && r.epoch && r.serial &&
	       kernelRange(r.cpu,r.bytes);
}
template <uint32_t N>
inline Match correlate(const Receipt (&receipts)[N],uint64_t owner,uint64_t epoch,
                       uint64_t firstSerial,uint64_t lastSerial,
                       uint64_t cpu,uint64_t bytes) {
	Match out;
	if(!owner || !epoch || !firstSerial || firstSerial>lastSerial || !kernelRange(cpu,bytes)) {
		out.result=Result::Invalid;return out;
	}
	for(uint32_t i=0;i<N;++i) {
		const auto &r=receipts[i];
		if(!valid(r) || r.owner!=owner || r.epoch!=epoch ||
		   r.serial<firstSerial || r.serial>lastSerial || cpu<r.cpu)continue;
		const uint64_t offset=cpu-r.cpu;
		if(offset>r.bytes || bytes>r.bytes-offset)continue;
		// Even duplicate receipts are ambiguous: pointer reuse/ABA cannot
		// be resolved by matching addresses or a still-nonzero lock count.
		if(out.result==Result::Unique) {
			out.result=Result::Ambiguous;out.index=0;out.offset=0;return out;
		}
		out.result=Result::Unique;out.index=i;out.offset=offset;
	}
	return out;
}
}
#endif
