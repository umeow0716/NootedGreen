#include "../NootedGreen/kern_vf_irq_gate.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    uint64_t cases = 0;

    for (uint32_t count = 0; count <= 4096; ++count) {
        for (uint32_t isClosed = 0; isClosed < 2; ++isClosed) {
            const uint32_t state = count |
                (isClosed ? NGVfIrqGate::closedBit : 0U);

            assert(NGVfIrqGate::closed(state) == (isClosed != 0));
            assert(NGVfIrqGate::active(state) == count);
            assert(NGVfIrqGate::drained(state) ==
                   (isClosed != 0 && count == 0));

            uint32_t next = 0xDEADBEEFU;
            const bool entered = NGVfIrqGate::enter(state, next);
            assert(entered == (isClosed == 0));
            if (entered) {
                assert(NGVfIrqGate::active(next) == count + 1U);
                assert(!NGVfIrqGate::closed(next));
            }

            next = 0xDEADBEEFU;
            const bool left = NGVfIrqGate::leave(state, next);
            assert(left == (count != 0));
            if (left) {
                assert(NGVfIrqGate::active(next) == count - 1U);
                assert(NGVfIrqGate::closed(next) == (isClosed != 0));
            }

            const uint32_t shut = NGVfIrqGate::close(state);
            assert(NGVfIrqGate::closed(shut));
            assert(NGVfIrqGate::active(shut) == count);
            ++cases;
        }
    }

    uint32_t next = 0;
    assert(!NGVfIrqGate::enter(NGVfIrqGate::activeMask, next));
    assert(!NGVfIrqGate::leave(0, next));
    assert(!NGVfIrqGate::leave(NGVfIrqGate::closedBit, next));

    uint32_t state = 0;
    assert(NGVfIrqGate::enter(state, state));
    assert(NGVfIrqGate::enter(state, state));
    state = NGVfIrqGate::close(state);
    assert(!NGVfIrqGate::drained(state));
    assert(!NGVfIrqGate::enter(state, next));
    assert(NGVfIrqGate::leave(state, state));
    assert(NGVfIrqGate::leave(state, state));
    assert(NGVfIrqGate::drained(state));

    std::printf("PASS: %llu VF IRQ gate state cases\n",
                static_cast<unsigned long long>(cases));
    return 0;
}
