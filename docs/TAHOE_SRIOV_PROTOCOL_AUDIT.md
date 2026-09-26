# Tahoe SR-IOV protocol audit — in progress

Updated: 2026-09-26. Source baseline: `1dd2f4b` on
`codex/tahoe-sriov-vf`. This safety-audit checkpoint is untested on hardware
and is NOT a boot-test candidate or successful driver baseline.
Checkpoint `4a0b838` passed native macOS build/link and sanitizer CI:
https://github.com/umeow0716/NootedGreen/actions/runs/36215000826 .
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
   branches still require review. PCI capability gating was added below.
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
GT enable/mask/selector/identity registers; routed VF replacements now
use the memory-IRQ enable vector, matching `intel_iov_memirq_postinstall/reset`.
PF routes retain the originals. Both non-multithreaded force-wake entry points
are routed to the VF no-op only for an identified VF.

Correction after inspecting `intel_uncore.c:vf_accessible_regs`: `0x190010`
and the GT IRQ register ranges ARE listed as VF-accessible. They must not be
described categorically as PF-only or as evidence of the host freeze. The
replacement rationale is the `HAS_MEMORY_IRQ_STATUS` branch in `i915_irq.c`,
whose VF handler/reset/postinstall use memory IRQs without those MMIO accesses.

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

## Follow-up after the first CI checkpoint

- CTB allocation is tied to the initializing object and current thread, checks
  the native size/type/flags, and records the exact enlarged backing. Channel
  initialization rejects mismatches and live-ring reinitialization before any
  native layout writes. CPU mapping publication now follows descriptor setup.
- Quarantine now retains the CTB object as well as its backing: a preempted
  sender may hold a captured queue-lock pointer, so retaining backing alone
  cannot prevent a freed-lock access. Native `withOptions` failure and GuC free
  release the CTB object, so retention also defers its free/ownership-transfer
  path. This intentionally retains additional objects and accelerator state;
  it is NOT optimized teardown or support for restart/unload.
- CTB submission admission requires the firmware's enable acknowledgement;
  published mappings or partially registered KLVs are insufficient.
- The `createUkContext` route returns the native `0x400` failure sentinel if VF
  transport initialization failed, preventing the initialization fallback from
  creating legacy MMIO contexts. Native `initWithOptions` checks this sentinel
  at `0x1ff61`. Zero-context-count and other initialization exits still need review.
- Bootstrap RESET is one-shot: a concurrent caller or retry after incomplete
  initialization cannot reset firmware beneath partially initialized state.
- Removed the generic out-of-BAR INDEX2/DATA2 fallback from common MMIO access.
  Reads/writes now check alignment and subtraction-based bounds. Removed unused
  `readReg64/writeReg64` methods, which indexed a 32-bit pointer and truncated
  64-bit writes. No project call sites existed; prior code remains in git.
- The actual bounded ring copy now shares the tested helper. Added 1,277,856
  copy cases checking payload, head advancement, unchanged output on rejection
  and output canaries under ASan/UBSan, alongside 9,621,504 framing cases.
- Modern GuC memory-IRQ SW_INT_0 is a VF migration notification according to
  `intel_iov_memirq.c`, not Apple's legacy software interrupt bit 36. Migration
  currently faults/quarantines submission rather than dispatching the wrong
  handler. Migration recovery is not implemented.
- Clang static analyzer ran over all 11 built C++ units, logs in
  `/tmp/ngreen-analysis.PD6Iu7`. It reports possible null MMIO callbacks,
  DisplayMergeNub dictionary-copy leaks and dead stores. This is automated
  analysis, NOT completion of the requested full source/reachability review.
  The reported null callback path in `raWriteRegister32` now has an entry guard.

## Context submission/retirement audit (VM still stopped)

CI run 36215583625 for `aaf14a4` passed the macOS build and offline tests.
This is build evidence, not a successful acceleration baseline.

- Native `IGMappedBuffer::initWithOptions` stores the requested byte count at
  object offset `0x20` (`0x13b7d` saves the argument, `0x13c48` publishes it);
  `fillIfRequested` uses it as the fill bound. CTB initialization now checks
  that field against the enlarged backing before native channel writes.
  Context attach/submit similarly check the largest accessed register-image
  offset. These checks do not prove mapping ownership or concurrent unmapping.
- `IGHardwareContext::initWithOptions`, `0x7bfcd..0x7bfec`, derives descriptor
  LRCA directly from the image's GPU VA, without adding a page. Its register
  image is at CPU backing + `0x1000`. Attach now bounds the whole requested
  image length inside the VF GGTT window instead of checking only its first
  page. Actual page-table contents/ownership still need verification.
- i915 `intel_lrc.c:init_vf_irq_reg_state` is called by `__lrc_init_regs` for
  initial restore; the accompanying comment says later GPU saves recreate
  this state. Memory-IRQ command initialization has moved from every submit
  to the new-slot owner before REGISTER_CONTEXT. Repeated attach does not
  rewrite an active context image. Full per-engine layout validation remains.
- The old submit-state-check / CTB-enqueue gap allowed final detach to enqueue
  disable/deregister first. Submission now owns the pinned native H2G lock
  across admission, backing identity validation, tail write and enqueue.
  Final detach acquires the same lock before claiming retirement. Lock order
  is H2G -> context spinlock. Neither completion waits nor original detach
  run while holding H2G. The FAST sender accepts that exact already-held lock
  to avoid recursive acquisition. G2H lifecycle handling only takes the context
  spinlock and does not acquire H2G while holding it. Broader native-caller
  lock ordering, retry/backpressure and externally updated ring tails remain
  audit items; this is not a complete concurrency proof.
- Final detach claims the last reference exactly once. Attach rejects reference
  overflow or mismatched backing/descriptor/engine identity. Submission checks
  a live reference before image access. Native duplicate attach creates proxy
  bookkeeping, so successful duplicate attach/detach calls remain balanced.
- IRQ-context or interrupts-disabled callers cannot enter the new queue guard,
  FAST sender or synchronous GuC wait. Other mutex users and preemption-disabled
  call paths still require review. The VA getter at `0x10b60` simply reads
  backing + `0x38`; it does not acquire another lock.
- Pre-memory-IRQ submission now returns failure instead of reporting success
  without submitting. The old first-stamp workaround and caller failure paths
  must be reconciled before dynamic testing; this may deliberately expose an
  initialization failure previously hidden by fabricated progress.

The host-freeze cause remains unproven. VM boot, EFI installation, GPU work and
Sunshine configuration have not been attempted during this static audit.

## PCI capability gate and next transport finding

`9c705c2` passed macOS CI run 36216348082, including native kext link after
adding interrupt-context checks. No artifact from that revision was installed.

`kern_gpu_capabilities.hpp` records exact PCI IDs and `has_sriov` membership
from the local i915 tree's `include/drm/intel/pciids.h` and `i915_pci.c`.
Identification reads VF_CAP only for a known capable platform; known non-SR-IOV
platforms do not touch that Gen12 register; unknown/uninitialized IDs reject
identification without a speculative MMIO read. Device ID is captured at
`kern_green.cpp` PCI discovery before this project's configRead16/32 hooks.
The test compared all 65,536 IDs against independently expanded primary-source
macros (70 capable / 65 known non-capable). This does NOT establish acceleration
support for all those GPUs, nor remove the remaining CPU-model topology hacks.

Additional blocker found in `intel_guc_ct.c`: H2G availability is not sufficient
flow control. `ct_send_nb` also reserves G2H credits for asynchronous lifecycle
and TLB replies, leaving one quarter of G2H storage for unsolicited events.
The bridge currently bounds each copy and the H2G queue, but does not reserve
future G2H response space. This must be addressed before boot testing.
Completion lengths and enable-before-disable token ordering were compared with
`intel_guc_sched_done_process_msg` / `intel_guc_deregister_done_process_msg`:
Linux likewise requires at least two/one payload dwords and consumes pending
enable first; it does not interpret the second scheduling payload as status.

## G2H accounting and truthful completion follow-up

- Added atomic G2H reply-credit reservation before H2G publication: 4 dwords
  for MODE_DONE, 3 for DEREGISTER_DONE/TLB_DONE, matching i915's payload counts
  plus CT/HXG headers. Capacity is 3071 dwords, retaining the 4 KiB unsolicited
  reserve and empty/full sentinel in a 16 KiB receive ring. Timed-out operations
  keep their reservation; only matched completions return it. Duplicate TLB
  replies cannot refund twice. The supported fixed completion sizes are now
  exact, since larger unnegotiated replies invalidate the credit contract.
- Pure accounting tests exhaust 102,505 small-capacity combinations, check
  full production capacity, rejected over-refunds and UINT32 overflow under
  ASan/UBSan. These do not test real interrupts, firmware or DMA ordering.
  Credit release still occurs on the software workloop rather than Linux's
  receive tasklet; broader call-context/backpressure behavior remains under
  review. Send retries are bounded, not indefinite completion waits.
- Native CTB `hostToGuCAction` (`0x1f600`) has one direct caller, the GuC wrapper
  tail call at `0x21827`, already rejected by the VF route. This bridge sends
  FAST requests only. Response-type messages now halt instead of being fed to
  a nonexistent legacy waiter, particularly preventing FAST failure responses
  from leaving a context falsely treated as operational. Indirect reachability
  remains part of the full binary review.
- DEREGISTER_DONE keeps the LRCA associated with its retained tombstone until
  the retirement owner finishes native detach. Attach waits during that gap;
  only then can the ID/backing be reclaimed. Any protocol fault keeps backing
  quarantined even if a later event says deregistration completed.
- Removed the global V221 waitForStamp override and its startup flag. It used
  to change **any** pre-start wait error into success and fabricate outStamp,
  including for IOAccelerator clients other than the selected GPU. Native
  completion results now remain intact. This can expose the original startup
  sequencing failure; that failure must be fixed, not hidden. The VF startup
  wrapper also no longer registerService()s an accelerator whose start failed.

PCI-gate checkpoint `839de9d` passed native macOS CI run 36216611987. This later
credit/completion batch is still an offline safety change, not a boot candidate.

## Native doorbells and idle-query reachability

`9c09653` passed macOS CI run 36216977521. VM remains shut off, autostart
disabled; read-only libvirt inspection reports 16 vCPUs and 16 GiB RAM.

- `acquireDoorbell` (`0x212da`) can write/poll `0xcee8`, then touch `0x2030`
  before its old `0x10` request. `releaseDoorbell` (`0x21626`) accesses `0xfd4`,
  `0x1000/0x1004` register banks and `0xc530` before its `0x20` request.
  `allocUkDoorbell` (`0x21992`) has a separate direct `0xcee8` loop, and
  `reacquireDoorbell` (`0x218da`) retries acquisition. All four VF entry points
  now reject before these native bodies; PF keeps its original implementation.
  Failure is explicit (native invalid ID `0x100` / false), not fake allocation.
- `createUkContext` (`0x204c4`) only creates legacy software proxy storage/work
  queues, but has unchecked allocation paths: the WorkQueue result is read at
  `0x20628` without a null check, and conditional transferOwnership remains
  reachable at `0x20698`. Its failure cleanup and ownership call remain blockers.
- Original scheduler-4 `isGpuIdle` (`0x1da82`) calls GuC `isGuCIdle` (`0x22382`),
  which reads legacy WorkQueue/proxy fields through `isContextIdle` (`0x21b2c`).
  `isKmdContextIdle` (`0x223cc`) likewise reads an old proxy slot. Direct-LRCA
  submission never updates those fields, so they cannot establish modern idle.
  These three GuC methods and the scheduler idle wrappers now use conservative
  modern lifecycle snapshots: enabled/pending contexts are not declared idle;
  only known non-executing states qualify. Missing state/fault rejects idle.
  An idle snapshot is NOT a submission barrier or device-DMA-stop proof.
- Native `waitForGpuIdle` (`0x1da94`) has a bounded polling loop but a void
  return. Its callers may proceed after timeout. Enabled-context completion,
  watchdog policy, sleep/resume and actual teardown must still be reconciled
  before boot testing; conservative queries alone cannot make these safe.
- Force-wake now requires confirmed physical identity, including failure and
  pre-identification cases, instead of relying on VF transport readiness.

## Mailbox deadline and GuC address-window follow-up

`78501e8` passed macOS CI run 36217266991; no VM boot or installation.

- `intel_guc_send_mmio` allows 10 ms to obtain a GuC-origin reply, then 20
  one-second BUSY intervals for a VF. The bridge's previous 20 ms BUSY budget
  was too short. It now uses absolute clock deadlines, briefly spins for the
  fast response and sleeps 1 ms between later reads, retaining mailbox ownership
  throughout. Timeout still poisons the mailbox; only explicit RETRY permits
  replay, with the existing four-attempt limit. This avoids 20 seconds of CPU
  busy-waiting. Preemption-disabled callers and wider lock ordering remain open.
- MMIO failure error `0x107` is VF_MIGRATED (`guc_errors_abi.h`), not an ordinary
  retryable operation error. It now faults/quarantines the mailbox because
  migration recovery is unimplemented. Unknown response types also poison it.
  Success preserves the already-validated header, matching Linux's response
  copy rather than rereading scratch[0].
- `intel_guc.h` defines `GUC_GGTT_TOP = 0xFEE00000`; higher addresses bypass
  GGTT translation even if the VF's assigned window otherwise contains them.
  CTB/shared-memory-IRQ and context backing must now fit below that boundary.
  Lower WOPCM/pin-bias restrictions and allocator ballooning still need audit.
- PF provisioning evidence: `intel_iov_provisioning.c:pf_provision_ggtt`
  allocates VF regions between the PF GGTT pin bias and GUC_GGTT_TOP. VF KLV
  base/size therefore inherit that lower bound from this trusted host driver;
  the guest must still stay inside the reported interval. This does not prove
  every native allocator respects the interval or that mappings are pinned.
- Routed native `transferOwnership` on the TGL payload: physical devices keep
  the original; a VF preserves its native no-op when flag `0x20` is absent,
  and faults before any PCI-config ownership command if that unsupported flag
  is present. This contains the identified explicit ownership-transfer path,
  not all possible unmap paths. CTB init already checks protocol fault on exit.
- The accelerator-start route is mandatory for VF/unknown identity rather
  than merely logging a failed hook and letting native startup proceed.
- Removed V111's deviceStart false-to-true override. Failed initialization
  remains failure on both PF and VF; an unready VF previously met its fallback
  condition because that condition tested readiness, not hardware identity.

### Display property merge review (static, no VM test)

- Reviewed DisplayMergeNub.cpp and its header in full. Probe now validates
  provider and property types before dereferencing them. Removed the shallow
  property-table merge that overwrote nested provider dictionaries before the
  intended recursive merge; writes now use setProperty. Rename occurs only
  after successful merge.
- Released temporary dictionary copies on both merge success and failure;
  checked recursive iterator allocation; allocation failure cannot inherit a
  previous successful result. Empty dictionaries are successful no-ops.
- Bounded recursive merge to 16 levels, including cyclic dictionary inputs;
  atomically claimed the existing deliberate metaclass lifetime reference.
  This preserves that legacy keep-loaded policy, not a proof it is necessary.
- Syntax checks and offline ASan/UBSan ring/capability suites pass in
  /tmp/ngreen-static.dgSJeZ. Clang static analysis of DisplayMergeNub.cpp now
  reports no warnings. These tests do not execute IOKit property merging;
  concurrent property updates and partial top-level merge remain limitations.
- Commit 9848bc0 built successfully in GitHub Actions run 36217644463.
  No artifact installed and VM remains unstarted.
- Next teardown issue: IGHardwareGlobalPageTableUnmapRange currently calls
  native unmap and only logs a PF relay failure. Because the API returns void,
  merely faulting later cannot prove the caller retains the underlying pages.
  This is an unresolved DMA-lifetime blocker, not evidence of safe teardown.

### Direct GGTT initializer aperture overflow (static binary evidence)

- Payload inspected: v213 AppleIntelTGLGraphics, SHA256
  `1b2f5aa3131f9b909fe984877e41d5ebcdb2837572b45e1e901a72a6271515a2`.
- initWithOptions at 0x101b4 stores the BAR base/PTE pointer/dummy page, but
  does not store its range argument. The first PTE loop uses start through
  start+length-1 (0x10227..0x1029a). Unless length is exactly 4 GiB, the second
  loop writes dummy PTEs from start+length through start+4 GiB, exclusive
  (0x102c0..0x1030f). A nonzero VF base therefore makes this second loop reach
  beyond the full 8 MiB PTE aperture. A zero length is not a safe suppression.
- Applied the existing relay-path no-loop arguments to direct VFs too:
  `{UINT64_MAX, 4 GiB}`. Unsigned wrap skips the first loop; the exact length
  skips the second. This is specific to the inspected native implementation.
  The real allocator interval is unchanged and PF initialization is unchanged.
- i915 intel_ggtt.c gen12vf_ggtt_probe installs nop_clear_range for BOTH
  direct and relay VF transports. This supports avoiding physical-style
  initialization, but does not prove all later native PTE flags are suitable.
- New ASan/UBSan offline arithmetic model checks 3,839 nonzero base examples
  and suppressed-loop bounds; added to local checks and CI. Full local suites
  pass in /tmp/ngreen-static.iN8AqF. The model is not execution of the binary.
- This is a definite unsafe argument/loop combination, not proof that the
  prior host freeze executed these exact bounds. Direct map/unmap bounds,
  native TLB invalidation, and DMA ownership are still boot blockers.
- Display merge checkpoint 77e218e CI run 36217922452 succeeded.

### GGTT map admission bounds (not a full mapping-lifetime fix)

- Native mapRange, mapRangeDummy, and mapRangeRotated write PTEs before the
  existing relay-range validation. Added VF identity/readiness/fault and
  page-aligned assignment bounds checks before these native calls. Physical
  device calls retain their existing behavior. Rotated mapping also requires
  non-null source iterator, range descriptor, and physical iterator.
- Shared pure range predicate additionally bounds absolute PTE indices to
  the 4 GiB address aperture. Tested 28,561 combinations against a 128-bit
  arithmetic oracle, including overflow, end-of-window empty ranges, malformed
  alignment, and near-UINT64_MAX inputs. Local sanitizer/syntax suites pass.
- This only validates the rotated iterator's enclosing range. Internal cursor,
  dimensions, divide-by-zero and complete permutation bounds still require
  reconstruction; therefore this path is NOT certified safe for execution.
- Native unmap's void API, partial map/relay rollback, direct PTE store
  atomicity, dummy-page ownership and GuC invalidation ordering remain open.
  Native map/unmap stores high/low halves separately (0x10384/0x10389 and
  0x1066d/0x10672), unlike i915 gen8_set_pte writeq. A bound check alone does
  not prove safety while a GPU can observe an existing valid PTE.
- Initializer checkpoint 55b092c CI run 36218034448 succeeded. No deployment.

### DYLD/header and HDMI reachability review

- Read DYLDPatches.cpp/.hpp and HDMI.cpp/.hpp completely. Removed 217 lines
  of unused AMD VA/VCN pattern tables from DYLDPatches.hpp after repository
  reference searches found no users; recoverable from Git history.
- Restricted the Sonoma-derived CoreDisplay patch block to Sonoma rather
  than every OS >= Ventura. Tahoe must not accidentally receive legacy
  AccessComplete stubs/completion jumps on a coincidental signature match.
  Exact build/UUID validation is still missing inside the Sonoma branch.
- Remaining DYLD issues: non-shared-cache CoreLSKD path filter uses OR of
  two nonmatches, making that branch unreachable; do not silently enable an
  unverified patch by changing it to AND. Shared-cache ICL Metal ID bypass
  still has broad page scope, CPU-based full-Metal mode remains unreliable,
  and one-shot logging flags are unsynchronized. No actual Metal completion
  or hardware video encoding has been demonstrated by these patches.
- HDMI::processKext immediately returns false; registrations and call sites
  in kern_green.cpp are commented out. Its AMD-derived HDA patch body is
  unreachable and does not supply guest audio. No HDMI behavior changed.
- Generation transport investigation: i915 intel_gtt.c selects binder on
  media IP 13.0, not BAR length. intel_device_info.c forbids direct GMD_ID
  reads by a VF; intel_iov_query.c uses per-GT KLV 0x3000 with VF ABI >=1.2.
  The current BAR-only transport selection lacks this discovery and remains
  a blocker for claiming MTL/ARL or general Gen11+ support.
- Map-admission checkpoint f43f222 CI run 36218171344 succeeded.

### Physical PHY translation layout review

- Reviewed IntelDPLinkTraining.cpp/.hpp and compared Linux
  intel_ddi_buf_trans.h/.c, intel_combo_phy_regs.h and intel_ddi.c signal-level
  routines. All 50 local table rows exactly match the five numeric fields
  in the installed i915 source, but the local STRUCT ORDER was wrong:
  a nonexistent iboost displaced swing/N-scalar/cursor fields. Corrected to
  swing, N-scalar, cursor, post2, post1 and consume both post fields.
- Masked SWING_SEL_UPPER to its single bit, so even malformed inputs cannot
  spill into neighboring fields. Validate lane count 1/2/4, non-null arrays
  and all four lanes' legal swing/pre-emphasis combinations before MMIO.
- Both public entry points now require confirmed physical identity; VF or
  unknown identity cannot program display PHYs. Limit this implementation
  to PHY A/B and DP/eDP; its tables do not implement HDMI training.
- ADL-P eDP HBR2 no longer selects the DP/HBR3 table. The boolean-rate API
  cannot represent HBR3; complete negotiated-rate/table selection, panel
  high-output-swing policy and eDP override sequencing remain unfinished.
- Removed the active ICL hwInitializeCState calls that forced two four-lane
  HBR links using TGL electrical tables outside actual link training. Native
  ICL initialization remains. No new signal-level call sites were enabled.
- Added offline struct-offset/register-value/table-range/bit-mask tests,
  all 65,536 swing/pre pairs, and all input lanes/counts; CI includes them.
  These tests do not exercise physical links or prove PF display support.

### Retired HDMI module removal

- Removed HDMI.cpp/.hpp, the unused agfxhda object/include, commented call
  sites, and all eight Xcode project references after the reachability review
  above. Its entry point unconditionally returned false and registration was
  disabled; no functioning audio path was removed. Git retains the old code.
- Local checks now compile 10 active C++ units (retired kern_netdbg.cpp is
  still excluded), plus all offline sanitizer suites. Guest audio is a
  separate unfinished requirement; this cleanup does not implement it.
- DYLD checkpoint 9c13c7e CI run 36218296554 succeeded.

### Patcher fallback location review

- Read kern_start.cpp and kern_patcherplus.cpp/.hpp. Replaced first-match
  fallback routing/symbol lookup with bounded unique-pattern preflight:
  accept offset zero, reject missing/ambiguous (including overlapping)
  matches, null/empty ranges, all-wildcard signatures and address overflow.
  Symbol-based routing is unchanged when it succeeds.
- Downloaded upstream acidanthera/Lilu into /tmp/ngreen-lilu.BEuBI9 and
  inspected tag 1.7.2, commit e4748cc081bf060302c7d3c44a643ce1d11b7e1d,
  matching the vendored bundle's declared version. Upstream HEAD at download
  was 0515f40b7f2a096adc85e832a4c6104fbd07f936. No dependency was replaced.
- Lilu findPattern compares `(data & mask) == pattern`, requiring premasked
  patterns. The new matcher preserves that semantics (does not broaden
  matching by masking the pattern). Offline tests cover 173,740 cases plus
  partial-bit masks, non-premasked rejection, first/last byte and null inputs.
- Lilu routeMultipleInternal clears errors on failure, but populates `from`
  before attempting the route. If a symbol resolved and routing then failed,
  no pattern retry is allowed: the first attempt may already have modified
  code. This is containment, not rollback of partial writes.
- Further finding: Lilu masked replacement reports any positive replacement
  count as success, even if the requested count was not reached. Its plain
  lookup path excludes the final eligible offset. LookupPatchPlus still
  needs count/bounds preflight and failure-atomicity review.
- PHY checkpoint f536b68 CI 36218492866 and retired-HDMI checkpoint ce7898f
  CI 36218582458 both succeeded. Still no installation or VM startup.

### Lookup replacement preflight

- LookupPatchPlus now requires valid explicit image bounds, non-null
  replacement, a loaded kext if specified, and enough non-overlapping
  candidates for skip + requested count before any replacement starts.
  count=0 still means all remaining matches, requiring at least one.
  Overflow in the requested count/range is rejected.
- Plain and masked patterns now share Lilu's inclusive masked replacement
  implementation; this avoids the plain implementation's final-offset bug
  and keeps kernel-write protection handling inside Lilu. Examined all local
  LookupPatchPlus call sites: they supply image or symbol-bounded ranges.
- The same 173,740 generated inputs now additionally compare six required
  match counts against an independent non-overlapping oracle, plus null and
  end-of-image tests. Local checks pass in /tmp/ngreen-static.QkxLvy.
- This is NOT a transactional patch system: other patchers can mutate memory
  between preflight and writes; applyAll can partially apply before a later
  patch fails; upstream logs a failure to restore write protection without
  returning failure. Those remain review items. No VM test is authorized by
  a successful preflight alone.
- Fallback checkpoint aa1babd CI run 36218749302 succeeded.

### Legacy renamed ICL framebuffer / DVMT review

- Read kern_genx.cpp/.hpp. Its renamed ICL framebuffer path was independently
  reachable and not VF-aware. For VF/unknown identity it now routes native
  probe/start to rejection before installing legacy clock/DVMT/PM patches;
  mandatory route failure is fatal rather than allowing physical startup.
  This does not implement a virtual display or cover other framebuffer paths.
- Removed four active empty sleep/wake hooks from the physical path; native
  transitions are preserved. The unused AUX wrapper also checks read success,
  non-null buffer and full DPCD capability length before inspecting fields.
- The old DVMT scanner ignored write-enable failure, could copy more bytes
  than its NOP buffer, did not verify register operands/width, and repeatedly
  mutated its MOV opcode on every remaining loop iteration after a match.
- Replaced it with bounded decoding of a padded local 15-byte window, checked
  image bounds, a maximum of 64 instructions and return/tail-jump termination.
  Only adjacent 32-bit SHL reg,17 + AND same-reg,0xFE000000 are accepted.
  It writes once and stops; failed write permission aborts, failed protection
  restoration is fatal. A missing pair leaves native code unchanged.
- The replacement is MOV reg,stolen_size + TEST reg,reg + NOP padding:
  unlike the previous MOV-only sequence it supplies the defined logical
  condition flags (SF/ZF/PF, cleared CF/OF; AF is unspecified) for the new
  result. Rejected encodings leave the output buffer untouched.
- New offline tests cover 26,688 register/immediate/corruption combinations,
  exact output guards, accumulator-specific AND and short/null buffers.
  Full suites passed in /tmp/ngreen-static.qXt19o before the additional
  framebuffer admission guard; subsequent full validation is required.
- Remaining limitations: no exact payload UUID/function-length allowlist,
  optional patch may not match, BIOS stolen-memory truth and generation-wide
  physical display/clock/PM behavior are not certified. Other unused legacy
  wrappers and commented patch experiments still need removal/review.
- Lookup preflight checkpoint 261ca1a CI run 36218855810 succeeded.

### Embedded DMC payload boundaries and VF admission

- Firmware.cpp/FirmwareADLP.cpp contain literal firmware data plus sizeof
  declarations, not host-side algorithms. Compared every payload byte to
  installed linux-firmware containers, then fetched the matching upstream
  GitLab linux-firmware/main/i915 files and verified identical SHA256 hashes:
  TGL 3c013ef0ad96ba73aee8e5bd04a8e27cc9b1c6e9183b1a83ce124485f325afca;
  ADLP 2da482ea46a40e54c9ca3b54185959177f393eff98ece21acdac7eb6cacb0fcb.
- Both main payloads start at file offset 0x310, following v3 headers at
  0x210. TGL declares fw_size=0x116c dwords (17,840 bytes), while the old
  embedded array had 18,976 bytes: it incorrectly included the next 256-byte
  header at 0x48c0 and its 880-byte payload, destined for SRAM 0x90000.
  Removed that 1,136-byte tail from the main array. Git retains the old data.
- ADLP main fw_size=0x1833 dwords (24,780 bytes) was already correct. Fixed
  its loader comment: this is the main image, not pipe-A. Added compile-time
  payload-size assertions and tools/check-dmc-blobs.sh for repeatable byte
  comparison against pinned source containers. The script is read-only.
- hwInitializeCState now rejects VF/unknown identity before private-object
  reads, DMC writes, power-well programming OR the native fallback. The void
  callback faults admission rather than pretending this is a VF display
  implementation. Complete framebuffer startup still needs separate work.
- Remaining physical-path defects: omitted per-pipe payload handling,
  boot-argument rather than stepping/IP-based selection, hardcoded timing
  and power-well values, mixing native ICL firmware with newer context
  registers, and global controller pointer ownership. These are not made
  safe by correcting payload length. Firmware instruction semantics remain
  opaque; byte integrity is not a firmware-internal source review.
- Full offline suite passes in /tmp/ngreen-static.3OKBXc. DVMT checkpoint
  a580586 CI run 36219197807 succeeded. No dynamic test performed.

### Legacy proxy context allocator wrap and exhaustion

- Rechecked the pinned TGL payload's createUkContext (0x204c4), allocator
  (0x21066), release (0x2117c), pool setup (0x20b34), and both allocator
  call sites (0x21162 and 0x21ea7). Both callers hold GuC+0x40; replacement
  must not recursively lock it. Owner is unused in this native allocator.
- Native next-ID scanning wraps the numerical index at 0x2110d but leaves
  the flag pointer advancing beyond count*0x5b00 at 0x21119/0x2111c. Full
  allocation also sets next=0x400; release decrements used without restoring
  next, so subsequent allocation refuses even after a slot is freed.
- VF allocContextId now validates count<=1024 and actual mapped-buffer length,
  uses bounded record-index arithmetic on every iteration, and preserves the
  used/next/flag bookkeeping and optional record clearing. Full pools return
  the native invalid-ID sentinel; invalid input/occupancy faults the protocol.
  PF keeps its native allocator. Negative UK priorities and null receivers
  are rejected before calling native createUkContext.
- Tests exercise 8,192 exhaustive small-pool occupancy/start/clear combinations,
  all bytes of changed and unchanged records plus canaries, complete 1024-ID
  exhaustion, reuse after release with the legacy sentinel, and malformed
  inputs with no writes. ASan/UBSan and full syntax suites pass in
  /tmp/ngreen-static.2nsDqH. DMC checkpoint 9c15e9f CI 36219426423 succeeded.
- This is NOT a complete createUkContext rewrite: its work-queue OOM null
  dereference at 0x20628, reserved-ID leak after buffer allocation failure,
  earlier native attach pool scan, teardown exclusion, payload ABI validation,
  and release-path bounds still need work. The new allocator cannot certify
  callers' entire lifetime. No VM boot, installation or hardware submission.

### Unreachable legacy GuC firmware swapping removed

- Whole-repository reference checks found no routes/callers for the old
  wrapLoadGuCBinary, wrapLoadFirmware, firmware-buffer swapping and sleep/wake
  wrappers. Their function pointers were never resolved. Removed those six
  dead wrappers and their private state (recoverable through Git).
- They contained a scalar-size/pointer mismatch, null firmware/signature
  placeholders, private buffer pointer replacement, unverified release on
  wake, and unchecked write-protection restoration. These were dormant bugs,
  not evidence that they caused the observed freeze.
- The active wrapInitSchedControl hook existed only to toggle that unreachable
  firmware-swap state. Removed this no-op detour and resolve the native symbol
  directly for VF scheduler allocation instead. A missing symbol fails closed
  before use. Native PF scheduler calls are no longer needlessly detoured.
- Full syntax and ASan/UBSan suites passed in /tmp/ngreen-static.6ZBcDm.
  Actual PF firmware support, VF allocation OOM handling, and PM lifecycle
  remain separate unresolved tasks; no runtime behavior has been certified.

### Separate ICL/TGL firmware entry ownership

- Both hardware payloads routed loadGuCBinary through the same saved original
  slot and selected layout using global tglHWLoaded. If both loaded, an ICL
  receiver could enter TGL scheduler code or the wrong original trampoline.
  Added an ICL-only wrapper and original slot; ICL rejects non-physical
  identity, while TGL independently selects its verified VF identity path.
- Removed the physical non-TGL fallback that returned firmware-load success
  without loading firmware. It now returns failure. This is NOT new PF
  support; the remaining physical TGL CPU-based gate needs GPU-IP/payload
  replacement. Other shared original slots still need individual review.
- VF scheduler initialization cannot return success after a protocol fault.
  Native initSchedControl still ignores setupLogBuffers and ADS return values
  (0x20af1, 0x20af9, then unconditional AL=1); its internal allocation and
  mapping failure contracts are an outstanding blocker, not fixed by this.
- Full offline suite passed in /tmp/ngreen-static.N3QM5d. Proxy allocator
  checkpoint e2b205d CI 36219798073 succeeded. VM remains off.

### Main physical framebuffer admission on VFs

- AppleIntelBaseControllerstart wrote DC_STATE_EN, PCH clock gating/reset
  handshake and display chicken registers before native start. The previous
  DMC guard was too late to contain this path. Added an immediate non-physical
  rejection before any of these writes.
- For all three main ICL/TGL framebuffer identities, processKext now routes
  base probe and derived controller start to rejection for VF/unknown devices,
  before physical patch installation and before the old ICL-skipped-if-TGL
  branch. Mandatory route failure remains fatal. The separate renamed ICL
  path already has equivalent admission handling in Genx.
- Verified these TGL symbols in local framebuffer payload SHA256
  285ee7a9c3d6c9a9647013f44fb40f312b1cb42879974ef0542544cf049442b5:
  base probe 0x60ac2, derived controller start 0xdd828, base start 0x5a8a0.
  This is service-admission containment, NOT proof of all constructor/free
  behavior or support for every other binary variant.
- This deliberately prevents physical display startup on a VF. A real
  headless accelerator/virtual-display integration is still required;
  Sunshine/Moonlight is not implemented by this rejection. Physical devices
  retain the existing display path, which still needs generation-wide review.
- Full offline suite passes in /tmp/ngreen-static.fk4vFW. CI a0d46bb
  (36219866509) and 4a22593 (36219932023) succeeded. No deployment or boot.

### VF private-layout payload UUID admission

- TGL VF hardware routes now require the reference payload's LC_UUID
  BA3AA1C0-FE6B-33B3-9D85-73F848394E3D before personality injection, symbol
  routing or private-layout use. The reference SHA256 remains
  1b2f5aa3131f9b909fe984877e41d5ebcdb2837572b45e1e901a72a6271515a2.
- New bytewise Mach-O parser checks x86_64 KEXT_BUNDLE, available command
  extent, command count/size/alignment, exact command consumption, unique UUID
  and exact match. It accepts unaligned input without struct dereferences.
  Lilu processKextLoadCallbacks passes the kext base and updated image size;
  MachInfo::getRunningAddresses treats that same base as the Mach header.
- Tested every truncation of a valid fixture, 4096 UUID byte mutations,
  malformed sizes/counts, missing/duplicate UUID, unaligned start and 50,000
  deterministic mutated fixtures under ASan/UBSan. Also parsed the actual
  pinned on-disk payload successfully. Full suite: /tmp/ngreen-static.FQUamG.
- UUID is an ABI version gate, not cryptographic authenticity or proof that
  code was not modified while retaining its UUID. Runtime-patched opcodes,
  loaded-KC layout validation and other payloads still need review. Unknown
  VF payloads fail closed, not silently use guessed offsets. This does not
  certify the known payload's unresolved lifecycle/OOM defects.
- Framebuffer admission checkpoint 6d50aa2 CI 36220035067 succeeded.

### Main device initialization and PCI identity reads

- Read all of kern_green.cpp. The optional legacy saved-config seeding
  passed a zero-length array to setProperty with length 0xea in two places,
  copying beyond the object into an IORegistry property. Both arrays now
  actually contain 234 zero bytes, and copy lengths use sizeof.
- configRead32 treated both offset 0 and offset 2 like vendor/device reads:
  offset 2 instead contains device ID low and command register high. The
  replacement now preserves the correct other half for each offset. Both
  config-read wrappers reject >16-bit spoof IDs and restrict substitution to
  the actual selected IOPCIDevice, not any name beginning with IGPU.
- The shared pure helper passes 16,777,216 device/offset combinations against
  an independent byte reconstruction plus invalid-ID passthrough. This tests
  returned values, not physical PCI writes (none are performed by the helper).
- First syntax pass caught legacy SDK setProperty(void*) rejecting const
  arrays; corrected the buffer declaration and reran the complete suite.
  Successful full run: /tmp/ngreen-static.5bgmHz. UUID checkpoint 83178e2
  CI 36220151307 succeeded.
- Remaining kern_green review findings: null BAR0 map handling, CPU-based GPU
  classification, unverified DVMT fallback, unconditional PCI bus-master
  enabling, global IOAccelFamily capability bypass/mode stripping, dormant
  false-success wrapper, panel-data allocation ownership and partial property
  allocation failure. Reading a file is not closing its review findings.

### Physical GuC firmware device classification

- Replaced the physical TGL firmware entry's CPU-model gate with exact
  pre-spoof GPU PCI ID membership from i915 INTEL_TGL_IDS (11 IDs). A guest
  CPUID model is not the GPU generation. VF admission remains a separate
  identity decision and does not enter this physical firmware branch.
- Extended the 65,536-ID test to check the TGL classification against the
  actual primary-source macro expansion; it passes along with all previous
  capability classifications and the complete suite in
  /tmp/ngreen-static.B4lUv5.
- The many other isRealTGL gates are deliberately NOT mechanically rewritten:
  several currently conflate platform workarounds and physical/VF behavior,
  and need individual control-flow review. Exact TGL membership also does
  not establish stepping-specific firmware compatibility or loaded-payload
  correctness on all physical devices. These remain open.
