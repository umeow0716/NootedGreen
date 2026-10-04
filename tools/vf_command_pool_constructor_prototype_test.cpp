// Offline design prototype ONLY: not linked into the kext or a VM.
// Native UUID/layout admission and complete callback ABI remain prerequisites.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

struct PoolImage { alignas(8) unsigned char bytes[0x18b8]; };
struct Trace {
    unsigned initCalls {}, backingCalls {}, releases {};
    bool nativeResult {}, sawZeroRecord {};
};

static bool nativeInit(PoolImage &pool, Trace &trace) {
    ++trace.initCalls;
    uint64_t record;
    std::memcpy(&record, pool.bytes + 0x1860, sizeof(record));
    trace.sawZeroRecord = record == 0;
    return trace.nativeResult;
}

// Do not release here: selected outer context factories release on false.
// Keep success's native Boolean, and stop before optional backing on failure.
static bool constructorPrototype(bool admittedVf, PoolImage &pool, Trace &trace) {
    if (admittedVf) {
        const uint64_t zero = 0;
        std::memcpy(pool.bytes + 0x1860, &zero, sizeof(zero));
    }
    const bool initialized = nativeInit(pool, trace);
    if (admittedVf && !initialized)
        return false;
    ++trace.backingCalls;
    return true; // Models selected native continuation, not backing failure.
}

int main() {
    for (bool vf : {false, true}) {
        for (bool result : {false, true}) {
            PoolImage pool;
            std::memset(pool.bytes, 0xa5, sizeof(pool.bytes));
            const PoolImage before = pool;
            Trace trace;
            trace.nativeResult = result;
            const bool ok = constructorPrototype(vf, pool, trace);
            if (!ok) ++trace.releases; // Selected outer factory only.
            assert(trace.initCalls == 1);
            assert(trace.backingCalls == unsigned(!vf || result));
            assert(trace.releases == unsigned(vf && !result));
            assert(trace.sawZeroRecord == vf);
            assert(ok == (!vf || result));
            for (unsigned i = 0; i < sizeof(pool.bytes); ++i)
                assert(pool.bytes[i] == ((vf && i >= 0x1860 && i < 0x1868)
                    ? 0 : before.bytes[i]));
        }
    }
}
