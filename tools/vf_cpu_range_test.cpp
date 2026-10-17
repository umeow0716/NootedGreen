#include "../NootedGreen/kern_vf_cpu_range.hpp"
#include <assert.h>
#include <stdio.h>
using namespace NGVfCpuRange;
int main() {
	const uint64_t base=0xffff800000010000ULL;
	Receipt r[4]{};
	r[0]={1,2,3,4,base,65536};
	const auto query=[&](uint64_t p,uint64_t n) {return correlate(r,1,3,4,4,p,n);};
	auto m=query(base+64,128);
	assert(m.result==Result::Unique && m.index==0 && m.offset==64);
	assert(query(base,65536).result==Result::Unique);
	assert(query(base+65535,1).result==Result::Unique);
	assert(query(base+65536,1).result==Result::Missing);
	assert(query(base+65535,2).result==Result::Missing);
	assert(query(base-1,1).result==Result::Missing);
	assert(query(base,0).result==Result::Invalid);
	assert(query(UINT64_MAX-3,8).result==Result::Invalid);
	assert(query(0x10000,4).result==Result::Invalid);
	assert(correlate(r,9,3,4,4,base,4).result==Result::Missing);
	assert(correlate(r,1,9,4,4,base,4).result==Result::Missing);
	assert(correlate(r,1,3,5,6,base,4).result==Result::Missing);
	assert(correlate(r,1,3,1,3,base,4).result==Result::Missing);
	assert(correlate(r,1,3,5,4,base,4).result==Result::Invalid);
	r[1]=r[0];
	assert(query(base,4).result==Result::Ambiguous);
	r[1].cpu=base+32;r[1].bytes=32;
	assert(query(base+32,4).result==Result::Ambiguous);
	assert(query(base+64,4).result==Result::Unique);
	r[0].bytes=UINT64_MAX;
	assert(query(base+64,4).result==Result::Missing);
	puts("PASS CPU receipt containment/owner/epoch/serial/overflow/overlap/ABA refusal; no read authorization");
}
