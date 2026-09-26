#include "../NootedGreen/kern_vf_guc_event.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

using namespace NGVfGuCEvent;

static Kind expected(uint32_t action, uint32_t length)
{
	switch (action) {
		case scheduleContextModeDone:
			return length == 3 ? Kind::ScheduleContextModeDone : Kind::Invalid;
		case deregisterContextDone:
			return length == 2 ? Kind::DeregisterContextDone : Kind::Invalid;
		case tlbInvalidationDone:
			return length == 2 ? Kind::TlbInvalidationDone : Kind::Invalid;
		case contextResetNotification:
			return length == 2 ? Kind::ContextReset : Kind::Invalid;
		case engineFailureNotification:
			return length == 4 ? Kind::EngineFailure : Kind::Invalid;
		default:
			return Kind::Invalid;
	}
}

int main()
{
	uint64_t cases = 0;
	for (uint32_t action = 0; action <= 0xFFFFU; ++action) {
		for (uint32_t length = 0; length <= 32U; ++length) {
			for (uint32_t data0 : {0U, 0x05550000U, 0x0FFF0000U}) {
				const auto result = inspect(originGuc | typeEvent | data0 | action,
				                            length);
				const Kind wanted = expected(action, length);
				assert(result.kind == wanted);
				assert(result.valid() == (wanted != Kind::Invalid));
				const bool fatalWanted = wanted == Kind::ContextReset ||
				                         wanted == Kind::EngineFailure;
				assert(result.fatal() == fatalWanted);
				uint32_t credits = 0;
				if (wanted == Kind::ScheduleContextModeDone)
					credits = 4;
				else if (wanted == Kind::DeregisterContextDone ||
				         wanted == Kind::TlbInvalidationDone)
					credits = 3;
				assert(result.responseCredits == credits);
				++cases;
			}
		}
	}

	for (uint32_t origin : {0U, originGuc}) {
		for (uint32_t type = 0; type <= typeMask; type += 0x10000000U) {
			const auto result = inspect(origin | type | scheduleContextModeDone, 3);
			assert(result.valid() == (origin == originGuc && type == typeEvent));
		}
	}
	assert(!inspect(originGuc | typeEvent | 0x1234U, 2).valid());

	std::printf("PASS: %llu GuC G2H action/length/header classifications\n",
	            static_cast<unsigned long long>(cases));
}
