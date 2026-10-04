#!/usr/bin/env python3
"""Abstract teardown counterexamples, NOT an implementation/KC verifier.

Events are atomic at the model boundary. Pinned KC/source contracts verify the
native locks, generation checks and callback ownership used by this model;
this file remains an interleaving model, not runtime execution evidence.
"""
from itertools import permutations


def run(order, *, close_admission, drain):
    admitting = True
    pending = True
    active = False
    alive = True
    unsafe = False
    released = False
    for event in order:
        if event == "begin" and pending:
            pending = False
            active = True
            unsafe |= not alive
        elif event == "cancel":
            pending = False
            if close_admission:
                admitting = False
        elif event == "rearm" and admitting:
            pending = True
            unsafe |= not alive
        elif event == "finish" and active:
            unsafe |= not alive
            active = False
        elif event == "release":
            if drain and (admitting or pending or active):
                # Deferred release is required, not fabricated completion.
                continue
            alive = False
            released = True
            unsafe |= pending or active
    return unsafe, released


def has_wait_cycle(owners, waiting):
    edges = {actor: owners[lock] for actor, lock in waiting.items()
             if lock in owners and owners[lock] != actor}
    for start in edges:
        seen = set()
        actor = start
        while actor in edges:
            if actor in seen:
                return True
            seen.add(actor)
            actor = edges[actor]
    return False


def detached_passive_timer(*, active, queued):
    """Model synchronous gate removal plus IOTimerEventSource generation skip."""
    generation = 7
    scheduled_generation = generation
    owner_alive = True
    action_after_remove = False

    # An already-entered callback owns the workloop gate. removeEventSource
    # cannot enter its maintenance action until that callback returns.
    if active:
        active = False

    # setWorkLoop(nullptr) invokes disable before clearing the binding. A
    # queued passive callout retains the source, but its old generation can no
    # longer dispatch the raw owner after the synchronous remove returns.
    generation += 1
    attached = False
    if queued:
        action_after_remove = generation == scheduled_generation
    assert not attached
    owner_alive = False
    return active, action_after_remove, owner_alive


def main():
    # Only enforce each actor's local order; enumerate stop/producer/callback
    # interleavings rather than assuming cancel is last or a callback is idle.
    orders = [order for order in permutations(
        ("begin", "cancel", "rearm", "finish", "release"))
        if order.index("cancel") < order.index("release")
        and order.index("begin") < order.index("finish")]
    witness = ("begin", "cancel", "rearm", "release", "finish")
    assert witness in orders
    assert run(witness, close_admission=False, drain=False)[0]
    # Closing new admission alone does not drain an already-entered callback.
    assert run(witness, close_admission=True, drain=False)[0]
    # Draining alone without closing admission also cannot release safely.
    assert not run(witness, close_admission=False, drain=True)[1]
    successful_releases = 0
    for order in orders:
        unsafe, released = run(order, close_admission=True, drain=True)
        assert not unsafe, order
        successful_releases += released
    assert successful_releases > 0  # Avoid a vacuous never-release policy.
    # A finish waiter balances only its own reference. It cannot certify
    # removal of an earlier event's reference or exclude a new entrant.
    for prior_refs in range(4):
        refs = prior_refs
        refs += 1  # admitted finish waiter
        refs -= 1  # balanced finish waiter exit (including error cleanup)
        assert refs == prior_refs
        assert (refs == 0) == (prior_refs == 0)
        refs += 1  # admission still open: a later waiter can re-enable source
        assert refs > 0
    # Kernel evidence establishes timeout's gate-before-callback-mutex path,
    # and removeEventSource delegates synchronously through a command gate.
    # Retain the rejected mutex-before-remove counterexample.
    owners = {"gate": "callback", "mutex": "cleanup"}
    waiting = {"callback": "mutex", "cleanup": "gate"}
    assert has_wait_cycle(owners, waiting)
    # Production only snapshots the empty collection under its mutex, releases
    # that mutex, and then enters the gate for removal: no simultaneous
    # mutex-before-gate ownership exists.
    owners = {"gate": "callback"}
    waiting = {"cleanup": "gate"}
    assert not has_wait_cycle(owners, waiting)
    # Consistent gate-first acquisition also waits without a circular edge.
    assert not has_wait_cycle({"gate": "callback", "mutex": "callback"},
                              {"cleanup": "gate"})
    # Same-thread recursive gate entry is not this two-thread counterexample;
    # but waiting for one's own callback completion is independently invalid.
    owners["gate"] = "cleanup"
    assert not has_wait_cycle(owners, waiting)
    current_callback = "cleanup"
    draining_actor = "cleanup"
    assert current_callback == draining_actor  # Must defer, not wait on self.
    for active in (False, True):
        for queued in (False, True):
            active_after, action_after, owner_alive = detached_passive_timer(
                active=active, queued=queued)
            assert not active_after and not action_after and not owner_alive
    print(f"PASS: {len(orders)} abstract timer teardown orders and four "
          "gate/generation detach states; cancel/admission-only, reversed-lock "
          "and self-drain counterexamples retained")


if __name__ == "__main__":
    main()
