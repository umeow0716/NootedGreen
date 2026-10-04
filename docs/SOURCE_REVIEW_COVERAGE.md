# Source-review coverage — incomplete

The user's baseline requires all program files to be reviewed. This ledger
does not certify that requirement as complete, and passing CI is not source
review or hardware validation. The VM must remain off while the protocol
audit's runtime blockers are open.

## Scope

2026-10-04 root snapshot: complete 32/64 PPGTT physical-root getters (0x20
each) reviewed/pinned, concrete slot +0x148 and context task/private-table
snapshot call checked. Existing 32-bit init revisited, not new review credit.
Context CPU-image copies are not yet GuC registration/publication proof;
root visibility and construction-failure admission remain incomplete.

2026-10-04 pre-zero hook scope: complete 32-bit PPGTT expand (0x23e)
reviewed/pinned, including descriptor rollback after software-index allocation
failure. Imported IOMalloc/memset and rollback edge/clear checked. Direct
release caller inventory spans shrink/remap/expand/free, not a complete
indirect/inlined call graph. Hardware publication status at each caller still
must be proven before distinguishing safe unpublished cleanup from retirement.

2026-10-04 concrete table/pool owner references: twenty inherited retain/
release/tagged virtual imports checked across pool, 32/64 PPGTT and GGTT.
Complete Boot release (0x10), taggedRelease (0x20) and threshold overload
(0xa0) reviewed/pinned. Ordinary last-release dispatch is not GPU retirement;
explicit free, special threshold calls and outer owner admission remain scope
limits. No transaction or driver behavior was installed.

2026-10-04 task reference acquisition: complete Boot OSObject retain (0x10),
getRetainCount (0x10), taggedRetain (0x70) reviewed/pinned; concrete native task
virtual imports checked. Atomic count update presupposes live object ownership,
not a borrowed-pointer validity check. Existing native task release revisited,
not new complete-body review. Outer lifetime/admission still unimplemented.

2026-10-04 task-list destruction/observer: complete base task free (0x144),
TaskList removeTask (0x9a), accelerator freeAllGPUMappings (0xba) reviewed/
pinned. Native private-table teardown precedes base raw-list unlink. Selected
observer walks active and orphan lists without local retain/mutex; its outer
caller admission is required. Function-bounded direct iterator-edge inventory
is discovery only, not complete coverage of all inlined/indirect observers.

2026-10-04 Shared construction outer mutex: complete inherited user-client
start (0xea), sharedStart (0x62), createShared (0x50) and native Shared-start
override (0x40) reviewed/pinned. Effective Intel start inheritance, override
dispatch and base-header call pairing checked. Accelerator +0x88 spans the
selected Shared/task factory and failed-init cleanup chain. Other factory
entries/list observers and all owner leases remain outside this proof.

2026-10-04 task factory ownership: complete native withOptions (0x48), kernel
factory (0xa), user factory (0x3c) and inherited Shared init (0x208) reviewed/
pinned. User factory either retains/reuses kernel task or constructs a new
task according to feature bit; concrete accelerator virtual slots checked.
No local lock in these bodies. Caller-side serialization remains unresolved;
same virtual offsets in unrelated classes are not factory call evidence.

2026-10-04 task construction/publication: complete native IGAccelTask init
(0x1aa), inherited IOAccelTask init (0x138) and TaskList addTask (0x14)
reviewed/pinned. Base init publishes task to accelerator raw list before
private PPGTT and managed-table list construction. Base vtable relocation and
header-relative slot verified. Outer factory serialization/construction
visibility remains required; no concurrent partial-task observation claimed.

2026-10-04 selected completion lock graph: re-read existing native software
dispatcher and current poll/drain/parser/readiness/credit/fault/IRQ-gate helpers.
New structural source contract covers twelve selected helper bodies and rejects
four inverse-lock/wait mutations. No new native-body review is credited for
the already-pinned dispatcher. Outer task/table admission, other callbacks,
external APIs and runtime deadlock/DMA proof remain incomplete.

2026-10-04 selected destruction admission: complete SharedUserClient2
delete_resource (0x116) reviewed/pinned. Accelerator mutex +0x88 covers
namespace borrowed lookup and resource Shared-release; native IGAccelResource
inherits that virtual, now checked via import relocation. This proves one
selected deletion caller's outer lock, not all mapping/PPGTT destruction paths
or completion-consumer compatibility while holding that lock.

2026-10-04 event storage destruction: complete Resource2::free reviewed/pinned
(0x41c), including all selected type branches and corruption exits. Mapping
remove_resource and release precede event-pair clear/typed free. Free import
identity and clear-before-free bytes checked; outer serialization, callees'
event drain and retained-owner implementation are not proven by this body.
Complete mapping remove_resource (0x5e) also reviewed/pinned: raw pointer array
lookup, decrement and compaction only, no retain/release, lock or drain.

2026-10-04 resource event-pair producer: complete Resource2 initialize body
reviewed and pinned (0x33e); bit-12 mode sets resource flag before storing the
typed allocation at +0x90 and initializing both events without a null branch.
Complete Boot IOMallocTypeImpl wrapper reviewed/pinned and its import and tail
edge to external kalloc checked. Nullable versus must-succeed behavior still
requires downstream allocation-policy proof; no reachable fault is claimed.

2026-10-04 null entries: existing channel/Fast2 stamp consumers re-read; entry
pointer is dereferenced without null guard. Pinned read anchor and extended
test-only admission predicate to reject null even when exact lists match.
Producer non-null invariant and real owner/pointer validation remain unproven;
not a reproduced fault or deployed repair.

2026-10-04 admission specification: test-only 2,720 event-omission states cover
selected variants/skip/alias/mapping cases and reject same-count substitutions.
Not a runtime fix, instruction equivalence or owner-lease proof. Null semantics,
other copies, ordering and integration remain pending; source audit incomplete.

2026-10-04 duplicate collector: full selected user-client grow/collector copies
reviewed/pinned. Grow identical to CCS, collector has an extra Boolean-controlled
wait omission while retaining update collection. Expected completeness must
respect helper options/resource flags; fixed-count or name-only routing is not
valid. Other copies/callers and failure integration remain unreviewed/incomplete.

2026-10-04 depth caller domains: full user-client and small Metal wrapper
reviewed/pinned. User-client holds accelerator mutex but post-publish cleanup
returns unconditional success; Metal wrapper maps resource false to status10.
Different helper copies and status/admission contracts require coverage.
Large Metal/GL caller bodies and full owner lifetime remain pending.

2026-10-04 native depth publisher: one complete 0x1f6 wrapper reviewed/pinned,
barrier/assembler/imported submit and phase-loop anchors checked. Empty wait
list bypasses aggregate barrier; chunks publish until software phase0xe, not
hardware ACK. Large resolve_hiz_g7 identified only, not whole-reviewed; AL
meaning, partial-progress lifetime and admission failure propagation remain open.

2026-10-04 depth admission: complete resource submitDepthResolve reviewed/pinned
with exact duplicate vector/collection helper copies and submission/free imports.
Initial growth and collection omission have no result gate before submit/state
clear. Shared CCS/depth completeness and effective outer locking remain pending;
callee identity is not a whole-body completion proof. No production change.

2026-10-04 bind pairing: native bindResource and KC dirty/channel-add complete
bodies reviewed/pinned. prepare false skips channel-add; post-bind cleanup
requires both channel state and preparation. Channel +0xa0 is distinct from
prepare +0x28 and OSObject lifetime. Storage recursion/dirty subordinate
helpers and complete owner domain remain pending; no driver change.

2026-10-04 borrowed lookup: three complete KC wrappers/lookup/channel-cleanup
bodies reviewed and pinned, plus native +0x178/+0x198 cleanup identities.
Namespace lookup has no retain; removeFromChannel updates events/counts rather
than releasing a lookup reference. Existing mutex scope cannot be casually
dropped across waits. Preparation/subordinate callbacks and leases remain open.

2026-10-04 color_resolve: one full 0x5d0 user-client body reviewed/pinned with
mutex/busy/lookup imports. Holds accelerator +0x88 across selected CCS resolve,
maps false to 0xe00002c2, then resource cleanup/busy/mutex release. One concrete
effective lock is proven, not all caller domains or owner retention. Waiting or
dropping that lock requires callback/lifetime analysis. No new driver change.

2026-10-04 outer CCS state: two full wrapper/enable bodies reviewed and pinned.
Plane wrapper ANDs status while retaining partial progress; compression enable
sets flags before selected resolve and does not propagate its Boolean. Effective
outer state/locks, depth helper users and render/user-client propagation remain
open. Auxiliary map/release identified, not wholly reviewed. No production change.

2026-10-04 event-result semantics: existing full merge body re-read; final AL
queries aggregate completion, not merge-error status. Two native barrier/finish
wrapper bodies newly pinned with effective imports. Fallback waits aggregate,
not all stamps; 0x2b indexes statistics. Missing pre-merge events remain uncovered.
No new driver patch; effective event admission/owner serialization still pending.

2026-10-04 implemented VF CCS null repair: 34-byte bounded setup rewrite enters
verified false cleanup on null, preserves successful vector/r12 and neighbours.
Exact start/end, zero-state and cleanup preflight required. Full offline suite
and separate selected actual x86 emulation passed; event dependency failure,
cross-owner admission/retirement and runtime safety remain unproven. Not deployed.

2026-10-04 CCS cleanup: imported rectangle allocation/free and both false-path
vector frees pinned; false cleanup can accept empty rectangle allocation state.
Two complete event-vector grow/append helpers reviewed/pinned. Failed event
append is silently skipped without aggregate status, so a rectangle-null repair
cannot certify dependency coverage. Bounded branch rewrite and outer submission
failure propagation remain to implement; no runtime changes this checkpoint.

2026-10-04 resource CCS: one complete 0x554 body reviewed/pinned. Downstream
submission precedes resolve-state clearing and successful return, with no local
submission status gate. UNREPAIRED allocation-null path keeps r12 zero and can
reach rectangle stores. Anchors preserve evidence of the defect, not certify
correctness. Cleanup/import semantics and effective outer lock remain pending.

2026-10-04 CCS submission: complete submitCCSResolve reviewed/pinned with cache
update, resolve assembly and relocated command-buffer submission identities.
No local update-result gate precedes downstream submission. Function-bounded
direct inventory found 87 resource/video-wrapper edges across 29 native owners,
including Metal render/compute/blit, GL and media; inventory is NOT whole-body
review of these owners or indirect-call closure. Shared admission and effective
outer locking must encompass more than video wrappers.

2026-10-04 resource cache update: two complete small resource/video wrappers
reviewed and pinned. Resource flag update precedes borrowed mapping dispatch;
video capability-gated wrapper requests cache type 1. No local retain/lock or
failure recovery. Function-bounded inventory is required: linear text decoding
can stop on invalid bytes. Larger blit/video parser callers are NOT whole-reviewed
by this checkpoint; outer synchronization remains open.

2026-10-04 mapping update caller: two additional complete native bodies and
declared +0x180 dispatch pinned. Cache-type software field changes before an
installed mapping's update; local caller has no failure restoration. Mapping
wrapper borrows task/accelerator owners, forwards manager result. Effective
outer serialization and indirect caller/override coverage remain incomplete.

2026-10-04 update fanout: two additional complete native bodies reviewed and
pinned (manager updatePageTableForTask and hardware updateRange). Raw task-list
traversal continues after a failed update; segment remap failure has no local
prefix rollback, and deferred flush is not an ACK. Descriptor virtual references
do not prove task/table/pool ownership. Update admission must join the pending
cross-owner retirement transaction. No runtime safety certification.

2026-10-04 concrete Intel VA-free: one new complete override and declared
slot/base-dispatch anchor reviewed/pinned. It only adds diagnostics around
base VA release; no added ACK boundary. Existing page recycle before outer
VA free means retirement cannot be repaired only there or at free-list publish.
Cross-owner pre-zero acquisition/admission/submission/destruction integration
remains unimplemented; fixed graph is not hardware safety proof.

2026-10-04 stolen purge disposition: four whole bodies reviewed/pinned;
declared setPurgeable operation 4 only updates software state, not backing
release/PTE invalidation/GPU quiescence. Pool deallocation separately returns
allocator range/unlinks descriptor without local ACK. Final destruction and
pool ownership remain incomplete; prioritize cross-owner PPGTT admission and
retirement rather than treating purge as a completion gate.

2026-10-04 power subordinate ordering: seven new full-body fixtures include
purge, scheduler/base forwarding and previously edge-reviewed bridge helpers.
Corrected Intel ordering to scheduler -> stolen-pool purge -> base cleanup ->
bridge sleep. Base scheduler sleep only manages its timer; no local GuC/GPU
retirement. Purge callee disposition and finishAllStamps dependencies remain
open. Native power-path correctness and DMA quiescence are not certified.

2026-10-04 effective power graph: three complete system_will_sleep/Intel
sleep/wake bodies reviewed/pinned. True-mode caller holds accelerator mutex
while Intel override delegates to the base sleep slot, connecting the archived
iterator defect to this declared graph. False-mode display scope differs.
Scheduler/bridge/engine quiescence and actual dynamic admission remain open;
lock ownership is not DMA completion and the iterator defect stays unrepaired.

2026-10-04 sleep/wake: four complete bodies reviewed/pinned, including task
iterator semantics. True-mode wake cleanup holds accelerator mutex; sleep
caller locking unresolved. Archived sleep's second iterator is built at -48
but read at exhausted -30, so its orphan-task cleanup is skipped. This defect
is explicitly UNREPAIRED; fixture success pins bad reference bytes, not correct
sleep behavior. No guest-suspend/Host-panic attribution or runtime patch.

2026-10-04 Shared user-client teardown: four whole bodies and declared
sharedStop slot reviewed/pinned. Active stop holds captured accelerator +0x88
mutex and an explicit accelerator retain while sharedStop releases its object.
clientClose uses a distinct client mutex and can defer termination; success
is not a drain. Other actual classes/last-reference callers remain unresolved;
one established locked path is not universal teardown or DMA safety proof.

2026-10-04 Shared destruction: three complete Shared/resource/orphan-pool
bodies reviewed/pinned, plus resource virtual identity and task cleanup edges.
Shared task is pruned/released/cleared after resource/pool processing; borrowed
task pointers cannot survive async work without explicit owner references.
No local accelerator mutex acquisition, but effective caller locks and pool
disposition helpers remain unverified. This does not certify DMA quiescence.

2026-10-04 G2H application ordering: VF source repair holds existing G2H mutex
through event application instead of only frame copy, with locked readiness
recheck and scoped all-return release. Native software handler body pinned;
three guard/admission/unlock mutation regressions reject unsafe structures.
Selected TLB wait/poll/application path has no accelerator/H2G/global-GuC
mutex acquisition in its consumer; all external caller/teardown dependencies
are not proven. Offline suite passes; repair remains undeployed/unvalidated
against hardware, and does not implement native PPGTT retirement.

2026-10-04 task allocator ownership: complete init/allocate/deallocate bodies
and declared +0x140/148 targets reviewed/pinned. Supplied allocator references
are retained, but options/class provenance remains unresolved. Full local XNU
IORangeAllocator cpp/header read as reference: optional free-list locking is
not GPU retirement. Runtime class/option equivalence, index/count provenance
and completion-consumer lock dependencies are not certified by this review.

2026-10-04 factory/VA lifecycle: four whole bodies reviewed/pinned with three
declared VA method identities and five direct edges. Deferred mapping reuse
transfers the saved reference rather than retaining anew; factory has no local
mutex. VA free returns its saved address to task before clearing identity,
without local GuC ACK. Retirement must also exclude premature VA reuse, not
only physical-page reuse. Task allocator policy and effective caller locks
remain unverified; this is not a reproduced stale-translation fault.

2026-10-04 collector ownership: five complete bodies reviewed/pinned (Task
last release, orphan-task cleanup/timer kick, garbage/GART collectors). Both
collector paths demonstrably hold accelerator +0x88 IOLock during cleanup;
Task release alone has no local lock and can defer whole tasks by aggregate
event status. Counter/notification calls are not the mutex. Other caller
exclusion and GuC completion-consumer lock dependencies remain unfinished.

2026-10-04 task mapping drains: nine complete bodies reviewed/pinned across
ordinary orphan cleanup, prune, freeAllGPU, mapping finish/test wrappers,
reverse iterator and Fast2 event tests. Declared +0x190 is event test, not a
page-table manager predicate. Termination +0xdc8 permits outstanding stamps
to pass, so cleanup admission cannot certify hardware completion. Borrowed
predecessor snapshots support serial removal but do not establish concurrent
owner lifetime. Outer exclusion and finish/wait dependencies remain open.

2026-10-04 mapping last-release: complete release const, raw-list add/remove,
and parent orphan-state comparison reviewed/pinned with declared vtable and
four direct edges. Immediate installed-PTE cleanup differs from deferred
flag-8 transfer between owner lists; deferred release does not decrement the
base reference locally. No local lock/GuC ACK; outer owner serialization,
manager predicate and list drains remain unresolved. These fixed-class checks
are not an exhaustive indirect-call inventory or DMA retirement proof.

2026-10-04 allocator scope/prioritization: unnamed 0x3d4230 target's 0x4200
nearest-symbol span is mixed/truncated, not reviewed completely. Reference zone
creation defaults do not certify runtime policy. Standard non-threaded pool
init has no demonstrated false path; park staged factory cleanup and prioritize
the unresolved cross-owner mapping transaction/retirement gate.

2026-10-04 allocation root/helper region pinned with null-propagation edge.
OSObject flags lack NOFAIL, but WAITOK's effective guarantee depends on zone
exhaustibility; selected pool size-class/zone initialization remain unresolved.
No null allocation or OOM reproduced; lower callees incomplete.

2026-10-04 OSObject allocation wrapper: complete Boot operator new pinned;
native binary returns allocator result without local source assert/null check.
Underlying allocator/flags policy still unresolved; not a proven null return.

2026-10-04 pool construction admission: two native and two Boot bodies pinned.
Initial refcount 1/class accounting confirmed; metaclass alloc has no local null
check. Allocator failure policy, runtime metaclass vptr and exclusive factory
hook admission remain open; resource predicate still not integrated.

2026-10-04 empty-state cleanup predicate implemented/tested (39 offline states).
Not integrated into deletion hook: resource emptiness does not prove exclusive
factory ownership/no escape or GPU retirement. Actual admission remains pending.

2026-10-04 base destructor bookkeeping: complete Boot OSObjectD2/instanceDestructed
pinned; no second derived cleanup. Sized delete leads to kfree_ext, whose nearest
span mixes unnamed helpers and remains incompletely reviewed. Do not infer full
deletion/allocator coverage from the existing small wrapper checks.

2026-10-04 pool deletion ABI: complete D2/D1/D0 plus imports/vtable pinned.
Deleting destructor only delegates base destructor/sized delete (0x78), not
owned-resource cleanup. A failed-init base-free bypass needs empty-owned-state
preflight and callee/admission proof; no repair implemented in this checkpoint.

2026-10-04 failed-init lock cleanup: four complete Boot KC bodies/boundaries and
base-init virtual pinned. Spin-lock free cannot accept null; blanket factory
release would break threaded failed-lock initialization. Partial-state cleanup
must precede leak repair; no production/runtime change in this checkpoint.

2026-10-04 initialized-prefix unwind repair: VF-only bounded native patch now
visits N-1..0 on failed construction, never uninitialized suffix. Full static
suite and both ASan/UBSan payload patch checks pass; 4097 index-control models
are not DMA tests. All other pool/mapping/retirement hazards remain unresolved.

2026-10-04 manager pool cleanup: three complete native bodies pinned; device
memory release is distinct from pool release. Failure cleanup needs initialized
prefix tracking; ordinary free lacks local pool release. Indirect teardown and
GPU/table lifetime must be established before adding release.

2026-10-04 standard pool ownership: four complete native bodies pinned.
real_ncpus pools use options 0 (non-threaded), narrowing prior callback hazard.
Partial creation cleanup increments beyond array; withOptions leaks failed-init
object locally. Correct prefix unwind and outer releaseDeviceMemory pending.

2026-10-04 pool event callbacks: five complete native bodies/factory imports
pinned. Timer hands off to software interrupt; interrupt callback can rearm.
Source add results unchecked, no local terminal admission; base +0x218 is
cancelTimeout, not established drain. Concrete workloop/outer owner unresolved.

2026-10-04 pool init/free/hash removal: four complete native bodies pinned.
Init has unchecked allocations and ignored initial growth; free destroys queue
before source teardown; remove can trigger non-atomic rehash. Effective source
types, outer admission/drain and failure-safe replacements remain open.

2026-10-04 hash/heap callers: complete shrinkIfNeeded/percolateDown bodies pinned
and native __text direct edges decoded excluding external placeholders. Inner
heap index writes assume full hash membership; failed rehash can invalidate that
invariant. Indirect callers, pool init and removal lifecycle remain pending.

2026-10-04 queue publication callees: six complete native vector/hash bodies
pinned. False really includes allocation failure; uninserted blocks can be
published/countable but undiscoverable to allocator retries. Missing-key index
returns address 0x8. Resize publishes before checking bucket storage and ignores
copy insertion failure before deleting original nodes; full caller inventory
and failure-atomic replacement remain open.

2026-10-04 PagePool allocation/prune/grow: four complete native bodies pinned.
Allocator reuses published free bits without the prune age filter; prune's
descriptor completion result is ignored. Actual backing types, new-block
allocation factories, hash helpers and outer GPU lifetime remain pending. Grow
has an unchecked map result and non-propagated queue publication failures;
factory guarantees/recovery must be established before a safe implementation.

2026-10-04 PagePool reuse/PPGTT expansion: nine complete native bodies pinned.
Last descriptor release clears backing and publishes a free bit without a local
GPU barrier; hierarchy shrink is not full transactional mapping rollback.
Page allocation/prune, outer GPU lifetime and shared-record counts remain open.

2026-10-04 base/descriptor ownership: eight complete native bodies reviewed and
pinned, including address-mode selector and atomic descriptor reference calls.
1 GiB descriptor granularity and release-before-remap ordering are concrete;
PagePool recycling, expansion failure cleanup, mode writers and outer lifetime
remain open. This is not a GPU invalidation/drain proof.

2026-10-04 synchronization feature provenance: complete common/32/64-bit page
table initializers reviewed/pinned. 64-bit options=1 selects descriptor reuse;
32-bit options=0 selects per-entry sync and ignores dummy-map statuses locally.
Historical VF range crosses fixed reuse window, but actual shared-buffer
requirements/current provisioning and safe dynamic-range reuse remain pending.

2026-10-04 PPGTT synchronization/list release: six complete native wrapper/helper/
read/list-release bodies reviewed/pinned. Per-entry failure silently ends void
sync; descriptor branch uses a fixed 1 GiB range and ignores destination status.
Feature flags/range compatibility, descriptor ownership, table free ordering
and runtime invalidation remain pending; list-node free is not GPU retirement.

2026-10-04 task address-space construction: complete list init (0xfc) and new
per-task page-table factory selector (0xa6) reviewed/pinned with synchronization
edges. PPGTT-enabled kernel tasks can contain private+global tables, disproving
GGTT-only rollback as a complete manager repair. Factory/synchronization bodies,
pointer ownership and multi-space transaction/invalidation remain pending.

2026-10-04 complete native commitRange review: 0x41c body, manager direct call,
global mapping virtual slots and ignored suffix result checked in both payloads.
Later segment failure preserves earlier successful prefix; false commit does
not publish installed flag. Per-owner transaction/whole-range rollback and
multi-address-space cleanup remain required before runtime. Prior post-write
barrier patch is not a complete solution to this separate failure protocol.

2026-10-04 VF post-write completion repair: complete manager commit reviewed/
pinned. Normal/dummy and both rotated post-write branches now reject unconfirmed
TLB completion without returning into native backing cleanup. Four source guard/
release-order mutations are checked. Multi-segment/address-space partial commit,
failed-wire cleanup and Host DMA quiescence remain unresolved. This production
change is offline only and does not authorize runtime/claim acceleration.

2026-10-04 PTE commit/final-free follow-up: complete base commit_pte and Intel
GPU-page-table commit reviewed/pinned with native manager edge. Sys-memory
free re-reviewed: wired-only unwire does not handle unwired failed command.
Retained descriptor may leak, not necessarily become freed backing. Manager
partial-commit behavior, bypass ownership and safe failure-unwind repair remain
pending; no dynamic/panic or DMA-quiescence certification.

2026-10-04 resident-set follow-up: complete wired publication, resident add/remove,
iterator, sort/realloc and parent LRU seed reviewed/pinned. Examined ordinary
wire failure precedes resident publication; recovery cannot be presumed to
dispose that failed target. Raw borrowed pointer/outer locking, alternate
membership, allocation failure helper and seed wrap policy remain pending.

2026-10-04 mapping recovery follow-up: complete freeToPrepareMapping and base
freeWaitToPrepareSysMap reviewed/pinned; resource direct edges and base recovery
virtual identities resolved. Recovery unwires eligible collection entries and
retries target prepare; it is not direct target-command cleanup. Concrete Intel
overrides, collection membership/waits, vid-map callee and failed-target disposal
remain pending. Retry overwrite risk is conditional, not runtime reproduction.

2026-10-04 mapping parent-prepare follow-up: complete map prepare/outlined helper
and parent/map getPrepareCount reviewed/pinned; Intel map import and paired
parent virtual identities resolved. Parent prepare false is propagated without
command cleanup. PTE initializer, resource recovery helper and outer disposal
remain pending. Aggregate accounting counts are not GPU retirement evidence.
Actual sys-memory complete override (0x42) was resolved after a failed fixture
expectation; it can dispatch unwire after balancing base/aggregate counts.

2026-10-04 sys-memory factory follow-up: complete legacy wrapper and bool
overload reviewed/pinned. Prewired pool branch sets wired flag without ordinary
wire/command construction; null command is not a universal broken-state test.
Pool helper ownership/retirement and ordinary failed-prepare disposal remain
pending. Same-offset virtual calls require receiver typing, not byte matching.

2026-10-04 Intel wire/caller follow-up: complete native wire override in both
payloads, base memory prepare and wire-count helpers reviewed/pinned. Explicit
base-table wire dispatch is paired-KC resolved. Native override forwards false
without cleanup; base prepare avoids normal rewiring via wired flag, but leaves
failed-wire command cleanup to outer callers. All failed-prepare disposal,
parent-count helper and per-owner admission remain uncertified.

2026-10-04 sys-memory wire/prepare follow-up: complete base wire and Boot DMA
prepare reviewed/pinned, including bind/prepare/failure-clear/descriptor-complete
anchors. Clean success prepares once; prepare errors after count increment
retain the reference, while wire clear(false) ignores failure. Concrete Intel
override, descriptor class, failed-wire disposal and mapping retirement remain
pending; no observed panic attribution or runtime safety certification.

2026-10-04 DMA return follow-up: returnDMACommand complete body re-reviewed
against base command free/clear; unwire direct caller and selected argument,
error-join, pool-lock/capacity/release anchors pinned. Return accepts command
without state/stop validation; unwire logs clear failure then returns anyway.
Prepare-count >1 is a conditional unsafe case, not yet proven reachable.
Indirect callers, prepare ownership and shutdown admission remain unresolved.

2026-10-04 DMA private allocation/free follow-up: complete Boot external typed
allocator and base IODMACommand free reviewed/pinned. External flags are masked
to 7, dropping NOFAIL; actual zone policy remains unresolved. Command free
detaches descriptor without complete/clear; destruction is not a DMA drain.
Local XNU source supports the intended detach workaround, not proof that all
runtime allocation or optional-lock paths are safe.

2026-10-04 clone initializer follow-up: complete initWithRefCon,
SegmentOptions initWithSpecification and setSpecification reviewed/pinned;
actual allocator, mapper-wait and mutex-factory direct calls resolved.
Effective mapper changes retain/release locally. Allocator flag/failure policy,
private-state free and optional mutex failure handling are not yet certified;
absence of a local allocation-null check alone is not a proven defect.

2026-10-04 DMA template clone follow-up: complete Boot cloneCommand body
reviewed/hash-pinned with initializer/failure-release instruction anchors.
New command configuration is reconstructed from the template, not descriptor
or prepared-state cloning. Null clone is skipped by pool construction; the
original template/lock failure unwind remains separate. Clone initializer
and metaclass internals, per-object VF ownership and shutdown admission still
require review. No production/runtime changes or hardware validation.

2026-10-04 display admission/cleanup follow-up: complete display pipe init,
lazy setup callers, shared/private base workloop construction, setup,
enable/disable event references, event_interrupt_gated, termination signal,
queue finish/idle, live/finished ownership transfer and source teardown bodies
reviewed and fixture-pinned. Paired Boot command gate factory and workloop
factory import identities resolved. Setup is LAZY, not part of pipe init;
gate-null external entrypoints can call setup again after field clearing unless
outer admission prevents it. Init-only validation is insufficient. Unresolved:
per-owner VF identification for a System-kext hook, outer admission/lifetime,
caller ABI/error unwind, failed source attachment/removal and callback drain.
Native unchecked statuses are inventories, not proof of an observed runtime
failure. No global display hook, successful acceleration or completed all-file
audit is claimed.

2026-10-04 periodic/fallback consolidation: complete native periodic enable,
disable, callback and shared cleanup; Intel event init/free/callback/stamp
enable-disable/enableSchedulerEvents; streamer5 init/register/context-switch
enable; SystemKC event Fast2/base free, stop/block, finishAllStamps,
finishStamp, per-event stamp-disable, waitForStamp and timeout reference cleanup
were read and locally pinned. Concrete normal accelerator stop virtual imports
and null-provider inherited-stop bypass are resolved. Earlier pending notes
below are chronological, not proof that these resolved windows are unread.
Unresolved: external event/waiter admission, retirement of all prior interrupt
references, callback-owner drain, source-removal failure handling and safe
integration with the GuC counted gate. DeviceStopping does not close readiness;
the current counted gate covers attach/detach/submit only. No complete global
caller inventory, DMA containment, hardware acceleration or all-file review
is claimed. Abstract models expose counterexamples, not native implementation.

2026-10-04 scheduling retention follow-up: complete Boot wakeAtTime(options)
read/pinned with passive source/workloop reference pairs. Stored owner is not
independently retained here; scheduling/cancel helper semantics remain pending.

2026-10-04 timer detach/disable: complete Boot bodies/effective virtuals read
and locally pinned; null detach disables before pointer clear. Schedule-time
retains, cancel-return semantics and failed-removal owner lifetime remain open.

2026-10-04 stop block API: paired import plus complete wrapper/adapter/base
runAction read/pinned; normal stop block executes synchronously under base gate.
Ignored removal results and owner/cancellation lifetime remain unresolved.

2026-10-04 inherited stop follow-up: complete accelerator stop and stop block
read/pinned; twelve removal calls and workloop release/clear tested locally.
Block-execution import and other callees remain pending; ignored removal
results still prevent an unconditional drain-before-free claim.

2026-10-04 workloop factory body follow-up: complete Boot factory reviewed and
locally pinned with base vtable, init and failed-init release/null result.
Actual construction-site type resolved; whole accelerator lifecycle pending.

2026-10-04 accelerator workloop factory provenance: selected inherited start
call/store and paired Boot import identity pinned. Full start/stop/stop block
and factory-body review remain pending; selected windows are not full review.

2026-10-04 Intel workloop getters: four complete payload bodies and three
effective virtuals reviewed/pinned. Scheduler4 resolves accelerator +0xf0;
Scheduler5/GuC own fields differ. Actual +0xf0 construction/type/lifetime pending.

2026-10-04 consolidated timer-review boundary: the protocol audit now separates
resolved base construction/action delegation from unresolved actual Intel
workloop/owner lifetime, timer retains/races, failed removal handling and true
GPU completion. Chronological pending notes are not silently counted as closed;
generic kernel identity review is not a substitute for those driver gates.

2026-10-04 metaclass initializer follow-up: complete unnamed Boot initializer
window reviewed/pinned, including exact vtable source/gMetaClass destination.
Initializer execution/registration and concrete driver lifecycle remain pending.

2026-10-04 metaclass reference follow-up: actual workloop init RIP load and
declared metaclass allocator virtual resolved/pinned locally. gMetaClass vptr
is zero on disk and initialized at runtime; constructor path remains pending,
not evidence of a null runtime pointer or completed concrete-type review.

2026-10-04 inherited event-source init/setter follow-up: complete Boot bodies
read/pinned with owner store, action dispatch and effective command-gate setter.
Owner is stored without explicit retain in reviewed init; owner lifetime and
actual caller metaclass/concrete workloop remain pending.

2026-10-04 command gate allocator/init follow-up: complete Boot init and
allocator reviewed; allocator separately bounded from unnamed initializer.
Local fixture pins base vtable installation, init virtual and inherited edge.
Caller metaclass resolution and inherited action storage remain pending.

2026-10-04 base workloop initializer follow-up: complete Boot init reviewed;
local fixture pins full body, base init/maintenance virtuals and control-gate
action binding instructions. Metaclass/concrete overrides and preexisting
control gate selection remain pending before unconditional teardown claims.

2026-10-04 maintenance action follow-up: complete Boot/XNU _maintRequest
reviewed; Boot body and detach/next-clear/release sequence pinned locally.
Initializer binding/concrete types and unchecked native removal return remain
open before any unconditional callback-drain claim.

2026-10-04 actual command action follow-up: complete Boot runAction body/base
virtual reviewed/pinned, including gated invocation, disabled sleep and teardown
abort. Removal action, request result handling and concrete overrides remain
pending; base gated invocation alone is not a cleanup success proof.

2026-10-04 command wrapper follow-up: complete Boot runCommand and base virtual
reviewed/pinned; complete reference XNU runAction read. Actual Boot virtual
+0x1c8, stored removal action and concrete control-gate type remain pending.

2026-10-04 timer binding getter: complete Boot getter 0x10 and effective
timer +0x168 virtual pinned. Exploratory +0x120 was callback dispatch, not
getter. Startup binding validation and failed-detach lifetime remain pending.

2026-10-04 scheduler patch admission: explicit unique/fixed-offset preflight
added because generic count=1 only means at-least-one. Mutation/duplicate/
moved/null/truncation rejection tests added; no concurrent patcher guarantee.

2026-10-04 Scheduler4 failure repair: VF-only bounded 0xaf init patch removes
premature free, retains factory release for all failures. Both payloads and
in-memory patch tests verify only six bytes change. _kfree_ext identity resolved;
its extra unnamed helpers are not treated as one reviewed allocator function.
Runtime validation and independent callback/drain safety remain pending.

2026-10-04 base deletion: actual Boot OSObject free wrapper 0x30 and sized
delete wrapper 0x40, plus TGL Scheduler4 D0 0x22 reviewed/pinned. Effective
deleting slot and external base-vtable/delete relocations verified. Allocator
callee, failed-init contract and repair remain pending.

2026-10-04 Scheduler4 init: complete actual 0xaf window reviewed/pinned,
separate from the extra unnamed thunk in nearest-symbol span; effective
partial-init free virtual pinned. Manual-free/factory-release inherited
lifetime semantics remain unresolved, not certified by clearing fields.

2026-10-04 IGGuC construction: full 0x48 factory reviewed/pinned; type-3
dispatcher/factory/init/base-init edges pinned. Full init and indirect
construction remain pending. Normal type-3 dispatch excluded by VF guard;
this is not a proof excluding all possible IGGuC allocation.

2026-10-04 IGGuC producers: complete H2G 0x122 and doorbell 0x12a bodies
reviewed/pinned with kick edges and H2G raw MMIO stores. Caller/allocator/
virtual reachability and VF isolation remain unreviewed; no containment claim.

2026-10-04 DPSM producer follow-up: complete Scheduler5 kick callback 0xe
and direct owner-to-kick edge reviewed/pinned; other producer candidates are
triage only, not whole-body review. Service-stop cancellation/quiesce ordering
revalidated; rearm exclusion remains unfinished.

2026-10-04 DPSM cancel restoration: VF-only engine-stop replacement now
null-checks/cancels timer1460 before final shutdown, with no false idle write
or PF wait. Callback/rearm exclusion and runtime safety remain unproven.

2026-10-04 DPSM provenance: complete idle/kick/isIdle bodies and separate
constructor subsection reviewed/pinned; default factory import verified.
1458 bit 0 is reported idle, not a neutral stopping flag. Software cancel
restoration/producer exclusion and stale-state lifetime remain pending.

2026-10-04 free/engine-stop: complete free wrapper 0x24, separate helper 0xce
and native engine stop 0xc4 reviewed/pinned; timer1460 cancellation/state1458
store and effective scheduler wait virtual pinned. VF omits these steps;
timer/field provenance and safe software-only restoration remain pending.

2026-10-04 complete native stop: full 0x3e4 body/branches reviewed/pinned,
including type-5-only scheduler release, source-dependent unchecked removal
and provider-gated inherited stop. stop(nullptr) does not reach inherited
stop/block; constructor ordering and later free lifetime remain pending.

2026-10-04 factory-null failure subsection: native 0xb3 window reviewed/pinned,
busy/mutex unlock imports and effective stop(nullptr) target established.
This is control-flow evidence, not partial-construction lifetime safety.

2026-10-04 final factory admission: VF-only typed scheduler create route
checks actual native bits before delegating; non-4 faults/returns null. Two
native caller result stores/null branches pinned; 95 routes. This does not
complete failure-unwind, callback lifetime or DMA containment review.

2026-10-04 options admission repair: VF pre-start retained-copy type check
rejects OSData scheduler override, releases both references and never mutates
global options. Source order/scope contracts pass; later registry mutation and
runtime behavior remain unproven.

2026-10-04 numeric property helper: complete 0x18c body reviewed/pinned,
including late IODeviceTree:/options override and imported number parser.
Property publication alone does not guarantee final scheduler selection;
late-override admission, getter/parser internals and runtime remain pending.

2026-10-04 firmware override follow-up: reviewed/pinned native start subsection
0x27a68..0x27b18 showing firmware-disable boot argument overwrites type with 5.
VF source now rejects it before bootstrap/native start; PF unchanged. Property
helper/full start and runtime verification remain incomplete.

2026-10-04 selection follow-up: complete Scheduler4/5 factory bodies and
separate native create dispatcher window reviewed/pinned. VF wrapper source
forces type 4; property-to-feature-bit propagation/runtime selection remains
unverified. Scheduler5 concerns are not attributed to the current VF crash.

2026-10-04 Scheduler5 init follow-up: complete 0x17c body, base-init edge,
private-workloop store, failed-init effective free virtual and three factory/
allocation imports reviewed/pinned. No base timer rebinding in this body;
engine/helper/upper-level ownership and selected scheduler remain pending.

2026-10-04 derived scheduler free follow-up: full Scheduler4/Scheduler5 free
bodies, base vtable free delegation and Scheduler5 private-workloop clear
before inherited cleanup reviewed/pinned. Earlier getter evidence implies
null getter at base cleanup on this path; prior stop/detach topology remains
unreviewed. No runtime race or ownership safety has been proven.

2026-10-04 scheduler cleanup follow-up: native cleanup/free/periodic callback
re-read; both payload variants pin unchecked removal followed by timer release,
set clear and mutex-free call. Successful same-workloop gate serialization is
conditional, not a failed-detach lifetime guarantee; no runtime repair yet.

2026-10-04 cancel-wait/owner follow-up: complete Boot cancel-wait 0x3d0
body and separate callback-invocation helper 0x160 window reviewed and pinned.
Conditional wait/fixed snapshot and direct owner forwarding are established;
external wait helpers, admission exclusion and owner lifetime are not.

2026-10-04 cancellation boundary follow-up: separately reviewed the complete
Boot public cancel wrapper (0x120) and unnamed locked helper window (0x1f0).
Local fixture pins both hashes and their call edge. Boolean cancellation is
not callback/DMA drain proof; external helper callees and cancel-wait remain
unreviewed. This does not complete the all-source audit.

2026-10-04 workloop gate follow-up: complete Boot base closeGate/openGate and
removeEventSource reviewed and locally pinned with effective virtuals and
mutex edges. Recursive ownership distinguished from callback-drain proof;
control-gate command/removal action and concrete overrides remain pending.

2026-10-04 passive timer callback follow-up: complete Boot timeoutAndRelease
reviewed and locally pinned with generation/gate/release instructions. Action
helper, schedule-time retains, gate implementations and removal drain remain
pending; generation invalidation alone does not certify teardown quiescence.

2026-10-04 timer init delegation follow-up: complete Boot owner/action init
body read, local fixture pins virtual +0x1c0, setup virtual +0x1b8 and inherited
event-source init call. Options-to-setup middle delegation resolved; inherited
init internals, callbacks and teardown synchronization remain pending.

2026-10-04 actual timer factory follow-up: complete Boot default factory,
options-init and setup code with five-entry data table reviewed/pinned locally.
Options 1/passive setup distinguished from workloop-priority active mode;
intermediate init delegation and callback/workloop draining remain pending.

2026-10-04 timer cancel follow-up: complete Tahoe base cancelTimeout body read;
local paired fixture pins virtual +0x218, body, mode selector and both cancel/
cancel-wait call targets. Actual timer init mode and thread-call draining remain
pending; no unconditional callback or hardware quiescence claim.

2026-10-04 scheduler timer construction/cleanup follow-up: complete native init,
unnamed shared cleanup and free reviewed as separate disassembled functions,
not merged nearest-symbol ranges. Factories, failure-to-cleanup/free edges and
exact windows pinned in both payloads. Factory kernel implementation, timer
cancel/drain semantics and workloop removal synchronization remain pending.

2026-10-04 Boot KC event-source follow-up: complete base normalInterruptOccurred
and setTimeoutUS bodies read and pinned with their two vtable targets in the
paired local fixture. Pending notification is distinguished from action
execution, and the base timer's microsecond API is identified. Actual driver
source types, workloop action dispatch, teardown drain and deadline handling
remain incomplete. No remote KC test or runtime safety is claimed.

2026-10-04 stamp-source follow-up: complete Intel event-machine init/free,
scheduler stamp callback and signalStampUpdate bodies reviewed and pinned;
factory/notification/time/debug imports plus callback-to-notification and
task-stamp direct edges tested. Workloop attachment, virtual interrupt dispatch
and drain-before-free remain open; observed CPU stamp changes do not prove
hardware completion or satisfy the runtime gate.

2026-10-04 periodic timer follow-up: read the complete external reference
`xnu-12377.121.6/libkern/c++/OSCollectionIterator.cpp`, including both storage
models, allocation failure, update-stamp invalidation and unretained iteration
results. Pin 13 concrete TGL factory/mutex/spin-lock call imports in both
payloads. Boot KC implementation identity and callback lifetime remain pending;
this does not complete SDK/dependency review or establish hardware safety.

At c312229, the tracked source/build/metadata inventory contained 1,305 files:

| Area | Files | Current review boundary |
| --- | ---: | --- |
| NootedGreen | 31 | Mixed; details below, not all findings closed |
| tools | 24 | Historical snapshot count. All current executable host tools are now syntax-gated; the Linux MMIO mapper pipeline was read end-to-end and its policy/round-trip behavior is tested. The stale AppleIntelParams generator was removed. |
| MacKernelSDK | 1,163 | Selected API declarations only; full review pending |
| Lilu.kext | 39 | Selected headers/upstream patching code; full dependency review pending |
| HookCase-master (removed) | 0 | All seven files were reviewed. The active service did not install its historical IDT/sysent/DYLD hook core, while its appended graphics providers used reversed/underflowing MMIO bounds, a guaranteed-null port callback, guessed object storage, unverified `| 7` GGTT PTE flags and wrong zero-argument GPU telemetry ABIs. The exact accelerator and pinned-TGL framebuffer imports are now provided by NootedGreen; the unrelated hooking project was removed and remains recoverable in Git. |
| sle_Internal | 35 | Metadata read end-to-end. Both patched TGL kernel payloads now require the boot root set, depend on NootedGreen rather than HookCase, and are contract-tested against their exact imports. Embedded binary semantics remain a separate per-route review obligation. |
| NootedGreen.xcodeproj | 4 | Read end-to-end. Product/source/configuration, scheme and workspace contracts are now tested; obsolete shell phases were removed. |
| .github | 1 | Read end-to-end. The branch/path filters and x86_64 NootedGreen/Metal artifact stages are enforced by the project contract test; HookCase build/packaging was removed and no deployment step exists. |

The earlier 1,256-file count omitted assembly and some build metadata. The
expanded scope above includes `.s`, `.S`, `.inc`, `.tool`, `.plist`, scheme and
workspace metadata. Subsequent deletion of the two retired logger sources
reduces this snapshot to 1,303. These are inventory counts, not completion
percentages. Binary libraries, kext executables, firmware instructions and
external reference trees are not magically reviewed by counting source files.
The subsequent workqueue-unwind helper and its test add two reviewed source
files (1,305 under this snapshot's scope). Removing the two obsolete Genx
sources returns the current count to 1,303; this does not close older coverage.

## Main project coverage

2026-10-04 producer review delta: symbol-bounded FIFO stamp/command/buffer,
accelerator sync/main-ring, ring submit and Scheduler4 push bodies were read.
Six concrete ring vtables share the submit virtual; five typed producer edges,
sync write/stamp ordering and three inherited FIFO event/stamp imports are now
contract-pinned for both payloads. False scheduler submission reaches a verified
native `_panic`, not a normal retry path. Caller locking, inherited method
implementation and complete virtual reachability remain open. This is a scoped
binary-graph review, not certification of all sources or runtime correctness.

Inherited producer follow-up: archived IOAccel channel wrapper bodies, Fast2
increment/write/set/merge stamp methods and accelerator scrubEvents were read
symbol-bounded. Local KC tests pin the wrappers, four effective event virtuals,
increment/write/scrub complete bodies and set/merge body hashes. Both Intel
payloads pin the inherited rollover scrub import. Stamp allocation updates
software state, not GPU completion. Resource/shared scrub callbacks, event
wait callbacks, iterator lifetime and producer serialization remain unfinished;
the KC test is local-only and does not run against KC content in remote CI.

Scrub follow-up reviewed complete Shared2/Resource2/memory/Fast2 scrub bodies
and four list iterator methods. Local contracts pin their identities, base
virtual targets and the termination-counter bypass that clears outstanding
event metadata without proving GPU completion. Iterator methods neither lock
nor retain nodes. Concrete resource subclass overrides, upstream list locking,
storage-resource and mapping lifetime remain open; no runtime safety or idle
certification follows from the newly pinned local fixture.

Producer locking delta reviewed busy counter/notification/ownership-query
bodies and the local accelerator lock/unlock copies. The local fixture pins
five unique helper bodies, busy atomics and notification/mutex import identities
across the paired KCs. Busy is counting, notifications are debug tracing, and
the ownership query always returns true; none proves current-thread ownership.
Actual mutex uses accelerator `+0x88`; four lock and five unlock copies now
have exact symbol inventory/body-hash contracts. Complete producer caller lock
coverage remains open, including candidate unlock/relock windows. No locking was added at
runtime and the local-only fixture is not a hardware or CI-KC certification.

Context2 getDataBuffer was subsequently read in full (`0x9e4` bytes). A local
identity contract and explicit helper/inlined unlock-lock, stack-event wait
and post-wait list reload contracts cover both wait windows. Fast2 event init,
copy and finishEventUnlocked effective virtuals are identified, not certified
as fully reviewed implementations. Caller/slot lifetime, mapping callbacks
and producer locking across those windows remain open; direct helper-call
inventory alone is demonstrably insufficient for lock-coverage review.

Unlocked finish/error-request delta: complete Fast2 finishEventUnlocked and
base signalHardwareError bodies were read and hash-pinned locally, including
the termination bypass, wait virtual and timeout request. The caller retries
without its own finite bound and returns elapsed-time accounting, not an idle
boolean. Error signaling uses a distinct event-machine mutex and software
request/event-source notification. Full wait locking, callback processing,
mapping/event lifetime and actual DMA completion remain open.

Full wait delta: base waitForStamp and timeout waiter cleanup were read fully
and locally pinned, with BootKC sleep/deadline imports resolved. Waiter mutex
is released during sleep; termination and a separate polling flag remain
non-hardware completion/boundedness concerns. The base stamp-interrupt no-ops
were read, but Intel overrides them. Their overload/virtual/helper graph is
not fully reviewed and is now an explicit next gate. Base-method verification
must not be substituted for the concrete Intel hardware-safety review.

Concrete Intel stamp-IRQ delta read 16 complete native bodies across event
overloads, Scheduler4, CommandStreamer4, singular bridge descriptor accounting,
periodic timer methods and type-index lookup (including its seven-entry jump
table). Both payloads pin body boundaries/hashes, concrete virtual targets and
typed direct edges. Singular traits methods have no direct MMIO in their bodies;
no speculative no-op was introduced. Timer/imported API semantics, source
construction, callback virtual and lifetime remain unfinished, so this scoped
review is not complete interrupt or hardware-safety certification.

V267 closes the verified VF event-timeout return into inherited restart/retry
by protocol-fault admission closure followed by guest fail-stop. This is not
normal recovery or proof that already-published DMA stops. PF and normal debug
paths are unchanged; the runtime hold, complete review and true completion/
reclamation obligations remain. The explicit route inventory increases to 94.

2026-10-04 mapping provenance follow-up: both pinned payloads now have
relocation contracts for the inherited GPU-address getter, system-memory
factory and mapping-preparation fallback, plus mapping admission/publication
anchors. These checks do not resolve the inherited IOAccel implementation or
prove that packet flags select the mapping's actual address space. No runtime
or mapping-lifetime certification is added.
The next delta pins the native `PPGTT` property/default and feature-bit
assignment, plus the mapped-buffer mapping-options constant. The native map
commit was traced to the task page-table list, but inherited allocation and
option semantics remain open rather than inferred from the constant 7.
Mapping teardown delta: explicit task stamp/scratch cleanup drops references;
mapped-buffer free invokes inherited finishEvent/complete before mapping
release. Shared-buffer explicit CPU unlock clears its CPU mapping independently
of object retention. Relocations and teardown anchors are now pinned, but
virtual/inherited unlock reachability is not certified and no hardware-idle
claim follows from a method named complete.
The archived Tahoe 25G229 SystemKC was subsequently located and its embedded
IOAccel symbols and selected mapping implementations read directly. A separate
hash-pinned local fixture test covers the getter's flag/field branch, the
counter-only complete method, and finishEvent's event-machine dispatch. The
inherited implementation is available for further analysis; its full review
and mapping/event lifetime proof remain incomplete. The KC is not in CI.
Termination follow-up resolves accelerator `+0xdc8` as a counter incremented
by deviceTerminatedUnlocked through OSIncrementAtomic. The local fixture now
optionally pins the paired BootKC, resolving the encoded import using its
__HIB base and confirming locked xadd semantics. Termination enables a wait
success branch without hardware-stamp evidence; complete callback/reset and
quiescence guarantees remain unresolved rather than inferred from this result.

2026-10-04 ownership review delta: V263 fail-stops uncertain void descriptor
detach, and V264 independently retains the actual DMA ring buffer in each
direct GuC record through acknowledged deregistration and final reference
release. The context ring/FIFO objects are released before descriptor cleanup;
the buffer at context `+0xa8` is not the ring. Partially registered contexts
whose compensating deregistration fails now fail-stop rather than return into
the unchecked native initializer. Task stamp getter/allocation/clone paths and
both task-retain paths are pinned. Ring notification only merges event
dependencies; it is not hardware completion. Checked garbage collection reaches
the routed descriptor-idle query, but forced collection and drain bypass idle
checks. Enabled VF contexts still remain conservatively busy, so real completion
and normal reclamation are unresolved functional requirements. These updates
supersede earlier image-only-quarantine claims without certifying the full
ownership graph, all native callers, inherited IOAccel implementation, or runtime
DMA safety as reviewed or complete.
V265 additionally retains task stamp/scratch buffers as direct GuC record
dependencies until the same deregistration/final-reference boundary. The buffer
getter provenance, record identity, retain/release ordering and tombstone reuse
are checked; this is not a completed review of every GPU-referenced resource.
V266 rejects negative/out-of-bounds stamp slots and undersized scratch backing
before registration. The overflow-safe numeric helper is tested against widened
endpoint arithmetic; address-space flags and actual GPU execution remain unproven.

Current Gen11 delta: the route inventory is 93 unique symbols (90 accelerator,
three framebuffer), superseding the earlier 65/62, 63/60 and 60/57 historical counts
retained below. Complete nested-path disassembly showed that bridge
enable/disable also dispatched into scheduler-4 physical command-streamer
error-IRQ MMIO. VF-only scheduler routes now replace only those two physical
error-IRQ helpers with no-ops. TGL/ADL/RPL retain Tahoe's native Gen11
virtual-MMIO interrupt bridge and logical scheduler callbacks; only MTL/ARL
select the separately capability-gated memory-IRQ transport. The two native
IOAccel completion events remain initialized in their original order. The
watchdog-contained `102ec33` runtime then reached the
headless local-filter path and proved that the legacy TGL driver had no
published PCI interrupt source even though the VF exposes a valid one-vector
MSI capability. The wrapper now requests that real MSI through Tahoe's exported
`IOPCIDevice::configureInterrupts` ABI before native start. A pre-CTB failure
rollback can publish DMA quiescence only after the drained, stable context table
is proven completely unowned. Both runtime checkpoints and the expanded
lifecycle contract are recorded in `TAHOE_SRIOV_PROTOCOL_AUDIT.md`.
The first contained MSI runtime then proved that the VF wrapper reached its
own impossible precondition: it demanded GuC/CTB readiness before invoking the
native scheduler firmware boundary that creates GuC/CTB. Complete native-tail
disassembly now requires `IGScheduler::initFirmware()` before transport
readiness and accelerator enable, while continuing to omit
`IGMemoryManager::initCache()` because that method directly writes PF-owned
force-wake, MOCS and L3 registers.
The next contained runtime reached successful CTB and memory-IRQ setup, then
failed in the first post-CTB proxy-context GGTT mapping because its synchronous
`TLB_DONE` wait preceded interrupt-bridge admission. Pinned Tahoe disassembly
and current i915 ordering now require the already VF-contained bridge before
scheduler firmware initialization, followed by an explicit CT-enable boundary
drain. Failed partial GuC construction can unwind only through the separately
acknowledged MMIO CTB-disable boundary, and only while scheduler firmware is
not ready and the drained complete direct-context table is proven unowned.
The correctly installed `8bf2f49` runtime proved that early bridge enable alone
does not deliver completions: GuC `initInterrupts()` registers its enable
callback only after the bridge's one-shot `enable()` list walk. Pinned
disassembly shows the late request is merely appended and can never run while
the bridge remains enabled. The VF route now invokes only post-enable callbacks
immediately, preserving the native queued path before enable. Every synchronous
FAST waiter also performs a bounded, lock-serialized G2H drain so a lost MSI
edge cannot strand TLB/context completion. The same failure exposed a native
Tahoe GuC factory use-after-free: `initWithOptions()` calls virtual `free()` and
the factory then virtually releases the deleted object. A UUID/function-bounded
instruction patch removes only that second dispatch while retaining cleanup
and the null return. These repairs and their binary/source contracts passed the
full static suite. The later `ce166c8` runtime exposed an independent
host-critical error: RPL-P was incorrectly forced onto the MTL/ARL memory-IRQ
ABI. Commit `8c45437` selects transport by the i915 hardware capability, and
its CI artifact is archival only. That run's PF DMAR faults and i915 hang make
every subsequent dynamic load prohibited until the remaining host-dangerous
MMIO/DMA reachability review and a separate host containment plan are complete.
The next offline checkpoint also closes Tahoe's unique post-engine `0x215`
start-failure edge: if GuC firmware is already live but native cleanup did not
start, the wrapper must enter the captured accelerator stop route and prove both
the device-stopping and DMA-quiesced boundaries before marking the failure.
Its Mach-O contract now fixes the retained scheduler-4/command-streamer/GuC/CTB
bootstrap call graph and requires every hardware-facing descendant to remain in
the VF route set. This is not runtime validation; `macos-tahoe-sriov` remains
off under `docs/HOST_CONTAINMENT_PLAN.md`.
The retained native-start success tail is now covered as well. Tahoe's
feature-bit-gated `setAsyncSliceCount` performs a raw force-wake-protected
`MMIO+0xA204` write, so a VF rejects that legacy page-ownership mode before
GGTT/MSI/native start. HWS mapped-buffer descendants, the sole engine-start
edge, DPSM timer placement, scheduler idle vtable slots and the exact no-op
DPSM/coarse-power local callbacks are pinned for both payloads.
The complete original engine-start body is now an explicit physical-only
boundary: its force-wake/mode/HWS-register edges and raw HWS MMIO stores are
machine-checked while the VF must replace the whole symbol without an original
trampoline. All eleven initialized headless local-callback slots are pinned to
their exact software-only bodies. The guest power-state graph must continue to
reach the routed engine/bridge boundaries, and scheduler-4's loaded-byte guard
must make repeated wake firmware initialization idempotent.
Tahoe also creates its telemetry manager and 64 per-stamp usage objects before
engine start. `TelemetryDisable` only gates trace-stream and context-image work;
native manager bootstrap, IOReport, sysctl, user-client OA operations and usage
reporting still reach force wake and PF-owned MMIO. Seventeen additional
VF-only routes now retain Apple's object/lifetime topology while rejecting or
neutralizing every hardware-facing entry. Manager initialization publishes only
the exact software ownership fields, per-stamp allocation remains in the native
supported zero-allocation state, and the pinned native destructors are proven to
take their null/zero fast paths. Both source personalities remain unchanged for
PF use; only the separately cloned injected VF `Development` dictionary forces
`TelemetryDisable=1`.
Tahoe's aperture-resource path also reaches the legacy `IGFenceAllocator`
outside the engine-start boundary. Both `IGFence::initWithOptions` and
`IGFence::free` take physical force-wake and write the `0x100000` fence-register
bank through the accelerator's raw MMIO base. A VF-only route now rejects the
allocator before it consumes a slot or constructs an object. The exact two
native callers and their null-result cleanup, both hardware bodies and all four
force-wake edges are pinned in both payloads; a VF never fabricates fence
success, while a PF keeps the native allocator.
The retained pre-engine memory-manager path is now explicit as well.
`IntelAccelerator` vtable slot `0xaf0` constructs the TGL manager, whose base
initializer clears its two eDRAM capability bytes and unconditionally invokes
virtual slot `0x148`. Native `IntelTGLMemoryManager::detectEDRAM` raw-reads
`0x120010` and can conditionally program `0x138124/0x138128` before confirming
at `0x145910`. A VF-only replacement preserves the software false/false
capability state and performs no force-wake or MMIO; PF detection is unchanged.
The post-engine debug sysctl surface is now excluded as a paired lifecycle on a
VF. Native start otherwise registers 55 OIDs before its `TelemetryDisable`
check, exposes 52 writable idvar leaves through one broad dispatcher, publishes
the accelerator globally and can reach private OA/trace controls. The matching
native stop unregisters the same 55 OIDs. Both functions are exact-ABI no-ops
only for a classified VF; PF debug behavior is unchanged, and binary contracts
pin the complete registration table, lifecycle edges and MMIO descendants.
The stamp-timeout and externally callable hardware-diagnosis paths are now
explicit. Scheduler4's timeout virtual slots and exact software-only bodies are
pinned, while the native non-virtual helpers are proven to write physical
RING_MI_MODE and the global INSTDONE selector or to traverse the full raw ring
register dump. Five VF-only routes preserve Tahoe's software event recovery but
replace halt/resume, debug capture, hang analysis and hang dumping with
exact-ABI inert results; PF timeout diagnosis remains native.
The FIFO reset/replay graph is separately contained. Its root virtual can
dispatch the ring-buffer physical reset, resume the scheduler and replay two
stamps; the reset primitive itself takes five force-wake paths and executes
engine-control, reset/fault-clear and reset-list register writes. A VF now
quarantines the root request without replay and the lower reset returns false;
both vtable slots and all destructive descendants are pinned while PF recovery
remains native.

| Files/area | Evidence and remaining boundary |
| --- | --- |
| kern_gen11.cpp/.hpp | Partial, protocol-focused review plus targeted native disassembly; Mach-O relocation audit removed 39 unreachable private wrappers/stubs and their original slots. Active empty VDD/shutdown replacements, four unversioned private-offset sleep/wake mutations and the PAVP command-4 false-success hook were removed so native physical lifecycle and DRM handling remain authoritative. VF native blit/barrier producers now require the complete transport admission state and preserve owned task/context objects; the pinned payload proves `barrierSubmission` is void, so its former unobservable `return 0` rejection is now exact-ABI fail-stop rather than silently continuing without event/FIFO side effects. Old V120/V130/V142 fabricated/bypass protocols were removed. All Tahoe TGL private routes/byte patches require the pinned UUID. The Mach-O symbol-table contract now inventories all 93 unique installed routes (90 accelerator, three framebuffer) and requires unique symbols in the applicable UUID-pinned variants. It caught that the production framebuffer overrides `probe` in `AppleIntelFramebufferController` while the debug payload exposes `AppleIntelBaseController::probe`; VF rejection now selects the exact UUID-specific entry rather than requiring an absent production symbol. The same contract now maps exact byte anchors through Mach-O segments to their owning symbols after the first AuxKC start-only load exposed that the SKU compare was incorrectly searched inside `getGPUInfo`: it is bounded independently to `IntelAccelerator::probe`, while only the six PF-relayed fuse anchors remain in `getGPUInfo`. GGTT, GuC/CTB, IRQ, direct-LRCA, force-wake, legacy-ring, telemetry/OA/debug-sysctl, timeout/hang-diagnosis and reset/replay containment routes now install only on a classified VF; PF keeps native ownership. Direct GGTT mutations now use an explicit system-memory PTE encoder, aligned 64-bit stores and synchronous post-CTB GuC invalidation rather than Apple's split physical encoding. The two void invalidation routes now fail-stop if their required post-CTB heavy invalidation cannot complete, and the void legacy ownership route fails before a caller can proceed under a fabricated ownership transition. VF descriptor attach/detach no longer enters Tahoe's unused legacy proxy-slot/LRCA-hash bodies; routed direct submit, idle and lifecycle operations use one context table, removing silent hash-allocation failure and two live physical-TLB patch sites. Every discoverable detach backing is quarantined when bookkeeping or the H2G queue is unavailable, covering early void-return paths as well as malformed identity. Proxy DMA discovery now resolves the UUID-admitted two-argument `IGAccelSysMemory::getPhysicalSegment` symbol after verifying the private object metaclass; its implementation supplies the same MemoryManager IOMapper options used by native GGTT commit. The earlier public three-argument `IOMemoryDescriptor` substitution was a type/ABI error and has been removed. Local scheduler-allocation rejection now stops new producers while retaining teardown/TLB retirement transport, and pre-CTB faults may unwind mappings because no GPU translation could have consumed them. The accelerator start wrapper exactly returns `bool`, preventing native AL=false from being misread through the former 64-bit RAX signature and published as success; binary call sites and native epilogues establish the same boolean ABI for the three VF firmware/engine replacements, and multithreaded force-wake now preserves its complete 32-bit context argument. A controlled root spindump found that Metal resource creation blocked in `acceleratorWaitEnabled()`: the VF `startGraphicsEngine` replacement had skipped the native interrupt-bridge and IOAccelerator enable tail. It now preserves those named enable/disable lifecycles after GuC admission and final DMA quiescence; a Mach-O relocation test proves both pinned payloads' original order. Unreachable PF fallbacks and original slots plus the physical topology wrapper/force-wake port were removed. The two accelerator payloads have identical executable content and differ only in bundle identifiers. Target-specific topology constants are removed: a one-shot, versioned PF MMIO relay supplies the five VF-invisible values consumed by `getGPUInfo`, with pure protocol/fuse tests and exact unique binary anchors. The broad SKU bypass is now a one-immediate `0x9a40`→`0x9a49` comparison change that preserves every native failure branch. The private header was reduced from 1,083 lines to the live address-range type, exact six-value engine enum and active route class; unused ring/reset/display/connector tables, structs and undefined methods were removed. Personality publication now clones the exact catalogue entry for the admitted bundle and changes only PCI matching; display, development, debug and HEVC media dictionaries can no longer disappear through sparse reconstruction. The late GuC interrupt-registration callback is now serviced across the early-enable boundary, synchronous completion waits poll G2H in bounded slices, and the native GuC factory's failed-init double destruction is removed only inside its pinned function boundary. IRQ transport is selected by exact i915 capability: TGL/ADL/RPL retain the allowlisted Gen11 virtual-MMIO bridge, while only MTL/ARL install memory-IRQ routes and LRCA mutations. Tahoe's pre-engine telemetry, IOReport, usage, user-client OA, post-engine debug-sysctl, timeout/hang-diagnosis and reset/replay graphs are now pinned end-to-end; VF wrappers keep native software object topology but never enter PF force-wake/MMIO, OA allocation, register restore, broad idvar publication, physical timeout dumps or engine reset/replay, while PF behavior remains native. Other reachable accelerator routes still require semantic review |
| kern_green.cpp/.hpp | Read end-to-end; PCI identity/overread, mandatory config-hook admission, BAR0 publication and dead-path cleanup completed. A controlled native probe exposed that the original spoof helper treated `extendedConfigRead32(2)` as a sliding byte window; Apple IOPCIFamily instead carries the raw low offset to the bridge, where DWORD address bits 1:0 are ignored. Both word and DWORD hooks now model natural alignment and reject nonzero extended-register pages, with the exact `0xa7a89a49` panic fixture in the offline test. The selected `IOPCIDevice` has explicit plugin-lifetime ownership before the borrowed `DeviceInfo` inventory is deleted, covering later lazy BAR mapping and routed config callbacks. The unused GMS/stolen-size decoder, CPUID diagnostic, IOAcceleratorFamily2 log-only callback, zero-caller BAR2 mapper, WA/whitelist MMIO helpers, guessed property seeder, legacy flags and commented service skeleton were removed. PF register lifecycle remains native and VF GGTT uses its separately validated BAR0 PTE transport. Physical device hot-removal is outside the integrated/passed-through GPU model; sleep/wake MMIO validity remains a controlled-runtime obligation. |
| kern_telemetry.cpp/.hpp | Read end-to-end. The pinned accelerator's only two HookCase imports now have exact XNU prototypes and are forwarded to the running kernel symbols resolved strictly under `KernelPatcher::KernelID`; an offline Mach-O/import/source contract and macOS link build verify the provider boundary. |
| kern_tgl_compat.cpp/.hpp | Read end-to-end against the deleted ICL reference binary, both pinned Tahoe TGL framebuffer variants and current i915. Admission requires a native TGL PF plus pinned framebuffer UUID. 32-bit register access uses NootedGreen's checked BAR mapping; every missing 64-bit caller was proven to use the controller's `+0xCA0` GGTT mapping and is bounded by `+0xCEC`. DSB allocation validates the exact `0x12000` layout, one contiguous physical segment, the complete GGTT range and every PTE readback, with rollback on failure and no guessed Apple descriptor field. PTEs use address+Present without HookCase's LM/VFID bits; the former nine speculative chicken-register writes were removed. FIA port mode uses the proven `PORT_TX_DFLEXDPSP(FIA1)` register and null-checks the pinned `+0x548` configuration pointer. Pure layout/PTE/port tests plus Mach-O relocation/provider tests pass; physical TGL display remains a controlled hardware-validation obligation. |
| kern_genx.cpp/.hpp and kern_gen11 ICL fallback (removed) | Read end-to-end. Both paths used unversioned private control-flow/ID rewrites; the latter also forced a Sonoma SKU gate, fixed 64-EU topology and firmware hook without a Tahoe payload to verify. Native Ice Lake is no longer intercepted; the maintained compatibility path is the UUID-pinned TGL payload. |
| kern_patcherplus.cpp/.hpp | Read and compared line-by-line with the pinned Lilu 1.7.2 routing/replacement implementation. Removed unused symbol-to-pattern fallback wrappers. Every route group now calls Lilu's native batch API, which resolves the complete group before the first trampoline write; every grouped lookup preflights bounds/signatures/counts before its first write. The second AuxKC load exposed a C++ overload trap: a mutable replacement array made an explicit `(size, count)` call select the array `(count, skip)` template. Array overloads no longer accept skip, their arity is distinct from raw-pointer forms, and the source contract rejects the ambiguous shape. A post-write allocator/protection failure still reaches a mandatory panic rather than rollback; Lilu exposes no per-group rollback API. |
| DYLDPatches.cpp/.hpp (removed) | Read end-to-end. After the earlier control-flow and bundle-redirect cleanup, the only active behavior was a default global `_cs_validate_page` route that rewrote shared-cache board/model strings to a Mac Pro identity and logged bundle discovery. It had no Tahoe cache identity admission and no VF transport role. Removed with the equally unversioned AGDP board-id mutation now that the complete native TGL display/media personality is preserved. |
| DisplayMergeNub.cpp/.h (removed) | Read end-to-end. Its only personality required decimal `DisplayVendorID=17119999` (`0x01053aff`), outside the 16-bit EDID manufacturer-ID space, and only renamed a matching `AppleDisplay` to `AppleBacklightDisplay`. It had no GPU/VF/headless transport role or other consumer. The source, impossible personality and Xcode entries were removed. |
| IntelDPLinkTraining.cpp/.hpp (removed) | Read end-to-end. The standalone MMIO writer had no product caller (only its offline table test), explicitly lacked DKL PHY C--F support and was not connected to an admitted framebuffer ABI. Physical link training remains owned by Apple's native UUID-pinned framebuffer path; a VF rejects that framebuffer entirely. Removed the dead implementation, test and Xcode references; recoverable in Git. |
| kern_start.cpp | Read end-to-end; only the Lilu plugin configuration and init callback remain. The disabled custom IOService implementation and catalogue publication skeleton were removed. Lifecycle integration still depends on the unfinished driver. |
| Firmware.cpp, FirmwareADLP.cpp (removed) | Payload bytes were compared to pinned upstream containers before removal. Their only consumer was the retired manual DMC/MMIO loader; native physical drivers now own DMC lifecycle, and a VF owns none of it |
| kern_gpu_capabilities.hpp, kern_guc_ring.hpp, kern_ggtt_bounds.hpp, kern_ggtt_rotation.hpp, kern_vf_ggtt_pte.hpp, kern_pattern_match.hpp, kern_context_pool.hpp, kern_binary_identity.hpp, kern_pci_identity.hpp | Implementations read and offline-tested; GPU/PF identity gates native TGL branches without CPUID, all GGTT unmap/CTB lifecycle states are checked, all accepted Apple GGTT attribute masks produce address+present without LM/VFID, and 4,096 rotation matrices are verified as bounded permutations. Tests cover pure helpers, not all caller lifetime/hardware contracts. The otherwise unconsumed Gen9 GMS decoder and oracle test were removed with their dead production caller. |
| kern_vf_irq_gate.hpp, kern_vf_context_shutdown.hpp, kern_vf_submission_gate.hpp | Read and exhaustively checked as pure state machines. The callback/operation admission gates, all nine direct-LRCA shutdown classifications, all eight failed-registration cleanup combinations, all 108 state/pending/runnable MODE_DONE combinations and all 512 native producer/consumer admission combinations are covered. MODE_DONE requires the firmware runnable payload and lifecycle state to match the oldest pending enable/disable token; impossible flag/state pairs fail closed. DEREGISTER_DONE preserves descriptor, engine and backing identity until every native owner late-detaches; only the final backing release clears it. A partially registered context must publish and observe its compensating deregistration or quarantine the transport while retaining its backing. G2H consumers remain open for submission-stopped teardown but reject partial init, sealed transport and protocol-fault quarantine; actual firmware completion and DMA behavior still require controlled hardware validation. |
| kern_vf_memirq.hpp, tools/vf_memirq_test.cpp | Read against Tahoe `IGInterruptBridge` disassembly and current i915 media-12 engine/memory-IRQ tables. This transport is admitted only on i915 device families with `has_iov_memirq` (currently MTL/ARL), never on the present RPL-P `a7a8` VF. All 64 possible source offsets, all 64 active-engine combinations, exact VCS0/VCS2 routing and context-image LRM/LRI writes/bounds are tested; real interrupt delivery remains a controlled-runtime obligation on supported hardware. |
| kern_vf_guc_event.hpp, tools/vf_guc_event_test.cpp | Read against current i915/xe HXG and G2H dispatch. The FAST-only bridge admits exact MODE_DONE, DEREGISTER_DONE and TLB_DONE shapes, classifies exact context-reset/engine-failure shapes as fatal, and rejects all other event actions rather than silently dropping firmware state. TLB_DONE must match the one active sequence and its exact modulo-32-bit predecessor; stale, duplicate and inactive completions now quarantine transport immediately instead of consuming credits until a later timeout. All 65,536 actions, lengths 0..32, header variants and completion-identity boundaries are sanitizer-tested; firmware delivery remains a controlled-runtime obligation. |
| kern_vf_runtime.hpp, kern_vf_runtime_patch.hpp | Read against current i915 VF/PF early-MMIO ABI and media-12 fuse decoding. Request framing, CRC-derived magic correlation, reply validation and invalid topology cases are sanitizer-tested; every runtime injection anchor is exact-one in both admitted accelerator payloads. Firmware/PF response behavior remains a controlled-runtime obligation. |
| kern_vf_mmio_response.hpp, tools/vf_mmio_response_test.cpp | Read against current i915/xe VF reset, version, QUERY_SINGLE_KLV, HOST2GUC_SELF_CFG and CONTROL_CTB ABIs. Zero-DATA0, parsed-count, reserved-bit and exact query-length contracts are shared by all bootstrap callers; 327,769 payload cases are sanitizer-tested. Mailbox delivery and firmware behavior remain controlled-runtime obligations. |
| kern_vf_legacy_ctb.hpp, tools/vf_legacy_ctb_test.cpp | Read against Tahoe `registerCommandTransportBuffers`/`deregisterCommandTransportBuffers` disassembly. Exact action, length, descriptor size, channel, GPU address and the CTBuffer `+0x38` registration token are validated before translating legacy 0x4505/0x4506 calls to the VF CTB ABI; invalid calls publish an explicit failure response. Firmware disable and DMA behavior remain controlled-runtime obligations. |
| kern_context_descriptor.hpp | Read against Tahoe context creation/submission/detach disassembly, its six-entry `IGHwCsType` jump table and current i915 Gen8-Gen12 LRCA/engine definitions; packed native descriptor is decoded without alignment assumptions, every persistent/reserved field and exact media-12 class/instance pair is validated, engine class is mapped explicitly, the complete low/high/SW-ID/class/instance/backing identity is shared by duplicate attach, submit and detach checks, and GuC `FORCE_RESTORE` normalization is exhaustively tested; object pointer readability remains a caller contract |
| kern_unaligned.hpp | Read; little-endian 32/64-bit reads and writes tested over offsets 0..15 with canaries; callers retain mapping/lifetime obligations |
| kern_model.hpp (removed) | Read end-to-end; the cosmetic device-name table and `getBranding` had no source or tool consumer. Removed rather than retaining a second, stale supported-device-looking list. |
| kern_netdbg.cpp/.hpp (removed) | Read completely; unbuilt/unreferenced retired logger with overread, port-shadowing and error/locking defects; recoverable in Git |
| Firmware.hpp, tools/check-dmc-blobs.sh (removed) | Read; declarations and byte-comparison helper became unreferenced with the manual DMC loader and were removed |
| kern_workqueue_unwind.hpp, tools/workqueue_unwind_test.cpp | Read against the complete pinned WorkQueue and CTB constructors/destructors. Their distinct one-lock/two-lock failure exits, marker-only OSObject base destruction, successful queue retain transfer and every modeled resource combination are sanitizer-tested. The routed callers propagate factory failure transactionally; live OOM injection remains dynamic evidence, not a static blocker. |
| AppleIntelParams.hpp (removed) | Read all 624 lines and cross-referenced every declared type. After the speculative framebuffer plane/scaler/accessor routes were retired, the generated header had no runtime consumer; its known alignment/type contradictions could only misrepresent the supported ABI, so it and both main-source includes were removed. The Ghidra extraction tools remain offline research inputs and are not driver declarations. |
| Info.plist | Read end-to-end; main personality/build identifiers and `SchedulerType` consumer inspected. The unconsumed Auto/ICL/TGL profile catalogue and impossible DisplayMergeNub personality were removed because runtime admission is code- and payload-identity-driven. The remaining plist is not a supported-device matrix. |
| IGGucBinary.h, IGHucBinary.h (removed) | Read as opaque 2017 Apple firmware arrays; no include or symbol consumer existed. Removed their 244-KiB source payload and product-header entries; recoverable in Git. Runtime firmware remains owned by admitted native/PF driver paths. |

## Host tooling coverage

- `tools/check-static.sh`, every current C++/Python offline test and
  `tools/metal_smoke/main.m` are part of the static or macOS CI path. The Metal
  program creates the default device, queue and shared buffer, performs a blit,
  waits for completion, checks command status/error and verifies all bytes; it
  remains a dynamic guest proof, not evidence from CI hardware.
- The host VF preflight, contained one-shot controller and immutable-manifest
  verifier were read end-to-end and are now CI-gated. Their contract executes
  the controller's default-refusal path, proves watcher then independent
  deadline precede its sole exact-domain start, centralizes every stop in one
  bounded exact-domain destroy helper and rejects PCI rebind, sysfs mutation,
  i915 unload and host reboot primitives. The manifest tests cover clean Git
  identity, file/zip SHA-256, Mach-O UUID and mutation/duplicate rejection. No
  privileged or dynamic controller mode is exercised by CI.
- All six executable `linux_mmio_mapper` stages were read end-to-end. The
  gate parses every tracked Python utility without importing Ghidra-only APIs,
  round-trips the checked-in sample, rejects address/symbol conflicts and
  incomplete provenance, tests source-confirmation promotion and escapes
  generated C comments. Ghidra export now uses operand reference semantics
  rather than classifying every MOV as a write, and never searches for a mask
  beyond the current function body.
- `extract_apple_params.py`, its C seed/parser output and two generated headers
  were deleted after the runtime `AppleIntelParams.hpp` consumer had already
  been removed. The generator's stale signature list and known field-width/
  alignment contradictions could only recreate an unsupported private ABI.
  `parsed.gdt` remains as inert archival reverse-engineering data and is never
  loaded by the build, tests or driver.
- The complete Xcode project, shared scheme/workspace, `Info.plist` and GitHub
  workflow are covered by `project_contract_test.py`. It proves all six
  product `.cpp` files plus Lilu's `plugin_start.cpp` are compiled, all 31
  product headers are transitively reachable, all three target configurations
  retain the x86_64 kernel-extension settings, and plist version/personality/
  Lilu dependency values agree. It also rejects shell phases: the old empty
  always-run `kern_fw.cpp` producer and broad product-directory zip cleanup
  were removed. Debug/Sanitize retain symbols without deployment stripping;
  Debug re-enables null, divide-by-zero and dead-store analysis.

## Evidence rules

- `TAHOE_SRIOV_PROTOCOL_AUDIT.md` records concrete findings, source references,
  tests and unresolved conditions. A read file may still have severe defects.
- The pinned TGL binary was inspected by function, not exhaustively. UUID
  admission limits private-layout use but is neither integrity verification
  nor evidence that the known layout is fully safe.
- No Metal/media baseline, dynamic DMA-quiescence proof, complete PF/VF driver,
  virtual-display/Sunshine integration or all-source approval exists yet.
- Preserve the original user baseline in the parent `WORK_BASELINE.md` and
  append checkpoints; do not replace it with a narrower task.

### Active-route and mandatory-lifecycle cleanup

- Removed the active `IGHardwareBlit3DContext::operator new`,
  `IGHardwareExtendedContext::initWithOptions` and
  `IGAccelSegmentResourceList::prepare` wrappers because they only called the
  captured native function. The extended-context wrapper also contained about
  180 lines after an unconditional return: an unreachable second native call
  plus obsolete MOCS, whitelist and workaround writes. Native allocation,
  initialization and resource preparation remain installed by Apple.
- `IGMappedBuffer::getMemory` remains a required native dependency of the VF
  context bridge, but no longer has a pass-through route. The pinned symbol is
  solved directly with the other VF buffer accessors, preserving the exact
  native entry address without modifying call dispatch.
- Removed the active ICL/TGL `initCDClock` and TGL
  `setCDClockFrequencyOnHotplug` hooks because both were zero-effect native
  trampolines. The separate CDCLK probe/sanitize path and its captured native
  disable entry remain under review.
- The UUID-pinned TGL accelerator's bootstrap symbols and start/stop lifecycle
  are now mandatory. Missing CTB dispatch, mapped-buffer, task-counter or
  lifecycle symbols fail payload admission for PF and VF instead of logging and
  continuing on a partially routed private ABI. This is static admission
  hardening, not runtime proof that GuC teardown completes.
- The full syntax gate, zero-finding analyzer, strict Gen11 warnings and every
  offline sanitizer/protocol model pass in `/tmp/ngreen-static.FYOPcq`. The VM
  stayed off. Review of the remaining physical start path found contradictory
  legacy reset/error-mask timer experiments; they are the next open safety
  block and are not certified by this checkpoint.

### Accelerator lifecycle and legacy experiment removal

- Replaced the roughly thousand-line accelerator `start` wrapper with the
  admitted lifecycle contract: classify PCI identity, bootstrap a VF, publish
  scheduler selection, disable PF-owned VF PM/fallback paths, call native start
  and publish only after success. Mandatory VF property failures now mark a
  protocol fault. Physical start no longer writes ring/IRQ/error/GGTT state.
- Removed recurring IRQ, health, EMR and bundle/child timers together with
  their retained service state. Removed pre/post-start GDRST, blanket EMR/error
  masking, BCS stop/head-tail rewrite, manual CSB draining, fixed GGTT page-zero
  remaps and forced child registration. These paths had no symmetric stop
  ownership and included comments documenting that the same reset killed the
  ring.
- `startGraphicsEngine`, `stopGraphicsEngine` and
  `populateResetRegisterList` now isolate PF-owned state on a VF and otherwise
  preserve the physical native implementation. Final VF stop retains the
  mandatory GuC context/DMA quiescence boundary.
- Removed physical-only LRCA repair hooks which used global cache flush plus
  temporary `GGTT[0]` remaps and hard-coded a submission slot. Also removed the
  partial Blit3D initializer, incompatible resolve-context substitution,
  unretained context caches and three wrappers that skipped native usage/DTrace
  work while reporting completion. The four context getters now only enforce
  complete VF admission and return their own native object type.
- `kern_gen11.cpp` now contains no `GDRST`, `wbinvd`, blanket `RING_EMR`
  mutation, `GGTT_PTE_LO(0)` remap or `thread_call_allocate`. README runtime
  claims and boot arguments for the removed Sonoma-era experiments were
  retired. The full static suite passes in `/tmp/ngreen-static.aikyPk`; the VM
  remained off and hardware behavior is not inferred from that result.

### Physical framebuffer policy cleanup

- Removed the remaining V400--V408 physical scanout experiment group and its
  BAR2/GGTT diagnostic probe. Native plane, scaler, CRTC, watermark, DSC and
  color-pipeline builders are no longer cross-mutated by that group.
- Removed forced aperture-memory and WindowServer-active policy, continuous PSR
  writes, the logging-only online-state/FastWrite hooks, and stale boot options.
  The internal force-wake MMIO helper now uses the common checked BAR mapping
  instead of unverified controller-object offsets. This completes review of
  those routes only; AUX, lane selection, power-well, DMC and remaining byte
  patches are still open. `/tmp/ngreen-static.cyj2sZ` passes the full suite.

- Follow-up review removed the AUX payload mutation, GOP-state lane override,
  private port-field write and pending-CRTC/live-register substitution. Native
  DPCD, link-status recovery and modeset comparison are authoritative again on
  ICL/TGL physical framebuffer paths. Power-well, DMC/CDCLK and remaining byte
  patches are still open. `/tmp/ngreen-static.dZ50PB` passes the full suite.

- Manual DMC SRAM upload and DC-exit replay, fixed transcoder/link/panel values,
  no-ACK power-well replacement, forced `fAlwaysOn` and unconditional CDCLK
  disable/reprogram were removed. Native physical firmware, power and clock
  lifecycle is authoritative; VF admission already rejects the framebuffer.
  The two now-unreferenced firmware payload sources and their comparison tool
  were removed from source/build metadata. Remaining framebuffer byte patches
  and accessor-repair routes are still open. `/tmp/ngreen-static.ZxzxZm`
  passes the full suite; macOS project/build validation remains a CI gate.

### Boot-policy cleanup

- The old `ngreenfullmtl*` switch did not enable Metal. Its only remaining
  effect was to override WEG coexistence and install the physical force-wake
  replacement; a VF already selects its required no-op isolation route. The
  misleading switch and its dead helper were removed, together with the dead
  `ngreenexp` parser left after recurring monitors were deleted.
- The workspace OpenCore generator no longer gives the VF physical-display
  DMC selection, obsolete version/3D switches, firmware-disable policy or the
  removed full-Metal switch. The VF profile now explicitly requests scheduler
  4 and otherwise keeps only debug, compatibility and security-policy inputs.
  The complete syntax/analyzer/protocol suite passes in
  `/tmp/ngreen-static.zuSxRI`. This is configuration hygiene, not a Metal
  result.
# Latest offline display dispatch checkpoint

Reviewed and pinned the base `displayModeWillChange`/`framebufferTerminated`
virtuals, display user-client external dispatch wrapper, and inherited BootKC
`IOUserClient::externalMethod` instruction body. Explicit imported table-header
offset is verified separately from object-vptr offsets. Descriptor dispatch
does not supply an accelerator lock or terminal admission check. Subclasses,
higher-level entry serialization, legacy dispatch helpers, descriptor action
coverage and lazy-setup failure unwind remain incomplete. No VM/PCI writes.
# Latest selector/action checkpoint

Selector 8 descriptor counts/target, `s_transaction_end` forwarding action,
and complete base pipe `transaction_end` span are pinned and locally checked.
Preparation errors preserve status but still queue the transaction. Downstream
error retirement, prepare/argument callee bodies, virtual +0x8d8 overrides,
event-machine +0x1b8 target and remaining 13 descriptor actions are pending.
This extends reviewed coverage, not hardware acceleration or DMA safety proof.
# Latest error-queue ownership checkpoint

Pinned/reviewed complete gated queue, transaction prepare/free, base submit
and begin virtuals. Queue overwrites transaction status with submit's result;
base submit preserves prior errors but does not prove hardware submission.
Free sends an 11-word async result before client/resource releases. Pending:
actual framebuffer subclass overrides, resource preparation virtuals/imports,
async payload semantics, queue-to-finished transfer and effective admission.
Full-project and hardware-acceleration completion remain unproven.
# Latest Intel display override checkpoint

Reviewed complete IGAccelDisplayPipe begin/submit in both pinned payloads;
verified effective vtable slots and external getter/legacy-table relocation
identities. Paired SystemKC legacy submit slot resolves to reviewed base
submit, not a new hardware-success method. Event-machine +0x1d0/+0x1d8/+0x1e0
targets, runtime vtable changes and virtual-display feasibility remain pending.
# Latest display stamp-record checkpoint

Resolved display-called event virtuals through external relocations to Fast2
setEventStamp/incrementStamp/writeStampCommand. Reviewed Intel writeStamp and
getStampOffset complete bodies and effective vtable targets in both payloads.
CPU `{index<<6,stamp}` record construction is not GPU submission/completion.
Dependency overflow helper, record consumers and hardware retirement remain
pending; this does not close runtime acceleration or containment requirements.
# Latest software completion checkpoint

Pinned/reviewed pipe completeTransaction/isTransactionComplete, transaction
complete/finish/sendNotification and the async-send wrapper. Queue ID ordering
is not hardware completion; prepared flag is not cleared by complete/finish,
so caller one-shot ownership/order must be established. Intel submit's stack
record has no subsequent consumer in that complete override. Remaining:
resource/import callees, signal override, retirement callers and genuine DMA
backing lifetime. Targeted paired-KC checks pass; no runtime mutation.
# Latest resource-count checkpoint

Reviewed/pinned complete base resource prepare/complete bodies and SystemKC
virtuals, plus Intel complete override/local slot/base-table import in both
payloads. Complete's unguarded 32-bit decrement requires caller pairing;
prepare success/count is not DMA completion. Mapping/recovery callees,
backing cleanup helper and Intel auxiliary virtuals remain pending. No
runtime evidence supports calling the counter hazard an observed VF defect.
# Latest finished-list pairing checkpoint

Reviewed/pinned accelerator finished-list unlink→finish→complete→release,
both display cleanup caller edges, extraction wrapper, complete current-plane
replacement body and memory-map remove_resource. The latter only compacts CPU
inventory. Local pairing is established for this path; other callers,
replacement event completion, backing destructor and DMA lifetime remain open.
# Latest backing final-free checkpoint

Reviewed/pinned Intel memory-map free delegation, complete base memory-map
free and parent remove_mapping inventory helper, with effective vtables and
native import identities. No DMA barrier is established by these bodies.
Parent final free, earlier unmap/complete, deallocator and inherited-free
callees plus concurrent ownership remain pending. Targeted checks pass.
# Latest parent/SysMemory free checkpoint

Reviewed/pinned base parent memory free/complete and inherited SysMemory free.
Resolved Intel SysMemory inherited-free import and conditional +0x1b8 target
to Intel unwire. Unwire body, retained descriptor cleanup and accounting/
collection callees remain pending. Complete counters are not DMA barriers.
# Latest unwire checkpoint

Reviewed/pinned Intel unwire delegation, complete base unwire/release_pte,
returnDMACommand/sysmem_unwired bodies and concrete cleanup edges. Base
unwire logs DMA virtual errors and continues; descriptor return is not tested.
Concrete DMA/descriptor/purge virtuals, conditional mapping +0x178, parent
+0x1d8 and helper lock/collection semantics remain pending. Not a drain proof.
# Latest mapping-release consolidation

Re-reviewed/pinned complete Intel mapping/manager release bodies and effective
mapping slot. Connected the prior native releaseRange review to SystemKC
release_pte's ignored bool result. This confirms false-return alone cannot
preserve backing; existing VF unmap barrier remains required. No executable
driver change. Task page-table ownership/getters, descriptor cleanup and Host
DMA containment remain unproven.
# Latest DMA-command identity checkpoint

Reviewed/pinned complete base IODMACommand complete/clearMemoryDescriptor
bodies and canonical Boot vtable slots. General descriptor complete slot is
resolved but its body is not reviewed here. Runtime subclass/descriptor identity,
mapping helper and DMA-operation virtuals remain pending. False clear rejects
outstanding prepare references; cleanup success is not GPU retirement proof.
# Latest descriptor/page-release checkpoint

Reviewed/pinned full GeneralMemoryDescriptor complete and DMA walkAll spans;
resolved explicit UPL commit/abort/deallocate and vm_page_free_list calls.
Actual page-release edges are established, not GPU retirement safety. Concrete
receiver identity, active-DMA count producer pairing, mapping walker/callbacks
and VM callee implementations remain pending. Targeted paired-KC checks pass.
# Latest descriptor DMA-operation checkpoint

Reviewed/pinned complete GeneralMemoryDescriptor dmaCommandOperation span,
six-category jump table and atomic +0x34 increment/decrement. Category-3 count
side effects precede a later size check; errors are not necessarily side-effect
free. SetMemoryDescriptor/clear registration pairing and mapping VM callees
remain pending. This protocol count is not direct GPU execution evidence.
# Latest DMA registration pairing checkpoint

Reviewed/pinned complete base setMemoryDescriptor and effective slot +0x128.
Established flagged 03000001/03000000 registration pairing with ignored
operation results and prepare-failure forced-clear order. Runtime subclasses,
prepare failure semantics, creator options and concurrency remain unproven.
No GPU retirement inference or production fix is made from ignored errors.
# Latest base DMA factory checkpoint

Reviewed/pinned base accelerator create/getDMACommand, exact imported Boot
withSpecification overload and cross-KC import resolution. Mapped-mode/64-bit
factory arguments are resolved; loaded globals, metaclass/init implementation,
effective Intel factory/pool construction and runtime receivers remain pending.
# Latest DMA template-pool checkpoint

Reviewed/pinned complete create/releaseDMACommandPool, Intel inherited factory
import and Boot cloneCommand slot. +0xa10 is the template command, not a
distinct pool object. Outer failure cleanup and concurrent get/return admission
must protect release's unchecked template dereference/unlocked list cleanup.
Clone/metaclass bodies and shutdown serialization remain pending.
# Latest failed-start pool unwind checkpoint

Decoded/pinned selected start failure windows and direct createPool→false
common path→base stop→releasePool edges. Together with partial pool creation,
this exposes a static allocation-failure cleanup hazard; not a dynamic crash
attribution or complete start review. Per-object VF ownership/partial-init
state and external admission must precede a safe shared-IOAccel repair.
