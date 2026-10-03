#!/usr/bin/env python3
"""Abstract teardown counterexamples, NOT an implementation/KC verifier.

Events are atomic at the model boundary. Native locks, generation checks,
producer admission, callback ownership and DMA must be verified separately.
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
    print(f"PASS: {len(orders)} abstract timer teardown orders; "
          "cancel-only and admission-only counterexamples retained")


if __name__ == "__main__":
    main()
