# Tahoe SR-IOV protocol audit — in progress

Updated: 2026-09-26. Source baseline: `1dd2f4b` on
`codex/tahoe-sriov-vf`. This safety-audit checkpoint is untested on hardware
and is NOT a boot-test candidate or successful driver baseline.
The reported whole-host freeze has NOT been assigned a proven cause.
Keep `macos-tahoe-sriov` shut off during this review.

## Evidence inspected

- Local i915 source: `/usr/src/i915-sriov-dkms-2026.03.05.7`.
- Apple binary: `build/test-kexts/v213/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics`
  under the parent workspace.
- `IGHardwareGuCCTBuffer::initWithAccelerator`, address `0x1f386`:
  stores `IGSharedMappedBuffer::withOptions` result at object offset `0x40`.
- `IGHardwareGuCCTBuffer::free`, address `0x1f564`:
  releases that object at `0x1f5a3`; it also accesses physical `0xcee8`.
  Offsets `0x18` and `0x20` are queue locks, NOT backing objects.
- `gt/iov/intel_iov_memirq.c` allocates and pins a separate memory-IRQ
  object, programs its addresses separately, and releases it in IOV teardown.
  Our bridge instead shares its allocation with CTB. CTB disable alone
  therefore does not establish a safe lifetime boundary for that allocation.

## Working changes

- Replace the physical interrupt filter on VF and consume memory IRQs;
  bound G2H draining and stop submissions on detected protocol faults.
- Track asynchronous context enable/disable/deregister events and retain
  context backing while firmware ownership is uncertain.
- Replace physical GuC TLB invalidation with modern GuC actions and remove
  legacy binary `0xcee8` accesses only on an identified VF.
- Exclude VF from physical engine setup, reset-list population, watchdog
  register reads and Blit3D tier-1/error-register diagnostics.
- Reject scheduler preferences other than reference GuC scheduler 4 on an
  already initialized VF.
- Retain CTB backing during channel layout, before publishing it. Reject subsequent
  initialization while it remains retained. Failed transport disable must
  propagate failure on the second legacy channel teardown as well.
- Keep the shared CTB/memory-IRQ backing retained even after successful CTB
  disable. This is an intentional temporary leak, not a completed teardown
  implementation. Device restart/reinitialization is currently unsupported.
- Mark undersized known GuC lifecycle completions as protocol faults.

## Required follow-up before any boot test

1. Complete the device-identity audit. VF_CAP identification is now independent
   of bootstrap; start, MMIO, interrupt and TLB routes fail closed rather than
   treating failed VF initialization as a PF. Remaining readiness/CPU-based
   branches and platform capability gating still require review.
2. Prove complete teardown ordering: prevent new senders, synchronize running
   interrupt handlers, stop engine and GuC memory-IRQ writes, then release
   mappings and backing. Retaining an OSObject alone is not proof that explicit
   unmap/ownership-transfer paths cannot invalidate its GPU mapping.
3. Audit CTB allocation failure and partial KLV registration, including failures
   before the new retain point. Audit shared state synchronization throughout.
4. Review all routed Apple callees, raw BAR access, force-wake variants,
   framebuffer/display paths, wait contexts and GuC parser length/type handling.
   A textual MMIO inventory is not a completed source or reachability review.
5. Verify GGTT mapping invalidation and LRCA lifetime through every error path.
6. Finish the full repository source/build review requested by the user.
   This document covers only the current protocol review, not all files.

## Validation limits

`git diff --check` passed during this batch. The initial syntax command lacked
kernel preprocessor definitions and incorrectly selected DriverKit declarations.
Adding `-DKERNEL=1 -DKERNEL_PRIVATE=1` makes Linux Clang syntax checking of
`kern_gen11.cpp` pass (23 warnings). This is not a full build; native macOS
build/link validation remains required. No VM boot, driver load, GPU submission
or Metal test was performed for this batch. The checkpoint is being submitted
to native macOS CI; build success must not be interpreted as runtime validation.

## Additional mailbox review

Compared `vfGucSendMMIO` with `gt/uc/intel_guc.c:intel_guc_send_mmio`.
The bridge previously retried after an ownership timeout or lost ownership
following BUSY. Working changes now replay only on explicit RETRY, validate
request origin/type, check the complete scratch mapping, initialize response
storage, and poison the mailbox after ambiguous ownership/BUSY timeouts.
Subsequent callers must not overwrite an outstanding request, including teardown.
The short existing BUSY deadline and interrupt-context callability still require
review. Lock publication now uses pointer compare-and-swap so concurrent first
callers cannot install distinct locks. A timeout poisons transport; it is not proof
that firmware stopped DMA.

VF identity evidence: i915 `gen12_pci_capability_is_vf()` reads `GEN12_VF_CAP_REG`
at `0x1901f8`, accepts only bit 0 and rejects other set bits. Identity must be
established independently of firmware handshake, with invalid MMIO distinguished
from a valid PF result. Implemented for the current transport; CPU-based
`isRealTGL` branches elsewhere are not a sufficient PF/VF discriminator.

## Software interrupt and bounded reader review

The Apple hardware callback at `0x2257e` only signals IOInterruptEventSource;
it does not advance the G2H head synchronously. Draining from the hardware
filter and checking head progress there was therefore incorrect. The VF filter
now dispatches once. A routed software callback drains up to 256 messages and
reschedules its event source when more remain. It preserves the CTBuffer
consumer's fence matching but ignores its legacy log-flush return bits, which
would otherwise misinterpret modern HXG actions (e.g. `0x1008`).

The VF reader validates fixed allocation bounds, descriptor status/indexes,
frame format and complete payload availability before copying into Apple's
32-dword stack buffer. Pending checks also validate the full descriptor instead
of treating corruption as an empty queue. The pure validation helpers passed
9,621,504 boundary cases plus malformed descriptors under ASan/UBSan. All 11
built C++ translation units passed syntax checking in `/tmp/ngreen-static.ZFW4Ro`.
These tests do not exercise DMA ordering, the actual copy, IRQ races or firmware.

Teardown remains a boot blocker: `IGHardwareGuC::free` (`0x206b8`) deregisters
CTB before unregistering hardware callbacks and removing its software event
source. The working shutdown flag now closes admission before disabling CTB,
waits out a current H2G writer under its queue lock, and leaves quarantined
addresses immutable. This fixes pointer-clearing races, not complete teardown.
`IGSharedMappedBuffer::free` (`0x10ae0`) and `unlockForCPUAccess` (`0x10b26`)
both clear the CPU mapping; object retention alone is not proof of mapping
lifetime. Caller reachability and synchronization are still under review.

## IRQ lifecycle audit

`IGInterruptBridge::enable` (`0x4a332`) and `disable` (`0x4a69a`) each
directly mask/unmask `GFX_MSTR_IRQ` (`0x190010`), outside the filter. Working
VF-only, symbol-bounded patches remove these four instructions while preserving
event-source operations and callback registration. Offline checks found exactly
one mask and one unmask instruction in each function. The adjacent
`enableInterrupts` (`0x4a42e`) and `disableInterrupts` (`0x4a778`) also write
physical GT enable/mask/selector/identity registers; routed VF replacements now
use the memory-IRQ enable vector, matching `intel_iov_memirq_postinstall/reset`.
PF routes retain the originals. Both non-multithreaded force-wake entry points
are routed to the VF no-op only for an identified VF.

Enable intent is recorded if requested before memory-IRQ allocation. This mask
does not stop GuC DMA or synchronize callbacks. Enable/configure/teardown races
and sleep/wake behavior remain to be completed. Latest offline syntax and ring
checks passed in `/tmp/ngreen-static.ETyDns`; these patches have not been loaded.

The relocation at `0x1fe47` resolves to `IOWorkLoop::workLoop`, confirming GuC
creates a separate workloop at object offset `0xa00`. This alone does not prove
that every synchronous lifecycle wait runs outside that workloop's gate.
State waits and TLB invalidation now explicitly reject a wait on that workloop
or while its gate is held. Other call-context and lock-order checks remain.

CT response dispatch now excludes BUSY, following `intel_guc_ct.c:ct_handle_hxg`;
BUSY is a mailbox transition, not a terminal CT fence completion. Retry remains
a terminal request outcome and must not be silently translated into success.

## Legacy sender containment and publication

Direct calls to `IGHardwareGuC::hostToGuCAction` (`0x21810`) issue `0x10`,
`0x20`, `0x30` and `0x3005`. They belong to the old doorbell/log/sampling path,
not the modern direct-LRCA submission implementation. The VF route now rejects
these untranslated requests rather than falling back to the old CT sender or
MMIO. The MMIO wrapper only translates CTB registration/deregistration; native
VF bootstrap/configuration calls use the modern helper directly. This is a
fail-closed boundary, not an implementation of those legacy features. The
upstream callers still need audit for physical accesses before issuing requests.

H2G pointer validation now checks the exact published descriptor/buffer layout
before dereferencing. Context table initialization is serialized by the shared
GuC mutex instead of publishing competing allocations. The queue rechecks
shutdown/fault admission under its own lock. All 11 syntax checks and ring tests
passed in `/tmp/ngreen-static.m8aWzd` after these changes.

`IGSharedMappedBuffer::unlockForCPUAccess` has no direct call sites in this
binary, but virtual/external reachability has not been ruled out.
`transferOwnership` (`0x2a318`) issues extended PCI configuration accesses at
`0xf8/0xfc` for each backing page when accelerator flag `0x20` is set; its VF
reachability and semantics need checking before retention can be called safe.
