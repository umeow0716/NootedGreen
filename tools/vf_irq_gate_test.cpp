#include "../NootedGreen/kern_vf_irq_gate.hpp"
#include "../NootedGreen/kern_gpu_capabilities.hpp"

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

    constexpr NGVfIrqGate::RegisterWrite expectedReset[] = {
        {0x190010U, 0U},
        {0x190030U, 0U}, {0x190034U, 0U}, {0x190038U, 0U},
        {0x19003CU, 0U}, {0x190040U, 0U}, {0x190048U, 0U},
        {0x190090U, 0xFFFFFFFFU}, {0x1900A0U, 0xFFFFFFFFU},
        {0x1900A8U, 0xFFFFFFFFU}, {0x1900ACU, 0xFFFFFFFFU},
        {0x1900D0U, 0xFFFFFFFFU}, {0x1900E8U, 0xFFFFFFFFU},
        {0x1900ECU, 0xFFFFFFFFU}, {0x1900F0U, 0xFFFFFFFFU},
        {0x190100U, 0xFFFFFFFFU},
    };
    constexpr uint32_t expectedResetCount =
        sizeof(expectedReset) / sizeof(expectedReset[0]);
    assert(NGVfIrqGate::preMsiQuiesceCount == expectedResetCount);
    assert(NGVfIrqGate::preMsiQuiescePlan[0].offset ==
           NGVfIrqGate::masterRegister);
    assert(NGVfIrqGate::preMsiQuiescePlan[0].value == 0U);
    for (uint32_t index = 0;
         index < NGVfIrqGate::preMsiQuiesceCount; ++index) {
        const auto &write = NGVfIrqGate::preMsiQuiescePlan[index];
        assert(write.offset == expectedReset[index].offset);
        assert(write.value == expectedReset[index].value);
        assert(NGGpuCapabilities::isVfMmioRegister(write.offset));
        if (index >= 1U && index <= 6U)
            assert(write.value == 0U);
        if (index >= 7U)
            assert(write.value == 0xFFFFFFFFU);
        for (uint32_t prior = 0; prior < index; ++prior)
            assert(NGVfIrqGate::preMsiQuiescePlan[prior].offset !=
                   write.offset);
    }
    assert(NGVfIrqGate::masterDisabled(0U));
    assert(NGVfIrqGate::masterDisabled(0x7FFFFFFFU));
    assert(!NGVfIrqGate::masterDisabled(0x80000000U));
    assert(!NGVfIrqGate::masterDisabled(0xFFFFFFFFU));

    std::printf("PASS: %llu VF IRQ gate states and %u pre-MSI reset writes\n",
                static_cast<unsigned long long>(cases),
                NGVfIrqGate::preMsiQuiesceCount);
    return 0;
}
