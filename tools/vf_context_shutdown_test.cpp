#include "../NootedGreen/kern_vf_context_shutdown.hpp"

#include <array>
#include <cassert>

using NGVfContextShutdown::Action;

int main() {
    const std::array<Action, 9> expected {{
        Action::Complete,   // Empty
        Action::Complete,   // Tombstone
        Action::Wait,       // Registering
        Action::Deregister, // Registered
        Action::Wait,       // PendingEnable
        Action::Disable,    // Enabled
        Action::Wait,       // PendingDisable
        Action::Deregister, // Disabled
        Action::Wait,       // PendingDeregister
    }};

    for (unsigned state = 0; state < expected.size(); ++state) {
        assert(NGVfContextShutdown::action(
                   static_cast<VfGucContextState>(state)) == expected[state]);
    }

    // Exhaust every lifecycle state, pending-token combination, and the two
    // defined runnable payloads plus an invalid value.  Enable completions have
    // ordering priority when both tokens exist; a disable cannot overtake one.
    for (unsigned rawState = 0; rawState < expected.size(); ++rawState) {
        for (unsigned enable = 0; enable <= 1; ++enable) {
            for (unsigned disable = 0; disable <= 1; ++disable) {
                for (uint32_t runnable : {0U, 1U, 2U}) {
                    const auto state =
                        static_cast<VfGucContextState>(rawState);
                    const auto result = NGVfContextEvent::scheduleDone(
                        state, enable != 0, disable != 0, runnable);
                    const bool enableState =
                        state == kVfGucContextPendingEnable ||
                        state == kVfGucContextEnabled ||
                        state == kVfGucContextPendingDisable;
                    const bool enableDone = enable && runnable == 1U &&
                                            enableState;
                    const bool disableDone = !enable && disable &&
                                             runnable == 0U &&
                                             state == kVfGucContextPendingDisable;
                    assert(result.handled == (enableDone || disableDone));
                    assert(result.enablePending ==
                           ((enable != 0) && !enableDone));
                    assert(result.disablePending ==
                           ((disable != 0) && !disableDone));

                    auto wantedState = state;
                    if (enableDone && state == kVfGucContextPendingEnable)
                        wantedState = kVfGucContextEnabled;
                    else if (disableDone)
                        wantedState = kVfGucContextDisabled;
                    assert(result.state == wantedState);
                }
            }
        }
    }

    // Model the only active shutdown chain: Enabled -> PendingDisable ->
    // Disabled -> PendingDeregister -> Tombstone. Pending states are completed
    // only by their matching GuC event and are never skipped by the sweeper.
    VfGucContextState state = kVfGucContextEnabled;
    assert(NGVfContextShutdown::action(state) == Action::Disable);
    state = kVfGucContextPendingDisable;
    assert(NGVfContextShutdown::action(state) == Action::Wait);
    state = kVfGucContextDisabled;
    assert(NGVfContextShutdown::action(state) == Action::Deregister);
    state = kVfGucContextPendingDeregister;
    assert(NGVfContextShutdown::action(state) == Action::Wait);
    state = kVfGucContextTombstone;
    assert(NGVfContextShutdown::action(state) == Action::Complete);
}
