#include "../NootedGreen/kern_vf_context_shutdown.hpp"

#include <array>
#include <cassert>

using NGVfContextShutdown::Action;

int main() {
    // Independent widened endpoint oracle for all nearby slot/buffer edges.
    for (int32_t index = -2; index <= 192; ++index) {
        for (uint64_t bytes = 0; bytes <= 0x3040; ++bytes) {
            for (uint64_t scratch : {0ULL, 7ULL, 8ULL, 4096ULL}) {
                const bool expectedBacking = index >= 0 && scratch >= 8 &&
                    (static_cast<uint64_t>(index) + 1) * 64 <= bytes;
                assert(NGVfContextShutdown::validPacketBacking(index, bytes, scratch) ==
                       expectedBacking);
            }
        }
    }
    assert(!NGVfContextShutdown::validPacketBacking(-1, UINT64_MAX, UINT64_MAX));
    assert(NGVfContextShutdown::validPacketBacking(INT32_MAX, UINT64_MAX, 8));
    const uint64_t lastEndpoint = (static_cast<uint64_t>(INT32_MAX) + 1) * 64;
    assert(!NGVfContextShutdown::validPacketBacking(INT32_MAX, lastEndpoint - 1, 8));
    assert(NGVfContextShutdown::validPacketBacking(INT32_MAX, lastEndpoint, 8));

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

    // Exhaust the failed-attach compensation boundary. Once registration was
    // published, neither a failed send nor an unobserved DEREGISTER_DONE can be
    // reported as a safely retired context.
    for (unsigned registered = 0; registered <= 1; ++registered) {
        for (unsigned sent = 0; sent <= 1; ++sent) {
            for (unsigned tombstone = 0; tombstone <= 1; ++tombstone) {
                const bool complete =
                    NGVfContextShutdown::registrationCleanupComplete(
                        registered != 0, sent != 0, tombstone != 0);
                assert(complete ==
                       (!registered || (sent && tombstone)));
            }
        }
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

    struct Context {
        uint32_t lrcaPage;
        uint32_t descriptorLo;
        uint32_t descriptorHi;
        uint16_t refCount;
        uint8_t engineClass;
        uint8_t engineInstance;
        VfGucContextState state;
        bool enablePending;
        bool disablePending;
        void *contextBacking;
        void *ringBacking;
        void *stampBacking;
        void *scratchBacking;
    };
    int backing = 0;
    int ringBacking = 0;
    int stampBacking = 0;
    int scratchBacking = 0;
    for (const auto initialState : {
             kVfGucContextEmpty, kVfGucContextTombstone,
             kVfGucContextRegistering, kVfGucContextRegistered,
             kVfGucContextPendingEnable, kVfGucContextEnabled,
             kVfGucContextPendingDisable, kVfGucContextDisabled,
             kVfGucContextPendingDeregister}) {
        Context context {0x12345000U, 0x12345309U, 0xA5A20020U, 7,
                         4, 2, initialState, true, true, &backing, &ringBacking,
                         &stampBacking, &scratchBacking};
        const bool handled = NGVfContextEvent::deregisterDone(context);
        assert(handled ==
               (initialState == kVfGucContextPendingDeregister));
        assert(context.state == (handled ? kVfGucContextTombstone :
                                           initialState));
        // Firmware completion must not erase identity needed by late detach.
        assert(context.lrcaPage == 0x12345000U);
        assert(context.descriptorLo == 0x12345309U);
        assert(context.descriptorHi == 0xA5A20020U);
        assert(context.engineClass == 4);
        assert(context.engineInstance == 2);
        assert(context.contextBacking == &backing);
        assert(context.ringBacking == &ringBacking);
        assert(context.stampBacking == &stampBacking);
        assert(context.scratchBacking == &scratchBacking);
        assert(context.refCount == 7);
        assert(context.enablePending == !handled);
        assert(context.disablePending == !handled);
    }

    Context released {0x12345000U, 0x12345309U, 0xA5A20020U, 0,
                      4, 2, kVfGucContextTombstone, false, false, &backing, &ringBacking,
                      &stampBacking, &scratchBacking};
    NGVfContextEvent::clearReleasedIdentity(released);
    assert(released.lrcaPage == 0 && released.descriptorLo == 0 &&
           released.descriptorHi == 0 && released.engineClass == 0 &&
           released.engineInstance == 0 && released.contextBacking == nullptr &&
           released.ringBacking == nullptr && released.stampBacking == nullptr &&
           released.scratchBacking == nullptr);
    assert(released.refCount == 0 &&
           released.state == kVfGucContextTombstone);
}
