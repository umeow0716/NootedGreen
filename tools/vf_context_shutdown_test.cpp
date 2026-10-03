#include "../NootedGreen/kern_vf_context_shutdown.hpp"

#include <array>
#include <cassert>

using NGVfContextShutdown::Action;

int main() {
    // Metadata coverage is not GPU completion. Enumerate accepted/rejected
    // transactions and interfering unknown writers against a simple oracle.
    for (unsigned marker = 0; marker < 2; ++marker)
        for (unsigned accept = 0; accept < 2; ++accept)
            for (unsigned interfere = 0; interfere < 2; ++interfere)
                for (uint32_t stamp : {0U, 1U, 0x7fffffffU, 0x80000000U, UINT32_MAX}) {
                    NGVfSubmissionCoverage::Tracker tracker;
                    tracker.covered = true; // prior submitted work had a marker
                    const uint64_t token = tracker.begin(7, stamp, 64, marker);
                    assert(token && !tracker.hasMarkerCoverage());
                    assert(!tracker.begin(8, stamp, 64, true));
                    assert(!tracker.claim(token, 8, stamp, 64));
                    assert(!tracker.claim(token, 7, stamp ^ 1, 64));
                    assert(!tracker.claim(token, 7, stamp, 72));
                    assert(!tracker.publish(token, 7, stamp, 64));
                    assert(tracker.claim(token, 7, stamp, 64));
                    assert(!tracker.claim(token, 7, stamp, 64));
                    if (interfere)
                        tracker.invalidate();
                    if (accept) {
                        assert(tracker.publish(token, 7, stamp, 64));
                        assert(!tracker.publish(token, 7, stamp, 64));
                    }
                    assert(!tracker.finish(token, 8, accept));
                    assert(tracker.finish(token, 7, accept));
                    const bool oracle = !interfere && (!accept || marker);
                    assert(tracker.hasMarkerCoverage() == oracle);
                    if (accept)
                        assert(tracker.coveredStamp == stamp && tracker.coveredTail == 64);
                    assert(!tracker.finish(token, 7, accept));
                    const uint64_t next = tracker.begin(7, stamp, 72, true);
                    assert(next != token && !tracker.claim(token, 7, stamp, 72));
                    assert(tracker.finish(next, 7, false));
                }
    for (unsigned published = 0; published < 2; ++published) {
        NGVfSubmissionCoverage::Tracker tracker;
        const auto token = tracker.begin(1, 2, 8, true);
        assert(tracker.claim(token, 1, 2, 8));
        if (published)
            assert(tracker.publish(token, 1, 2, 8));
        assert(!tracker.finish(token, 1, !published));
        assert(!tracker.hasMarkerCoverage());
    }
    NGVfSubmissionCoverage::Tracker exhausted;
    exhausted.serial = UINT64_MAX;
    assert(!exhausted.begin(1, 0, 8, true));
    assert(!exhausted.begin(0, 0, 8, true));
    NGVfSubmissionCoverage::Tracker lateWriter;
    const auto lateToken = lateWriter.begin(1, 2, 8, true);
    assert(lateWriter.claim(lateToken, 1, 2, 8));
    assert(lateWriter.publish(lateToken, 1, 2, 8));
    lateWriter.invalidate();
    assert(lateWriter.finish(lateToken, 1, true));
    assert(!lateWriter.hasMarkerCoverage());
    NGVfSubmissionCoverage::Tracker reused;
    for (uint64_t generation = 1; generation <= 1024; ++generation) {
        const auto token = reused.begin(7, 5, 8, true);
        assert(token == generation);
        // An active slot cannot be reset; taint survives until its finish.
        assert(!reused.resetForReuse());
        assert(reused.claim(token, 7, 5, 8));
        assert(reused.publish(token, 7, 5, 8));
        assert(reused.finish(token, 7, true));
        assert(!reused.hasMarkerCoverage());
        assert(reused.resetForReuse());
        assert(reused.serial == generation);
        assert(!reused.claim(token, 7, 5, 8));
    }
    const auto newest = reused.begin(7, 5, 8, true);
    for (uint64_t stale = 1; stale < newest; ++stale) {
        assert(!reused.claim(stale, 7, 5, 8));
        assert(!reused.publish(stale, 7, 5, 8));
        assert(!reused.finish(stale, 7, true));
    }
    assert(reused.claim(newest, 7, 5, 8));
    assert(reused.publish(newest, 7, 5, 8));
    assert(reused.finish(newest, 7, true));
    assert(reused.hasMarkerCoverage());
    assert(reused.resetForReuse() && !reused.hasMarkerCoverage());
    exhausted.covered = true;
    assert(exhausted.resetForReuse());
    assert(exhausted.serial == UINT64_MAX && !exhausted.hasMarkerCoverage());
    assert(!exhausted.begin(1, 5, 8, true));

    // Independent contexts may reuse the same numeric token. The production
    // bridge must first select/pin the exact context; a token is not a global
    // identity. Within that context, wrong owners and stale invocations must
    // not consume the live claim or finish, even after CTB publication.
    for (unsigned marker = 0; marker < 2; ++marker)
        for (unsigned prior = 0; prior < 2; ++prior)
            for (unsigned invalidationPhase = 0; invalidationPhase < 4; ++invalidationPhase) {
                NGVfSubmissionCoverage::Tracker tracker;
                tracker.covered = prior;
                const auto token = tracker.begin(7, 5, 8, marker);
                const auto rejectForeign = [&]() {
                    const auto before = tracker;
                    for (uint64_t wrongOwner : {0ULL, 8ULL}) {
                        assert(!tracker.claim(token, wrongOwner, 5, 8));
                        assert(!tracker.publish(token, wrongOwner, 5, 8));
                        assert(!tracker.finish(token, wrongOwner, true));
                        assert(!tracker.finish(token, wrongOwner, false));
                    }
                    for (uint64_t stale : std::array<uint64_t, 3>{{0, 2, UINT64_MAX}}) {
                        assert(!tracker.claim(stale, 7, 5, 8));
                        assert(!tracker.publish(stale, 7, 5, 8));
                        assert(!tracker.finish(stale, 7, true));
                    }
                    assert(tracker.serial == before.serial && tracker.owner == before.owner);
                    assert(tracker.stamp == before.stamp && tracker.tail == before.tail);
                    assert(tracker.claimed == before.claimed && tracker.published == before.published);
                    assert(tracker.carriesStamp == before.carriesStamp && tracker.tainted == before.tainted);
                    assert(tracker.covered == before.covered);
                    assert(tracker.coveredStamp == before.coveredStamp && tracker.coveredTail == before.coveredTail);
                };
                rejectForeign();
                if (invalidationPhase == 1)
                    tracker.invalidate();
                assert(tracker.claim(token, 7, 5, 8));
                rejectForeign();
                if (invalidationPhase == 2)
                    tracker.invalidate();
                assert(tracker.publish(token, 7, 5, 8));
                rejectForeign();
                if (invalidationPhase == 3)
                    tracker.invalidate();
                assert(!tracker.hasMarkerCoverage());
                assert(tracker.finish(token, 7, true));
                assert(tracker.hasMarkerCoverage() == (marker && invalidationPhase == 0));
            }

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
