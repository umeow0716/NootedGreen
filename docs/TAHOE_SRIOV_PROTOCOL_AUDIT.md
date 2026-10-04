# Tahoe SR-IOV protocol audit — in progress

Updated: 2026-10-04. The last dynamic source baseline is `ce166c8`; the latest
completed offline-reviewed checkpoint is V267 event-timeout fail-stop on
`codex/tahoe-sriov-vf`. This is NOT a boot-test candidate or a successful
driver baseline. The `ce166c8` run produced repeatable host PF DMAR faults
followed by i915 hangs and a host reboot. Keep `macos-tahoe-sriov` shut off until the
corrected code passes the remaining offline review and every independently
enforced containment precondition.

## V267 native event-timeout failure boundary (offline)

The verified Intel `IGAccelEventMachine::eventTimeout(int)` entry now has a
classified-VF-only replacement. It marks the protocol fault (closing native
work admission) and fail-stops the guest before returning to inherited restart
and wait retry. It does not call the original, free backing, fabricate stamp
progress or issue hardware reset/MMIO. Ordinary debug capture remains a no-op;
PF behavior remains native. The route inventory is now 96 (93 accelerator,
3 framebuffer admission symbols); both payloads pin the event-timeout virtual.

This is a provisional error-containment boundary, not implemented GPU recovery,
successful acceleration or host DMA containment. A guest panic cannot stop
already-issued DMA or guarantee PF health, so the runtime hold and independent
host watcher/deadline are still mandatory. Normal completion, stable reclamation
and a real hardware-safe recovery design remain unfinished. Offline tests
enforce fault-before-panic and prohibit native/helper/backing side effects.

## Mapping getter provenance follow-up (offline)

Display client ownership/terminal follow-up: complete user-client start 0x6c,
setPipeIndex 0x168, pipe user_client_terminated 0x7e and framebuffer_terminated
0xc2 reviewed. Start classifies/stores accelerator+0xd8 and display-machine
+0xe0, then inherited start. setPipeIndex holds accelerator mutex/busy lock,
releases prior pipe, selects new pipe, checks entitlement/exclusive ownership,
registers client and retains pipe on success. A separate complete 0x0e setter
at 0x14bb350c stores raw client pointer in pipe+0xe8 without retain; transaction
list entries are removed/released by user_client_terminated. Its corruption
cold branch is not reviewed as normal successful cleanup. User-client stop's
pipe release/clear uses the same observed accelerator lock/busy ordering.
Framebuffer termination sets pipe+0x280 before virtual cleanup/release-live,
unregisters framebuffer notifications then clears +0x98/index; it is not a
global native event-owner drain. Added four complete body hashes and six
ownership/terminal anchors; local paired KC passes. Cold callee, selection
helper and terminal virtual implementations remain separate obligations.
No production/VM change. Next terminal virtuals and external dispatch
serialization before implementing a per-VF owner/lifetime boundary.

Upstream display ABI/lifetime follow-up: fully reviewed submitFlipBufferTransaction
0x14bafaf0 (0x12a), DisplayPipeUserClient stop 0x14bb56dc (0x12c), requestNotify
0x14bb5cb6 (0xde), transactionEnd 0x14bb622e (0x232). Flip submit ignores
queue-slot wait result then tails into transaction_queue; user-client stop
holds accelerator mutex/busy lock, removes notification ignoring its result,
notifies pipe and releases/clears client pipe+0xe8 before inherited stop.
requestNotify checks pipe+0xe8 and propagates returned gate result. transactionEnd
checks client transaction state, pipe terminal flags and accelerator enabled
state; when queue is full it retains pipe, releases accelerator busy/mutex,
waits without these locks, then reacquires and releases pipe before retrying
state checks. The wait's result is ignored. This establishes a real temporary
pipe-lifetime lease and avoids holding accelerator locks across that wait;
it is not global caller admission closure or proof of timer owner drain.
Added four complete hashes, four upstream graph edges and result handling
anchors; local paired KC passes. Uniform failure-return stubs across these
callers are not valid repairs: ignored results/required cleanup must be handled
at their actual owners. No production/VM changes. Next actual user-client/pipe
ownership registration and terminal state ordering for VF-scoped integration.

Lazy display setup/error-boundary follow-up: complete pipe init
0x14bae358 (0x32c) reviewed: stores accelerator+0x88/display-machine+0x90/
framebuffer+0x98, initializes four embedded events/storage/notification; it
does NOT call setup_workloop. Four complete lazy callers were read:
wait_for_queue_slot_nolock 0x48, transaction_queue 0x4a, request_notify 0x58,
remove_notify 0x4a. Each tests gate+0xb0, calls setup if null, reloads it and
dereferences immediately without checking post-setup success. Thus an init-only
guard is the wrong boundary; setup allocation/attachment failure cannot be
safely handled merely by returning early from its void body. Teardown clears
+0xb0, so preventing later lazy reconstruction requires a separate external
owner admission/lifetime invariant. This is a conditional race/failure hazard,
not a proven concurrent call or observed crash. Added five complete hashes,
four exact setup edges and unchecked post-setup loads; local paired KC passes.
Coverage ledger updated. No production change/global hook/VM operation. Next
upstream caller ABI, owner identification and terminal admission state, not a
blanket transport shutdown that also rejects required retirement work.

Display workloop provenance/hook-scope follow-up: complete
IOAccelDisplayMachine::get_pipe_workloop 0x14b73d76 (0x2a) reviewed; returns
cached +0x120 or calls import 0x10138 and stores result. Base display-pipe
vtable +0x8c8 resolves createWorkLoop; shared path uses this getter plus retain,
private path uses the same 0x10138 import. Stub → cell 0x241a0 chained level 0
resolves previously reviewed Boot IOWorkLoop::workLoop base factory. Added
getter hash, base-pipe virtual and paired import identity; local paired KC
passes. This resolves base construction, not all possible display subclasses,
shared-workloop concurrency, attachment success or caller ownership.
Current production IOAcceleratorFamily branch only resolves lifecycle methods;
it has no display setup/free route. A global System-kext hook cannot be added
as if it were the existing VF-only TGL route table. A repair requires concrete
per-owner VF classification and a proven init-error/unwind ABI, without changing
PF/other accelerator behavior. No such global hook was introduced. Shared vs
private workloop provenance is now resolved; next owner classification and
initialization callers/error propagation are needed for scoped integration.

Concrete display gate follow-up: complete release_live_transaction_gated
0x14bb2d10 (0x40) moves live+0x250 transaction to finished list and clears
live ownership, without directly changing event interrupt references. Complete
createWorkLoop 0x14baf08c (0x5e) chooses shared device workloop/retain or kernel
workloop factory; complete setup_workloop 0x14bb15f6 (0x2a0) installs +0xb8,
calls commandGate(pipe, NULL action), stores +0xb0, then adds/enables gate and
five sources without checking add statuses. Allocation failures log and continue.
Resolved System import stub 0x10468 → cell 0x245e0, chained level 0 → Boot
IOCommandGate::commandGate at 0xffffff8000acd100. Entire 0x60 factory reviewed:
metaclass allocate +0x88, init +0x1b8, failure release/null. Existing concrete
allocator/init evidence applies; effective bound gate still must be validated
because constructor ignores attachment return. Pairing a gate factory with
a field store does not prove successful synchronization or all caller ordering.
Added four complete bodies (three System/one Boot), import identity, allocator/
init/failure edges, null-action/store/unchecked-add anchors and live-action LEAs.
Local paired KC passes; production unchanged. Next source/gate attachment
failure handling and caller lifetime across shared versus private workloops.

Display cleanup helpers follow-up: fully reviewed signalTransactionInterrupt
0x14bafd20 (0x80): checks +0x2a4, records time and signals source+0xd0;
it is not a synchronous queue flush or reference-deregistration acknowledgement.
Fully read finishTransactionQueue 0x80, releaseLiveTransaction 0xf2 and
teardown_workloop 0x102 called by free. They dispatch actions through object
+0xb0 virtual +0x1c8 before collecting finished transactions/releasing sources.
Resolved and fully read transaction_queue_idle_gated 0xc0,
get_finished_transactions_gated 0x48 and teardown_workloop_gated 0x132.
Idle waits on queue indices/active transaction, invokes event finish and the
reviewed event_interrupt_gated, then uses +0xb0 virtual +0x1e0 to wait/retry.
Finished-list helper moves list ownership; source teardown disables/removes
timer, transaction, event and other sources (six removals), ignoring statuses.
The outer helper releases/clears sources then gate+0xb0/workloop+0xb8.
Queue wait is an actual balancing candidate, not proof of concrete gate type,
admission closure, timeout safety or successful source detach. Added seven
complete hashes, three gated-action LEA identities and six-removal inventory;
local paired KC passes. No production/VM changes. Next concrete gate/source
construction and remaining live-transaction gated actions/caller serialization.

Display queue caller follow-up: full event_interrupt_gated 0x14bb1896 (0x41e)
read, with direct disable at 0x14bb19aa before transaction processing and
enable at 0x14bb1aa6 when event test returns false and pipe flag+0x29c is zero.
Queue indices+0x238/+0x23c, active transaction+0x248 and timers govern retries.
The terminated byte+0x280 is checked on the processing path, not directly at
the unmet-stamp enable branch; actual queue-flush/caller serialization must
be resolved before declaring late enable impossible. The method's name is
NOT a lock ownership proof. Complete device_terminated 0x14bafd0e (0x12) sets
+0x280 then tails to a separate helper at 0x14bafd20. Complete free
0x14baea94 (0x1c0) calls three cleanup helpers before releasing storage/locks;
their drain semantics remain unreviewed, so free alone proves no reference
balance. Added these three whole-body hashes and two exact direct enable/
disable queue edges; local paired KC passes. No runtime edits/VM actions.
Next resolve termination queue helper and the free helpers plus gated entry
construction/callers; do not insert a lock based on a suggestive method name.

Display-pipe admission caller follow-up: effective Fast2 vtable +0x258/+0x260
resolve event-stamp enable/disable. No direct calls to those implementations
were found by exploratory IOAccel text decoding; same-offset indirect calls
from unrelated object types must not be classified as event-machine edges.
Fully reviewed display pipe enable_event_interrupt 0x14bb2288 (0x4c) and
disable_event_interrupt 0x14bb22d4 (0x52): pipe flag+0x29c gates duplicate
enable, set before pipe+0x88 accelerator → +0x380 event owner → +0x258.
Disable first calls its device-side helper, dispatches owner +0x260, then
clears flag. Neither complete body takes a lock; caller synchronization and
teardown balancing remain unproven. Added complete hashes, event virtuals and
receiver-load/dispatch anchors to local paired-KC contracts; pass. An observed
+0x260 tail in deviceTerminatedUnlocked uses an explicitly loaded BASE vtable,
not the effective event enable/disable table, so it is not classified as this
reference cleanup. GitHub run 37156234302 observation initially returned HTTP
504; this was not treated as terminal or a reason to restart CI. Production
unchanged; next inspect display-pipe callers and outer lock/teardown ownership.

Graph-verifier and event admission follow-up: direct byte branch candidates
now reject any displacement overlapping external relocation storage. The native
periodic unlock placeholder at 0x5690f must not create an enable-to-disable edge;
synthetic zero-placeholder, all four displacement bytes, truncation, empty
range and valid negative-displacement tests enforce this. Both payload graph
contracts still pass. This remains a byte-candidate verifier, not a complete
x86 boundary decoder or indirect reachability proof.
Fully reviewed enableEventStampInterrupts at 0x14b95480 (0x66): iterates eight
event channels, skips -1 entries, increments per-channel reference under
lock+0x50, invokes +0x240 on zero-to-one and unlocks. It has no stopping or
external admission check in this complete body. Its complete hash is now
locally pinned alongside the matching disable path; whether its callers
serialize with teardown remains open. Do not confuse transport producer gate
with this independent reference-creation entrypoint. Production unchanged.

Admission boundary follow-up: reread production vfNativeGpuWorkReady,
acceleratorStop, context enter/leave/RAII close-and-wait, quiesce and counted
attach/submit entrypoints. DeviceStopping is a declaration only: readiness
does not consume it, intentionally preserving native finishAllStamps retirement
before engine-stop quiescence. Quiesce seals submission and closes/count-drains
GuC attach/detach/submit, not native event waiters or periodic callback owners.
Added an explicit production comment to prevent confusing transport readiness
with a lifetime lease. Existing actual readiness tests now exercise sequential
producer-stop/IRQ-disable/poll-drain/CTB-seal policy; the abstract timer model
also retains prior-reference and subsequent-admission counterexamples after
balanced finish waiter exit. No runtime behavior changed. The missing external
admission boundary cannot be repaired by simply adding DeviceStopping to every
readiness check, which may block required native retirement submissions.
Full offline suite passes at /tmp/ngreen-static.EaVZKt; only the two existing
SDK macro warnings remain. Checkpoint 8b65e9e CI 37155938371 passed. VM hold
is unchanged; no runtime teardown or hardware acceleration was exercised.

Per-channel finish follow-up: Fast2 effective +0x150 decodes level-1 target
finishStamp at 0x14b9554c, complete 0x158 body reviewed/pinned. It returns early
if stamp delta is nonpositive; otherwise reads mapped stamp, dispatches
waitForStamp through +0x238, and on errors calls signalHardwareError then
retries. It does not itself sweep all existing interrupt references. Fully
reread waitForStamp 0x314 and disable_stamp_interrupt 0x48: admitted waiters
increment lock-protected per-channel count+0x48, invoke +0x240 only on 0-to-1,
and decrement on ordinary exit, error exit, or timeout helper, invoking +0x248
only on 1-to-0. The pre-admission non-hardware bypass creates no waiter ref.
This balances the individual waiter, NOT all prior event/waiter references,
future admission or raw callback owner lifetime. FinishAllStamps is therefore
not by itself a proven periodic drain. Added effective finish virtual, complete
body hash, wait dispatch and success/error/timeout cleanup anchors; local paired
KC passes. First count-store encoding omitted the REX/index prefix and failed;
exact bytes rechecked and corrected to 42 89 14 a0. Production unchanged.
Next determine who closes external event/waiter admission and how existing
references are retired before owner/free; do not fabricate zero reference state.

Actual accelerator event-stop edge follow-up: complete native Intel stop
0x263c8..0x267ac reread; it only invokes inherited accelerator stop for a
nonnull provider (0x266d8 branch). Normal inherited stop loads event machine
+0x380, calls virtual +0x158 then +0x268. Intel event vtable imported external
entries 0xceb60/0xcec70 (0x0e relocations, NOT runtime null pointers) resolve
to Fast2::finishAllStamps and base event-machine stop. These typed identities
and inherited call ordering are now fixture-pinned. Null-provider start-failure
cleanup skips this inherited path; do not extrapolate normal-stop drainage to it.
Fully reviewed Fast2 finishAllStamps 0x14b956a4 (0x6c): iterates channel count
+0x30, invokes virtual +0x150, aggregates results and preserves -1 failure.
It does not directly clear Intel fallback bitset/counter or deregister source;
those obligations depend on per-channel finish and caller admission. Complete
body hash is pinned in paired KC tests. Both native payload contracts and
paired KC pass; production unchanged. Next resolve +0x150 per-channel finish
and its stamp-disable edges before inferring owner-safe shutdown.

Inherited fallback teardown follow-up: Intel free's RIP import storage 0xc81b8
is external relocation __ZTV24IOAccelEventMachineFast2 (type 0x0e); SystemKC
that vtable +0xa0 decodes level 1 target 0x14b95072 Fast2::free. Fully reviewed
Fast2 free 0x12, base free 0x14b777a8 (0x108), base stop 0x14b776ba (0x63),
its owner block 0x14b7771d (0x8b), and disableEventStampInterrupts
0x14b954e6 (0x66). Fast2 free delegates base free, which frees locks/arrays,
clears accelerator+0x10 and other fields, then delegates OSObject cleanup;
it does not deregister Intel's periodic fallback source. Base stop synchronously
runs a workloop block removing three base sources (+0x60/+0x78/+0x68), with
unchecked remove statuses, then releases/clears workloop+0x58. It does not
touch Intel +0xd30 or periodic scheduler membership. Per-event stamp-disable
iterates eight event channels under lock+0x50 and invokes virtual +0x248 only
on a reference count 1-to-0 transition. This offers an outer balancing path,
not proof that every accelerator teardown uses it. Added five full-body hashes,
inherited Fast2 free vtable identity and three-source stop inventory to local
paired KC tests; pass. An exploratory substring symbol selection also printed
kalloc_type_view DATA; that was excluded from code review/pinning. Production
unchanged. Next trace actual accelerator cleanup callers, not assumed stop drain.

Fallback owner follow-up: fully reread event-machine init 0x15c82 (0x64),
free 0x15cfe (0x66), callback 0x15ce6 (0x18), and enableSchedulerEvents
0x15d64 (0x40). Init creates source+0xd30 with the event machine as owner
and this callback as action; callback forwards owner+0x10 accelerator and
owner+0xd40 fallback bitset to signalStampUpdate. enableSchedulerEvents gets
accelerator workloop, calls addEventSource without checking status, then sets
flag+0xd88=1. Free conditionally removes the source using that flag, ignores
remove status, releases/clears +0xd30, then delegates inherited free. No
periodic collection deregistration exists in this complete derived free body.
An outer fallback-disable/owner-drain invariant remains unproven; retaining
source in scheduler's OSSet does not prove raw callback-owner lifetime.
Added exact enable body hash and constructor callback/owner/source plus
unchecked add/flag/remove/release anchors; both payload tests pass. Initial
exploratory 0x44 range included the next function prologue; exact symbol/body
boundary was rechecked as 0x40 before pinning. Production unchanged, no VM.
Next audit stop/disable callers and inherited owner teardown before choosing
a production ownership/admission fix (do not fabricate source cancellation).

Periodic caller inventory follow-up: reviewed complete event-machine enable
0x16182 (0xb6) and disable 0x16244 (0xf4). Their software stamp fallback
selects accelerator+0x1250 scheduler and event-machine+0xd30 source, with direct
periodic enable/disable tail branches 0x16232/0x162de. Enable uses the first
fallback-count transition; disable checks the fallback bitset and last-count
transition. Neither body establishes a teardown admission boundary. This
fallback cannot be excluded merely by the VF type-4 factory guard.
Also fully reviewed streamer5 init 0x39a78 (0x224), registerForInterrupts
0x3a01e (0x90) and enableContextSwitchInterrupt 0x3aaa0 (0x36): periodic
enable at 0x39c76 and disable at 0x3a033/0x3aab4 use streamer scheduler+0x20
and source+0x1c08. Init has unchecked workloop source-add results and a virtual
free failure edge; these type-5 paths are not the admitted VF scheduler4 path.
Added exact full-body hashes for these three and five direct graph edges plus
fallback ownership loads; both payload lifecycle tests pass. First exploratory
linear disassembly stopped early at an invalid byte; rerun with skipdata found
the calls, and native imported IOLockUnlock placeholder at 0x5690f was rejected
as a spurious apparent branch to disable. No whole indirect caller inventory
is claimed. Next inspect fallback-source construction/action/stop ownership.

Removal lock-order follow-up: complete Boot removeEventSource 0x30,
runCommand 0x30 and runAction 0x280 disassembly rechecked. Removal passes
operation 1 and source to controlG+0x20 virtual +0x1c0, which dispatches the
stored maintenance action through +0x1c8. runAction obtains workloop gate
(+0x180) before action invocation and normally opens it (+0x178) afterward;
unbound/disabled control gates have separate failure/sleep paths. This is
synchronous delegation, not an asynchronous detach acknowledgement. Local
paired-KC assertions now explicitly pin operation/source/controlG delegation.
The abstract teardown model additionally detects the two-thread wait cycle
gate-owner callback waiting on mutex / mutex-owner cleanup waiting on gate;
consistent gate-first ownership has no such modeled cycle. Recursive same-
thread gate acquisition is excluded from that cycle detector, but self-callback
drain must still be deferred. No safe outer producer lifetime/admission contract
is thereby proven; do not deploy a mutex-across-remove workaround. Paired KC
and model tests pass; this fixture/model-only change has no production edits.

Periodic synchronization follow-up: fully disassembled native enable 0x5689c
(0x78), disable 0x56914 (0x64), callback 0x56688 (0x9c), and shared cleanup
0x565a8 (0xe0). The three producers lock scheduler+0x440; enable/disable update
a separate unchecked counter at +0x450, while callback checks OSSet+0x438
count and rearms timer+0x448 under the mutex. Callback also dispatches event
action +0x1e0 under this lock. Collection mutation results are not checked;
duplicate enable/unbalanced disable semantics still require caller review.
Cleanup cancels/removes/releases timer, releases collection, then frees mutex;
it does not acquire that mutex in this complete body. No proven outer producer
drain has been established. Do not simply hold the mutex across source removal:
the verified timeout path obtains the workloop gate before invoking callback,
which then takes this mutex. Mutex-to-workloop removal may invert that order.
Actual removal locking, entry admission and external caller ownership must be
resolved before production integration. Semantic anchors now pin distinct
counter/collection/action/rearm instructions alongside existing whole-body
hashes and imported lock calls, on both payload copies. One initial anchor
offset typo (+0x66 versus actual +0x62) failed offline and was corrected;
the targeted lifecycle contracts now pass. No runtime state changed.

Timer teardown model follow-up: tools/vf_timer_teardown_model_test.py
enumerates 30 atomic event orders preserving cancel-before-release and
callback-begin-before-finish. It retains a concrete begin/cancel/rearm/release/
finish counterexample: cancellation alone allows rearm and owner use after
release; closing admission alone leaves an entered callback alive. Abstract
close-admission plus deferred release until no pending/active work avoids these
model violations, with reachable successful releases (not a never-free policy).
This is a specification model, NOT a production fix or proof of native
atomicity, lock ordering, generation checks, callback ownership or DMA drain.
Native stop currently cancels DPSM; this model does not invent a stopping check
in the native timer producers. Actual gate/drain integration remains required.

VF scheduler startup binding guard: route the typed base initWithOptions
entry only for VF. Preserve native failure; after native success require the
timer at scheduler+0x448 to report accelerator+0xf0 through getWorkLoop.
An unattached timer rejects initialization and leaves cleanup to the factory.
Missing ownership fields or a foreign workloop fail-stop rather than releasing
an owner potentially still registered elsewhere. This checks a binding snapshot,
not chain membership, concurrent mutation, callback drain or DMA quiescence.
The typed getter's emitted LLVM uses slot 45 (+0x168), matching the pinned
Boot kernel ABI. Full offline suite passed at /tmp/ngreen-static.8lWs1f;
only the existing SDK macro warnings remain. No deployment or VM operation.
Checkpoint 54665b9 CI 37154429588 subsequently completed successfully.

Timer binding getter follow-up: complete Boot IOEventSource::getWorkLoop
0x10 body reviewed/pinned: returns source+0x30. Timer effective vtable +0x168
resolves to this getter. An exploratory +0x120 guess instead resolved to timer
checkForWork/action dispatch and was discarded; it must never be used as a
workloop getter. SDK declares getWorkLoop virtual, so future typed calls must
also preserve the verified effective ABI. This provides a concrete startup
postcondition for native base scheduler init's unchecked addEventSource:
compare the created timer's binding to the accelerator workloop before later
producer admission. Production startup validation is not added in this turn;
failed detach/owner lifetime still requires independent handling. Local paired
KC tests pin exact getter bytes and timer vtable edge; no runtime calls made.

Scheduler patch uniqueness hardening: LookupPatchPlus preflight uses
hasAtLeast for count=1, which does not reject duplicate candidates. Scheduler
repair now separately requires an exact 0xaf range, one overlapping-aware
findUnique result, and fixed +0x93 location before apply. Missing, duplicate,
moved, truncated and null inputs reject without patch writes. Offline tests
exercise every anchor byte mutation, duplicate/moved candidates and unchanged
input on failure for both payloads. This strengthens production admission,
not synchronization against another patcher modifying memory concurrently.

Scheduler4 partial-init ownership repair: VF patch admission now resolves
initWithAccelerator and the following waitForGpuIdle symbol, requires the
pinned 0xc2 symbol distance, and searches only the actual 0xaf init window.
One exact 16-byte anchor preserves owner/vtable loads and false return while
NOPing only the six-byte virtual free call at 0x1da6c. Factory release remains
unchanged for every failed init, including base-init failure, so that the
factory performs the normal single final cleanup. Removing every factory
release instead would leak the base-init failure case and is not used.
Both payload fixtures and in-memory patch tests verify unique bounded match,
exact six changed bytes, unchanged surrounding payload and retained factory
release. Production source bounds/array use are checked. PF patch admission
is unchanged; this is installed within the classified VF branch. Boot sized
delete's callee resolves to _kfree_ext; its exploratory nearest-symbol 0x340
span includes unnamed helpers, not one allocator body. Kernel allocator
transitive review remains incomplete and is not a general memory-safety proof.
No deployment or dynamic fault injection; timer-owner/drain obligations remain.

Concrete deleting-destructor follow-up: complete TGL Scheduler4 D0 0x22
body reviewed/pinned, effective virtual+8 and both base-vtable/sized-delete
relocations verified for each payload. D0 calls its base destructor then
tail-calls OSObject::operator delete(object, 0x498). Complete Boot sized-delete
0x40 wrapper reviewed/pinned: null check, allocator call, allocation accounting
decrement and return. This strengthens the manual-free/factory-release concern;
allocator callee and exact failing-init caller contract still need review
before the repair boundary is declared complete. Object deletion is not a
callback drain or DMA barrier. No dynamic fault was intentionally induced.

Actual base-free follow-up: TGL IGScheduler::free's RIP pointer at +0xc81e0
has external relocation to __ZTV8OSObject (external 64-bit type 0x0e), not an
IOAccel scheduler cleanup vtable. Its raw vtable slot +0xa0 corresponds to
Boot OSObject::free at 0xffffff8000a1c830. Complete actual 0x30 wrapper reviewed
and locally pinned: obtains metaclass, conditionally accounts destruction,
then tail-dispatches object virtual +8 (deleting destructor). This is not just
field cleanup. The nearest-symbol 0x280 span includes additional unnamed
functions, and its truncated exploratory output is not counted as their
review. Concrete TGL deleting destructor/allocation release and actual
refcount path remain pending before concluding reachable use-after-free or
introducing a repair to partial-init virtual-free/factory-release behavior.

Scheduler4 complete-init follow-up: its actual init body is 0xaf bytes ending
at 0x1da80; the nearest named-symbol 0xc2 span also contains a separate unnamed
GuC thunk. Full init reviewed/pinned as the smaller window. It calls scheduler
base init first, clears +0x490 and creates per-engine command streamers from
the accelerator engine mask. Streamer failure invokes virtual +0x90, resolved
to the already-reviewed Scheduler4::free, before returning false. The factory
then releases the failed object. Actual inherited free/refcount behavior must
be resolved before certifying this manual-free/then-release combination;
reference XNU OSObject release/free can delete objects, but that is not proof
of this opaque native hierarchy's behavior. Do not infer double-free or safe
idempotence solely from field clearing. No GuC firmware allocation appears in
this init body; later firmware-init owns that boundary. Remaining ownership/
callback and inherited free review blocks runtime safety certification.

IGGuC construction classification follow-up: complete withAccelerator 0x48
factory reviewed/pinned; native scheduler create type-3 edge resolves directly
to it. Factory calls IGGuC initWithOptions; the reviewed initial base-init edge
calls IGScheduler::initWithOptions. Full IGGuC init remains unread. This is a
legacy scheduler construction path, not evidence of a shared VF helper object.
The VF final type-4 factory guard excludes this normal type-3 dispatch. An
exploratory direct-call scan found only create's type-3 factory edge, but this
does not exclude metaclass allocation or indirect calls. TLB routing alone
never proved actual VF IGGuC instantiation. Candidate H2G callers include
sleep/wake, bind/unbind, display stamps, idle waits and reset/control methods;
those caller bodies and alternate construction remain pending. Retain the
H2G MMIO inventory without attributing it to current VF execution or deleting
PF paths needed for the Gen11+ objective.

IGGuC producer-body follow-up: complete sendHostToGucMessage 0x122 and
ringDoorbell 0x12a bodies reviewed/pinned for both payloads, including their
direct DPSM kick edges. H2G calls kick before readiness/mutex handling; its
admitted branch force-wakes and directly writes accelerator MMIO+0xc180 and
+0x1901f0, then clears force-wake. RingDoorbell computes a mapped page address,
kicks DPSM and attempts a bounded doorbell update; it does not establish GPU
completion. Neither body can be classified as a harmless timer producer.
Current VF routes separately translate IGGuC::invalidateTLB, so the existence
of Scheduler4/IGHardwareGuC transport does not by itself exclude all IGGuC
objects. Allocation/caller/virtual reachability for these two methods remains
pending. Their full-body and raw-MMIO checks are inventory, not containment.
Do not deploy or infer current host-crash causation from this offline finding.

DPSM producer follow-up: production acceleratorStop sets final-stop intent
then delegates to native stop; that stop's finishAllStamps precedes the routed
engine-stop cancellation and GuC quiesce. No earlier quiesce was found in that
wrapper. Full Scheduler5 handleKickDPSMInterrupt 0xe body reviewed/pinned:
loads accelerator owner+0x10 and tail-calls dpsmKickTimer. Exploratory direct
branch candidates also occur in IGGuC sendHostToGucMessage/ringDoorbell,
Scheduler5 updateIdleState and display generateFlip; those containing bodies
and actual VF reachability are not yet fully reviewed. Thus cancel alone is
not producer exclusion, and disabling every callback without ownership/caller
evidence is not justified. Prioritize VF-reachable producer admission and
same-workloop detach before claiming timer/owner teardown safety.

DPSM cancellation restoration: VF stopGraphicsEngine now null-checks the
constructor-pinned IOTimerEventSource at accelerator+0x1460 and invokes its
cancelTimeout before the existing final GuC shutdown boundary. It does not
write +0x1458, call physical waitForGpuIdle, mutate PF behavior or assert a
completion/drain acknowledgement. Local source contracts pin null-check and
ordering. This restores a native software step omitted by the replacement,
but outstanding callback/rearm, owner lifetime and detach error handling are
still unresolved; cancellation alone cannot close those obligations. Native
stop's later disable/remove/release sequence remains unchanged. No deployment
or dynamic test is authorized until host-containment gates are satisfied.

DPSM provenance follow-up: complete dpsmIdleTimer (0x96), dpsmKickTimer
(0x7e) and dpsmIsIdle (0xe) bodies reviewed/pinned. Construction subsection
0x2463f..0x246a8 calls the imported default IOTimerEventSource factory with
dpsmIdleTimer, stores +0x1460, null-fails to 0x215, attaches through scheduler
getWorkLoop (unchecked add), enables/cancels and marks feature+0x1191 bit 2.
The callback serializes event/mutex work, queries scheduler idle and event
state, sets +0x1458 bit 0 when idle, otherwise rearms. Kick clears bit 0,
notifies and cancels/rearms; isIdle returns that bit. Consequently copying
native engine-stop's +0x1458=1 into VF would assert software idle rather than
merely set a neutral stopping flag. Do not fabricate that state. Timer cancel
is independently identifiable software work, but it is not a drain or admission
barrier. Construction-order evidence rules out the fresh-init factory-null
path having reached this later timer allocation; stale/reused object state
and whole-function failure paths remain pending. Next: restore justified
timer cancellation without false idle or physical waits and prove lifetime.

Free/engine-stop follow-up: complete IntelAccelerator free wrapper (0x24),
its separate unnamed helper (0xce), and native stopGraphicsEngine (0xc4)
reviewed/pinned. Free helper conditionally processes registry matches and a
service virtual, but has no scheduler+0x1250 release or workloop gate in its
own instructions. Helper/import transitive behavior and inherited free remain
pending; do not infer either an unconditional leak or a safe lifetime.
Native engine stop conditionally cancels timer+0x1460, stores 1 at +0x1458,
and calls scheduler virtual +0x170: both Scheduler4/5 tables identify this as
waitForGpuIdle. It then calls other native hardware/lifecycle helpers. Current
VF replacement does not perform those timer/state/wait steps. The timer and
field's full construction/use provenance remains pending, and waitForGpuIdle
is not certified safe for VF. Restoring this whole native engine-stop method
would bypass the physical-MMIO containment boundary and is not authorized by
these software findings. Next: timer1460 constructor/callback and state1458
consumers, then prove the minimal software teardown needed without PF MMIO.

Complete native stop follow-up: all 0x3e4 bytes of IntelAccelerator::stop
reviewed and locally pinned for both payloads. It finishes event-machine work,
serializes busy/mutex transitions, calls routed engine stop, then releases
software resources. Source +0x1460, when non-null, is disabled and removed
through scheduler+0x1250's workloop getter without checking scheduler/null
workloop or remove IOReturn. Reachability of that source with a null scheduler
still needs construction-order evidence; do not claim a proven crash.
Scheduler release/clear is conditional on native feature type 5, not type 4.
The inherited accelerator stop virtual is called only with non-null provider;
factory-failure stop(nullptr) skips it. Therefore the previously-reviewed
inherited stop/block gate serialization does not establish this failure path's
timer/source drain. The tail still invokes an imported helper; constructors,
remaining helpers and later free must establish partial-state lifetime safety.
No hardware or runtime behavior is certified by these body/branch checks.

Factory-null unwind follow-up: reviewed native start failure subsection
0x2473d..0x247ef as a window, not a standalone function. Null scheduler leads
to error 0x211, shared error/property handling, imported unlock_busy, imported
IOLockUnlock on accelerator+0x88, then virtual +0x5c8 with null provider.
Effective IntelAccelerator vtable maps that slot to its stop method. The
shared epilogue returns false after stack check; error 0x215 instead bypasses
this path, already separately tracked. Local fixtures pin the full window,
unlock import identities, null-provider dispatch and effective stop target.
This establishes failure-control flow for the new factory guard, not safe
partial-construction cleanup, owner/drain correctness or GPU quiescence.
The earlier exploratory 0xf0 range extended into a different initializer;
only the corrected 0xb3 failure subsection is claimed here.

Final factory admission repair: the VF-only route table now preserves the
typed IGScheduler::create trampoline and validates accelerator feature bits
23..25 immediately before delegation. Non-type-4 returns null after protocol
fault instead of dispatching into a PF scheduler factory. No force-write of
feature bits or synthetic scheduler is used; valid type 4 follows native
allocation/ownership. Both native creation calls (0x243f3/0x2448e) store the
result and branch on null to 0x2473d, now pinned in local payload fixtures.
Route inventory increases from 94 to 95; source contract pins validation before
delegation and typed route. This also covers native fallback calls that reach
this factory. It is not synchronization against arbitrary concurrent writes
to accelerator memory, proof of null-path teardown quiescence, or hardware
acceleration. Complete native start and failure-unwind review remain pending.

Late options admission repair: VF start now resolves IODeviceTree:/options,
copies/retains GraphicsSchedulerSelect for a stable type inspection, and
releases both copied property and retained registry entry before returning.
An OSData override causes protocol fault/false before GGTT bootstrap/native
start. Missing entry/property or non-OSData does not trigger this guard, matching
the observed native override type filter. PF skips the check entirely. No
global property is changed. All OSData values are rejected, even textual 4:
parser syntax/bounds are not yet established and this prevents relying on a
second selection authority. Remove the redundant options override for a VF
configuration; no options removal was performed here. Source contract checks
VF-only scope, retained-copy/release order, fault-before-bootstrap and absence
of options mutations. This admission snapshot does not exclude later external
property mutation; final scheduler validation/routing remains pending.

Late options override follow-up: complete utilGetProperty<unsigned int>
0x18c body reviewed/pinned for both payloads. It obtains the requested
registry property, distinguishes OSNumber/OSData using imported metaclass
identities, and only when missing checks the Development dictionary. After
that, it obtains IODeviceTree:/options and checks the same key there; OSData
is passed to imported OSNumber::withNumber(const char *, 32), and a successful
parse overwrites the earlier result before release/return. Thus accelerator
GraphicsSchedulerSelect publication is not the final authority. Local fixtures
pin full helper, path, overwrite and selected imported callees. Number/data
virtual getter bodies and string-parser bounds remain pending; no assumption
about NUL termination or accepted text syntax is justified yet. VF must also
exclude or validate this late override before native start. This is a separate
unresolved admission hole, not repaired by the firmware-disable guard. No
runtime options state was inspected or changed in this offline turn.

Firmware-disable scheduler override repair: reviewed native start subsection
0x27a68..0x27b18, separately from the still-incomplete full start review. It
loads GraphicsSchedulerSelect through an unreviewed property helper, accepts
3..5 into feature bits 23..25, then checks -disablegfxfirmware. A successful
boot-argument lookup unconditionally overwrites those bits with type 5. This
can defeat wrapper VF type-4 publication. VF start now rejects presence of
that argument with a protocol fault and false return before GGTT bootstrap,
MSI allocation or native start. PF short-circuits the guard and retains native
behavior. Local fixtures pin the reviewed native window/string and source
fault/return ordering; this prevents one proven override, not every possible
selection mutation or any existing DMA activity. Native lookup/import helper
semantics and complete upper teardown remain pending. No runtime deployment.

Scheduler selection/factory follow-up: native create has a separate 0x36
dispatcher window (nearest symbol also includes unnamed helpers). It reads
(accelerator+0x1190 >> 23) & 7 and tail-dispatches types 3/4/5, with invalid
types reaching a cold helper. Reviewed/pinned both complete 0x48 Scheduler4/5
factories: metaclass allocation, init call, and release/null on init failure.
Wrapper source sets VF selection to 4 after boot/property overrides and
requires GraphicsSchedulerSelect publication; PF defaults differ and may use
5. Property-to-feature-bit propagation remains pending, so this is source
intent plus native dispatch evidence, not a runtime-selected-type observation.
Scheduler5 teardown concerns remain relevant to the full PF/VF objective but
are not established as the current VF host-crash cause. Prioritize Scheduler4
owner/GuC teardown and the selection propagation while retaining PF review.

Scheduler5 init provenance follow-up: complete initWithAccelerator 0x17c
body reviewed/pinned. It calls base initWithOptions first, then imports base
IOWorkLoop::workLoop and stores private +0xa80; creates a provider-null
IOInterruptEventSource +0xcd8 and adds/enables it on that private workloop
(add return unchecked), allocates storage and creates engine objects. There
is no base timer +0x448 access/rebinding in this body. Post-base failures call
virtual +0x90, whose effective Scheduler5 vtable entry is the reviewed derived
free. Factory and allocation import relocations are pinned for both payloads.
This closes the proposed init-local timer-rebinding escape, but not helper/
engine/upper-level detach or producer admission. The currently selected VF
scheduler and partial-construction owner lifetime still need caller analysis.

Derived scheduler destruction follow-up: complete Scheduler4 free (0xa2)
and Scheduler5 free (0x126) bodies are reviewed/pinned for both payloads.
Their RIP-relative base vtable references resolve to base IGScheduler::free
at slot +0xa0. Scheduler4 releases GuC +0x488 and per-engine objects before
base timer cleanup. Scheduler5 removes private sources without checking
IOReturn, releases engine/storage objects, then releases and clears private
workloop +0xa80 before delegating to base free. Since its already-reviewed
effective getWorkLoop returns +0xa80, inherited cleanup's getter then returns
null and skips timer removal on that path. Base init attaches its timer through
the accelerator getter, not this private getter. Whether construction/rebinding
or higher-level stop previously detaches that timer remains unreviewed; do not
declare a reachable UAF or apply a workaround without that ownership evidence.
This invalidates a blanket claim that every derived destructor reaches a
successful same-workloop timer removal. Next: derived init/stop caller topology
and registration/rebinding provenance, while preserving Gen11+ PF/VF scope.

Scheduler cleanup ordering follow-up: re-read native shared cleanup/free and
periodic callback against the now-reviewed timer semantics. Cleanup cancels,
calls scheduler workloop getter twice, attempts removeEventSource, then loads
the timer for release without inspecting IOReturn; subsequently it clears the
timer/set and frees the callback's mutex. Local payload fixtures now explicitly
pin these instructions for both shipped payload variants. Successful removal
on the same stable workloop can provide gate serialization and timer disable/
generation invalidation; this cannot be extended to a missing workloop or
failed removal. The periodic callback accesses owner+0x440/0x438/0x448 and may
rearm the timer. No reachable failure is proven here; concrete ownership,
workloop stability, cleanup admission and failed-detach containment remain
required before introducing a runtime patch. Engine-stop ordering fixture
messages now describe call order rather than falsely claiming DMA quiescence.

Cancellation-wait and callback owner follow-up: the complete Boot
thread_call_cancel_wait 0x3d0 body is now reviewed and locally pinned. It
checks allocation ownership, enabled interrupts and self-call avoidance;
after locked cancellation, flag 0x20 takes a separate wait helper (still
unreviewed). Without that flag, a true cancellation returns without waiting;
false cancellation snapshots call+0x70 and waits until call+0x78 reaches it,
releasing/reacquiring the group lock around the wait. This is a fixed snapshot,
not an admission barrier against future submissions. Wait-queue/block callees
remain unreviewed, and no GPU DMA cessation follows from these CPU counters.

The passive callback's unnamed invocation helper at 0xffffff8000ad01b0 was
read completely in a separate 0x160 window, including tracing and block-action
branches. Direct action calls receive owner from source+0x18 and source itself;
there is no explicit owner retain/release in this helper. Existing source and
workloop references are not evidence that scheduler owner/its mutex survives.
Local fixture pins this helper, the callback call edge and owner argument.
Next: actual scheduler teardown/admission ordering and unchecked detach
failure handling, plus outstanding kernel wait helpers. Runtime hold remains.

Kernel cancellation boundary follow-up: the actual Boot thread_call_cancel
symbol's nearest-symbol span is 0x310, but it contains the public wrapper
(0x120) followed by an unnamed locked helper (0x1f0). The wrapper was read
completely: validates call/group metadata, disables interrupts and takes the
group ticket lock, calls the helper at `0xffffff80003bf320`, restores lock/
interrupt state and returns the helper's boolean. There is no wait invocation
in this wrapper. A subsequent separate, untruncated disassembly covered the
entire 0x1f0 helper window: flag 0x40 is cleared with true returned; otherwise
the dequeue helper's non-null result determines the boolean, updates pending
accounting, and may cancel/reprogram the group's earliest delayed timer.
No direct callback-completion wait appears in this helper. Its external
dequeue/timer callees remain pending; this is not a transitive drain proof.
Both windows and the wrapper-to-helper edge are pinned in the local fixture.
The later cancellation-wait review above supersedes its earlier pending status. Do not
substitute nearest-symbol ranges or reference XNU semantics for those missing
reviews; runtime hold is unchanged.

Timer schedule-time retention follow-up: complete Boot wakeAtTime(options,
deadline, leeway) 0x130-byte body is locally pinned. With action, enabled state,
nonzero deadline/workloop and expansion storage, passive mode retains the
source and workloop, stores expansion workloop and increments generation,
then calls the scheduling helper. If that helper returns nonzero, passive mode
releases the newly acquired pair; otherwise callback/cancel paths account for
them. These references protect source/workloop, not independently the stored
owner. Missing action fails; missing calloutEntry can panic. Kernel helper's
exact identity/return semantics, concurrency interleavings and complete start
still need verification. Selected XNU wakeAtTime and cancel methods were read
as references, not proof that unreviewed Boot helpers behave identically.

Timer detach follow-up: complete base setWorkLoop (0x60) and disable (0x60)
bodies/virtuals are locally pinned. A null workloop first invokes disable,
then stores null at +0x30. Disable increments generation, conditionally calls
cancel versus cancel-wait using the active flag, clears enabled, and releases
passive source/workloop references only when cancellation reports success.
Non-null attachment may rearm a pending enabled deadline through virtual +0x210.
Combined with successful gated maintenance removal, this gives the generation-
invalidation order before unlink/release. It does not establish schedule-time
retains, thread-call cancellation-return semantics, all generation interleavings
or owner lifetime, and failed removal bypasses this detach. Those obligations
remain open before certifying teardown; no runtime hold is relaxed.

Stop block execution follow-up: paired import `0x1051c` resolves to
IOWorkLoop::runActionBlock. Complete wrapper (0x40), separate unnamed block
invoke adapter (0x10) and effective base runAction body (0x60) were read/pinned.
The wrapper delegates through virtual +0x1a0; base runAction enters workloop
+0x180, synchronously calls the adapter/action, exits +0x178 and returns its
result. The adapter invokes the block's +0x10 function pointer. Thus the reviewed
stop block normally runs under the same base workloop gate as passive callbacks.
This proves synchronous serialization, not source-removal success: the block
ignores twelve removal returns and always returns zero. Owner lifetime,
cancel/generation races and complete start remain pending; no runtime hold
or DMA-quiescence requirement is relaxed.

Inherited stop follow-up: complete accelerator stop (0x43f) and its captured-
owner block (0x2d7) were read/pinned locally. Stop releases the accelerator
mutex before calling an imported block-execution helper with workloop +0xf0,
then reacquires the mutex for further cleanup and later releases/clears +0xf0.
The block contains twelve conditional source-removal calls, releasing/clearing
each source afterward without checking removal return. Four source paths
first call disable virtual +0x158. The block returns zero even if removal
failed. No direct MMIO occurs in the two bodies, but called virtuals/helpers
remain separate review obligations. Exact block-execution API/gate semantics,
event-machine pre-stop virtuals, related helper bodies and full start remain
pending. The observed unchecked failures are not repaired by a passing hash
contract, and no callback or GPU quiescence is certified.

Actual accelerator workloop factory follow-up: complete Boot workLoop factory
(0xb0 bytes) was read and locally pinned. It allocates an object, initializes
reference count to 1, installs the base IOWorkLoop vtable, updates metaclass
accounting, then invokes base effective init virtual `+0x88`. If init fails,
it releases the object and returns null; success returns the initial reference.
The factory vtable LEA, init dispatch and failure release/null instructions
are checked. Combined with the paired import/store and actual getter evidence,
this proves the reviewed start construction site uses a base workloop, not a
guessed subclass. It does not prove no later field replacement, successful
attachment/removal or safe owner teardown. Complete accelerator start/stop and
stop-block control flow remain pending before applying this to lifetime safety.

Accelerator workloop construction candidate: typed disassembly of inherited
start identifies call `0x14ba02c7` through System KC import `0x10138`, followed
by storing its result at accelerator `+0xf0` and a null-result failure branch.
The paired Boot import resolves to `IOWorkLoop::workLoop`; import identity,
direct edge and field store are now pinned. A separate earlier +0xf0 store
appears in initialization and stop contains release/clear of the field; those
observations are not a complete lifetime proof. Full start (0x17ae), stop
(0x43f), stop block (0x2d7) and actual factory body remain next review targets.
No whole-function review, successful detach or owner lifetime is claimed from
these selected windows. Candidate disassembly windows must start at verified
instruction boundaries, not arbitrary preceding bytes.

Concrete Intel workloop getter follow-up: both payloads pin four complete
getter bodies and three effective virtuals. Scheduler4 `+0x218` uses inherited
IGScheduler getter, which dispatches owner accelerator virtual `+0x688`;
Intel's effective target returns accelerator `+0xf0`. Scheduler5 overrides
the scheduler getter to return its own `+0xa80`; IGGuC returns its own `+0x458`.
Thus Scheduler4 stamp timer cleanup targets the accelerator workloop, not
automatically the GuC completion workloop or Scheduler5 private workloop.
Different fields do not prove different live pointer values, but their
provenance cannot be substituted. Actual accelerator `+0xf0` construction,
effective workloop type and release order are the next lifetime gates.

### Current timer teardown review boundary

The follow-ups below are chronological findings; older "pending" statements
are superseded only by subsequent explicit evidence. Current base-chain
evidence: workloop init references command-gate gMetaClass; the reviewed
initializer writes its metaclass vtable; allocator installs the base object
vtable; inherited init stores owner/action; runCommand delegates to gated
runAction; successful maintenance removal detaches/releases the source.
This is not a successful acceleration or complete teardown baseline.

Remaining driver-relevant gates, in priority order:

1. Resolve Intel accelerator's actual workloop construction/getter and effective
   overrides, scheduler ownership and callback owner lifetime. Base kernel
   vtables do not establish these driver objects.
2. Review schedule-time retains, generation/cancel races, unnamed action helper,
   and source setWorkLoop(NULL) against the same actual gate.
3. Design verified handling for failed native add/remove requests: scheduler
   ignores those results. Never free owner/mutex while callbacks can dispatch,
   or wait for cancellation under a gate needed by the callback. Do not add
   runtime patches based on guessed object types.
4. Prove normal GPU-written completion and backing reclamation independently
   of termination-forged CPU stamps; finish MMIO/DMA lifecycle review and all
   independent host containment requirements before any VM run.

Local proprietary KC tests pin identities/selected instructions; remote CI
only syntax-checks that fixture. Neither proves concurrency safety, callback
drain or PF/DMA quiescence. Generic metaclass runtime registration is still
unverified but must not substitute for the driver-specific gates above.

Metaclass initializer follow-up: the complete unnamed initializer immediately
after the separately bounded allocator was read and pinned as a 0x70-byte
window. It passes gMetaClass/name/superclass/size 0x50 to the metaclass
constructor, conditionally creates allocation metadata, then unconditionally
writes the declared IOCommandGate metaclass vtable address point into
gMetaClass. Exact RIP-relative vtable address and destination are checked.
This explains the on-disk zero vptr; it does not prove execution/registration
of the initializer or driver-specific workloop construction. Generic metaclass
constructor/registration semantics and concrete driver lifecycle remain open.
Runtime hold and unchecked removal-failure concern are unchanged.

Control-gate metaclass reference follow-up: workloop init's RIP-relative load
resolves to `IOCommandGate::gMetaClass`; the declared metaclass vtable `+0x88`
resolves to the reviewed base allocator. Both identities are locally pinned.
The on-disk gMetaClass vptr is zero runtime-initialized storage, not a null
runtime allocation target, nor a System KC chained pointer. Do not follow it
as a canonical pointer in an offline image. Metaclass static initialization
must still be reviewed before claiming a complete constructed-object chain.
Concrete driver workloop selection, preexisting gates and unchecked removal
return also remain open; no callback-drain or host-safety proof is claimed.

Inherited owner/action storage follow-up: complete Boot IOEventSource init
(0x70) and setAction (0x50) bodies are locally pinned. Init rejects null owner,
stores the owner pointer at `+0x18` without an explicit retain, delegates action
to virtual `+0x140`, enables the source and initializes expansion/statistics.
The effective base IOCommandGate action setter is inherited setAction: it
releases an existing block action when applicable, writes action `+0x20` and
clears the block-action flag. This closes base control-gate action-storage
delegation but does not independently pin owner lifetime. Workloop initializer
metaclass pointer identity, concrete driver workloop and removal failure
handling remain open; no runtime safety or acceleration claim follows.

Command gate allocation/init follow-up: complete Boot command-gate init and
metaclass allocator were read. Init delegates owner/action to inherited
IOEventSource init and adds optional statistics; effective base virtual
`+0x1b8` and inherited call are pinned. Allocator installs base IOCommandGate
vtable and initializes references/metaclass accounting; its 0x80-byte window
is separately pinned because the nearest symbol also contains an unrelated
unnamed initializer. The exact vtable-address LEA is checked. Actual workloop
initializer metaclass pointer resolution and inherited owner/action storage
remain pending; this does not prove all preexisting gates or driver workloops
use the base type, nor does it remedy unchecked scheduler removal failure.

Base workloop initializer follow-up: complete Boot init (0x1a0 bytes) and
base init virtual are locally pinned. It obtains maintenance action through
workloop virtual `+0x118` (base target `_maintRequest`), allocates a control gate
through a metaclass virtual, passes workloop owner/action to its init, stores
the gate at `+0x20`, bootstraps its workloop and checks addEventSource return.
The binding instructions and base maintenance target are pinned; allocation/
init/attachment/thread-start failures make init fail. Existing non-null control
gate state bypasses this binding branch. Metaclass identity, concrete driver
workloop override selection and preexisting state remain unproven, so this is
base construction evidence, not proof that every cleanup removes its timer.
Native scheduler ignores removal failure; runtime hold remains unchanged.

Maintenance action follow-up: complete Boot `_maintRequest` (0x290 bytes),
including add and both active/passive remove branches, is locally pinned.
Opcode 1 checks source workloop presence, selects the chain based on mode/
checkForWork topology, finds/unlinks the source, calls source `setWorkLoop(NULL)`
virtual `+0x128`, clears next virtual `+0x130`, releases the chain reference and
marks loop restart. A source absent from its selected chain yields BadArgument
without this detach sequence; no attached workloop yields success without
removal. The complete XNU reference method was read too. Binding of this action
in the actual workloop/control-gate initializer still requires proof, as do
concrete overrides and caller removal-return handling. The already reviewed
native scheduler cleanup does not check removal's return before releasing its
own timer/set/mutex, so unconditional drain-before-free remains unproven.
No runtime gate is relaxed by the successful-path detach order.

Actual command action follow-up: complete Boot base IOCommandGate::runAction
(0x280 bytes) and virtual `+0x1c8` are now locally pinned. It rejects null action
or missing workloop, enters that workloop's `+0x180` gate, checks workloop-thread
identity, and for other threads waits on disabled command state through sleep
virtual `+0x190`. It accounts active actions around the stored action call,
handles teardown/wakeup abort branches, and exits through gate `+0x178`.
The local fixture pins full body plus entry/action/exit/sleep instructions.
This establishes the base command path's gated action invocation, not success
of every removal request: missing/disabled/tearing-down gates can fail or wait.
Concrete control-gate/workloop types, removal action, return handling and
callback retention still require review before claiming drain-before-free.
No runtime hold or DMA-safety condition was relaxed.

Control-gate command wrapper follow-up: complete Boot base IOCommandGate
`runCommand` (0x30 bytes) and virtual `+0x1c0` are locally pinned. It shifts
the four arguments, loads stored action `+0x20` and tail-delegates through
virtual `+0x1c8`; it does not itself enter the workloop gate. The local XNU
runAction method was read completely, including disabled-gate sleep/teardown
abort and active-action accounting. That reference cannot certify the actual
Boot runAction virtual or workloop removal action; these remain the next
required edges for callback-drain analysis. No runtime hold change.

Workloop gate follow-up: complete Boot base closeGate/openGate/removeEventSource
bodies and effective base virtuals are locally pinned. CloseGate uses workloop
`+0x10` recursive gate storage: same current-thread owner increments recursion;
otherwise it locks the mutex, records owner and initializes count. OpenGate
decrements count and only clears owner/unlocks on the last recursion. Neither
method proves a concrete driver workloop has no override. RemoveEventSource
does not itself acquire that mutex in its body; it delegates to control gate
`+0x20` virtual `+0x1c0` with removal opcode 1. That command-gate method and
removal action still require review before linking cleanup to callback drain.
Exact mutex call/tail targets are checked locally; no VM/runtime gate changes.

Passive timer callback follow-up: complete Boot `timeoutAndRelease` (0x120
bytes) now has a local body contract. It checks initial enabled/action state,
obtains the expansion-stored workloop, enters its virtual `+0x180`, rereads
action and checks the passed generation against expansion generation before
calling an unnamed action helper at `0xffffff8000ad01b0`. It leaves through
workloop virtual `+0x178` and releases expansion-stored workloop/source refs.
The body/hash and generation/gate/release instructions are pinned. There is
no second enabled check after gate entry in this body; generation is the
post-gate guard. Actual gate implementations, the unnamed action helper,
schedule-time retains and removal synchronization remain unreviewed. A callback
already past the generation check is not proven drained by incrementing the
generation; no teardown or DMA-quiescence claim is made.

Timer init delegation is now resolved locally: base timer virtual `+0x1c0`
is `init(owner, action)`. Its complete 0x50-byte Boot body calls inherited
IOEventSource init, fails if that fails, dispatches `setTimeoutFunc` through
base timer virtual `+0x1b8`, then requires non-null calloutEntry `+0x48` before
success. The paired fixture pins the full body, both base virtual identities,
inherited init call target and setup dispatch instruction. This closes the
middle timer options-init/setup delegation gap, but not inherited init internals,
callback drain, thread-call execution or workloop removal. Existing passive-mode
setup evidence therefore remains a mode-selection finding, not a teardown
safety or hardware-completion guarantee. No runtime hold was relaxed.

Actual Boot timer factory follow-up: complete default factory and options-init
bodies, plus setTimeoutFunc code and its five-entry trailing jump table, were
read. The factory installs the base timer vtable and passes options 1 through
virtual `+0x220`; options-init stores them in `+0x50` and delegates through
`+0x1c0`. Setup option 1 selects the table's second entry and ORs flag value 1
(passive), whereas the workloop-priority case sets value 2 (active/cancel-wait).
The local paired fixture pins these three windows, all five table targets,
factory option and init virtual identity. Do not decode jump-table bytes as
instructions. The middle init virtual and setup dispatch, callback retention/
generation and workloop removal gates remain pending before certifying the
complete actual factory-to-teardown chain. No unconditional drain guarantee
or runtime authorization follows from the passive-mode observation.

Tahoe timer cancel follow-up: the base timer virtual `+0x218` resolves to
`cancelTimeout`. Its complete 0x70-byte Boot KC body increments generation when
expansion storage exists, chooses `thread_call_cancel_wait` only when object
flags byte `+0x2a` bit 1 is set, otherwise uses `thread_call_cancel`, clears
deadline `+0x50`, and conditionally releases passive-mode source/workloop
references. The paired local fixture pins the vtable, full body, active-mode
selector and both exact branch targets. Cancellation is therefore not an
unconditional callback-drain guarantee. Actual factory/init flag selection,
thread-call semantics and workloop removal synchronization remain pending;
the VM hold is unchanged. Neither a cancellation return nor software generation
increment is proof of GPU DMA quiescence.

Scheduler timer lifetime follow-up: complete native init (0x162 bytes), unnamed
cleanup helper (0xe0) and free (0x24) were read and separately pinned in both
payloads. Nearest-symbol ranges merge the helper into init and an unrelated
factory into free, so they are not treated as single function bodies. Init
creates OSSet capacity 2, obtains the default IOTimerEventSource factory with
the scheduler owner and periodic callback, attaches it through workloop
virtual `+0x140`, then allocates mutex `+0x440`. Factory/mutex allocation failures
share the cleanup helper; the workloop attachment return is not checked here.
Cleanup calls timer virtual `+0x218`, conditionally removes it through scheduler
workloop getter `+0x218` and workloop remove `+0x148`, releases/clears timer,
releases/clears collection, frees/clears mutex, then releases auxiliary source
array members. Free uses the same helper before inherited free. Five factory/
allocation imports and both cleanup caller edges are pinned. No direct MMIO
occurs in these bodies. Timer virtual `+0x218` semantics, factory concrete type,
workloop removal synchronization, allocation/attachment failure behavior and
callback drain-before-mutex-free remain open; cleanup order alone is not proof
of DMA or callback quiescence.

Paired Tahoe Boot KC event-source follow-up: the base IOInterruptEventSource
virtual `+0x1e0` is `normalInterruptOccurred`, not the driver's action callback.
Its complete 0x140-byte body increments software producerCount at `+0x54`,
updates optional statistics/tracing, then notifies workloop `+0x30` through
virtual `+0x170`; it does not directly invoke the stored action. Thus the
scheduler's locked virtual dispatch must not be conflated with synchronous
stamp-action execution. Workloop dispatch and concrete source overrides remain
unreviewed. Base IOTimerEventSource `+0x1d8` is `setTimeoutUS`; its complete
0x20-byte body delegates to virtual `+0x1e0` with scale 1000, so a base timer's
argument 1000 requests 1000 microseconds, not 1000 milliseconds. Timer concrete
type/overrides and deadline/cancel semantics remain open. The paired local
fixture pins both base vtable targets, full-body hashes and pending-count/
workloop-notification instructions. Boot vtables contain canonical pointers;
they are not decoded as System KC chained cache-level targets. Remote CI only
syntax-checks this proprietary-KC fixture; the paired KC run is local evidence.

Stamp event-source construction and callback follow-up: complete Intel
event-machine init/free/callback and `IntelAccelerator::signalStampUpdate`
bodies are now pinned in both payloads. Init clears software accounting,
delegates to inherited init and allocates `IOInterruptEventSource` at `+0xd30`
with the native callback, null provider and index zero; factory failure makes
init fail. Free conditionally removes the source from the workloop when
`+0xd88` is set, releases it and clears the pointer before inherited free.
The callback tail-calls signalStampUpdate with the `+0xd40` channel mask.
That method obtains task `+0x150` CPU stamps, compares each selected 64-byte
slot's first dword against accelerator `+0x1478` cached channel values, updates
the cache, calls event-machine virtual `+0x228`, and only signals inherited
`signalStampsUpdated` if at least one changed. It also updates software tracing.
The factory, tracing/time and inherited notification imports are pinned.
No direct MMIO or GuC submission occurs in these reviewed bodies. This is
CPU-observed change notification, not authenticated GPU completion: the
termination path can fabricate stamp values. Source enable/workloop attachment,
actual source virtual `+0x1e0`, inherited wakeup and drain-before-free still
require review; no callback-lifetime or host-safety claim follows from hashes.

Periodic timer import follow-up: both TGL payloads now pin all eight mutex
lock/unlock call relocations in the callback and timer enable/disable paths,
the `OSCollectionIterator::withCollection` factory call, and all four singular
bridge descriptor spin-lock calls. The bridge uses `lck_spin_lock/unlock`, not
`IOLock`; do not introduce sleeping work into that critical section.
The local XNU 12377.121.6 `OSCollectionIterator.cpp` was read in full: its
factory retains the collection, free releases it, iteration checks the
collection update stamp, and `getNextObject` returns the collection's object
without an additional retain. Allocation/init can fail. This source is a
reference, not proof that the Tahoe Boot KC implementation is identical.
Collection retention is not an independent callback-object lifetime guarantee;
The complete native callback checks a null factory result at `0x566ad` and
skips iteration to the count/rearm path; it releases a non-null iterator before
rearm. The collection mutex remains held across each event-source virtual
`+0x1e0` dispatch: mutation/reentrancy and the actual virtual target remain
open. No runtime route or safety gate was relaxed.

Normal-submit stamp provenance follow-up: `submitToRing` captures ring byte
`+0x48` (stamp written), clears that byte before scheduler dispatch, and passes
the saved boolean to Scheduler4 virtual `+0x148`; its tail argument is ring
`+0x64`, with a separate value at `+0x68`. The virtual is Scheduler4::push.
That push obtains the FIFO at context `+0xb8`, its ring at FIFO `+0x130`, and
loads ring `+0x44` into the GuC call's ringSequence argument. The original
tail is saved and passed as the final stack argument. The saved per-submit
stamp-presence boolean is not forwarded to submitWorkItem. Both payloads now
pin this chain, fields, virtual and capture/clear/dispatch order.

Therefore ringSequence is the last ring stamp value, not proof that every
submit carries a fresh completion packet. A stamp-only idle implementation
could incorrectly declare later un-stamped work complete using an older stamp.
Future tracking must capture stamp coverage at a verified producer boundary
(the Scheduler4 push still receives the boolean), invalidate coverage for
subsequent un-stamped submissions, and exclude software termination/restart
writers before observing hardware completion. The current GuC wrapper's tail
publication and conservative enabled-busy behavior remain unchanged. This
review does not yet implement coverage tracking or prove mapping flag/coherency.

Producer ownership follow-up: context init passes its newly created ring to
FIFO::withOptions, which calls FIFO::initWithOptions. The FIFO stores and
retains that ring at `+0x130`; FIFO::free releases and clears the same member.
Both payloads now pin the factory graph and retain/final-release instructions.
Scheduler4 push itself only has the one GuC submit call, then returns its
boolean result; its original body ignores the incoming stamp-presence booleans.
Any producer coverage bridge must preserve native argument/result semantics
and associate metadata with the exact descriptor, stamp, tail and invocation.
A global temporary flag would race concurrent producers; holding the GuC queue
lock across a call to the native push would recursively enter that same lock.
Neither approach is an acceptable implementation. Active object lifetime,
per-invocation metadata and publication/failure ordering remain design gates
before enabling any stamp-based idle/reclamation behavior.

The freestanding `NGVfSubmissionCoverage::Tracker` now models these metadata
requirements, without a runtime route. A nonzero owner and monotonic 64-bit
token identify one active invocation; claim/publish require exact owner,
token, stamp and tail. Duplicate claim/publication and overlapping begin are
rejected; token exhaustion refuses rather than wraps. Marker coverage is
hidden while a producer is active, established only on publication carrying
a stamp, and invalidated by a later un-stamped or unknown writer. Interference
during the invocation taints it so a later publish cannot restore coverage.
An accepted-result/publication mismatch invalidates coverage at finish.

Offline tests compare accepted/rejected/marked/unmarked/interfering cases with
an independent boolean oracle, plus identity mismatches, duplicate operations,
stale tokens, late invalidation and stamp/token boundaries. This is only
metadata policy: production synchronization, object lifetime, hook invocation
association and actual publication still require integration and verification.
`hasMarkerCoverage` must never be used alone as GPU idle or DMA-stop evidence.

Reuse follow-up adds `resetForReuse`: it invalidates coverage, refuses while
an invocation is active (tainting that invocation), and clears metadata only
after the owner is gone. It deliberately preserves the monotonically increasing
serial, including its exhausted value. Tests reuse a slot 1,024 times with the
same owner/stamp/tail and reject every prior token against a subsequent active
invocation; active reset and exhausted reset cannot restore marker coverage.
Production integration must use this reset rather than memset/reconstructing
the tracker. This only addresses reuse within the same tracker lifetime; table
replacement and stale callers still require independently proven shutdown and
object-lifetime synchronization. There is still no runtime coverage hook.

Foreign-invocation follow-up tests wrong/zero owners and stale/zero tokens
before claim, after claim and after publication. Every rejection must leave
all live metadata unchanged; an unknown writer before claim, before publication
or after publication must still prevent marker coverage at finish. The oracle
does not use prior coverage to excuse a newly published un-stamped submission.
Tokens are local to one tracker, not globally unique context identities: the
production bridge must select and pin the exact context before token validation.

Allocation/lifetime integration remains unresolved rather than silently patched:
`vfInitContextBridge` allocates the plain context table with `IOMallocZero`,
while `Tracker` currently has default member initializers. Adding it as a member
requires an explicit C++ object-initialization/lifetime strategy. Retired backing
release clears identity under the context lock, then releases objects outside
that lock. Tracker reset must occur before a slot becomes reusable and must
refuse an active invocation; table teardown must drain every producer bridge,
not merely rely on the context lock or numeric token. The existing operation
gate protects admitted operation lifetime, not concurrent producer serialization.
No runtime member, hook, idle predicate or teardown behavior changed here.

Native producer failure follow-up: the complete symbol-bounded Scheduler4 push
returns the GuC boolean unchanged. At `submitToRing` (`0x432e8..0x434a2`),
the scheduler dispatch at `0x4347e` tests AL and a false result branches to
`0x4349c`, which calls `submitToRing.cold.1` at `0x90a84`. That cold function
has a verified external PC-relative `_panic` relocation at `0x90a92`, with
the string `Work queue failure detected` and native source line 1762. Only
success clears ring `+0x4c` and `+0x6c`; stamp-presence `+0x48` was already
cleared before dispatch in either case. Both payloads pin the result branch,
success-only resets, exact cold body, panic relocation and diagnostic string.

Thus a false submit is not a retry-capable admission failure at this producer.
A future coverage bridge cannot casually return false for overlap or metadata
mismatch and claim normal recovery; doing so inherits a guest panic. Returning
true without real publication is equally invalid. Native producer serialization
and an explicitly justified error policy must be proved before implementing
the hook. Existing bootstrap/transport rejection paths also remain possible
native panic triggers; this discovery does not authorize enabling runtime.

Producer receiver follow-up: all six concrete ring vtables (base, Main,
Compute, Blit, Media and VEBox) resolve virtual `+0x138` to the same
`submitToRing`. Typed receiver chains now pin FIFO `submitStampCommand`,
`submitRingCommands` and `submitBuffer` through their owned ring `+0x130`,
plus accelerator `submitSyncEvents` and `submitMainRingCommand`. The latter
tail-dispatches through task -> context `+0xb8` -> FIFO `+0x130` -> ring.
`submitSyncEvents` obtains the FIFO ring, writes three buffers, dispatches
the ring, then calls FIFO `submitStampCommand`; a separate control-flow path
also calls that stamp method. Both direct calls are pinned. A control-packet
submission and its subsequent stamp submission therefore cannot be collapsed
into one presumed completion transaction. These are reviewed typed edges,
not a complete inventory of all virtual callers or proof of caller locking.

FIFO virtuals `+0x138`, `+0x140` and `+0x148` have external 64-bit relocation
identities `IOAccelChannel2::mergeEventExcluding`, `setEventStamp` and
`incrementStamp`, respectively; the on-disk slots are zero before linking.
Their names/import identities do not prove locking of native ring state.
Selected FIFO producer bodies themselves contain no explicit surrounding
producer lock; callers and inherited event-machine methods still need review.
The direct queue lock only begins in the GuC replacement, after the native
producer has captured/cleared its stamp flag, so that lock alone does not prove
safe concurrent capture at the Scheduler4 boundary. No runtime route was added.

Inherited producer stamp follow-up (archived 25G229 SystemKC): the three
`IOAccelChannel2` methods are short tail wrappers through accelerator `+0x380`
to event-machine virtuals `+0x1c8`, `+0x1d0`, `+0x1d8`. Their complete bodies
and Fast2 vtable targets are now locally pinned. Fast2 `incrementStamp(int)`
reads channel software stamp `event+0xfc+channel*0x18`, computes the next
32-bit value, calls accelerator virtual `+0x8f0` when `(old ^ next) >=
0x40000000`, stores the new software stamp, then increments accelerator
`+0xa0`. `writeStampCommand` reads that same software value and tail-dispatches
to virtual `+0x2a0`; it does not inspect the mapped GPU-completed stamp.
These two complete bodies are locally pinned, without asserting downstream
packet handling or synchronization is finished.

Both Intel payloads resolve accelerator virtual `+0x8f0` via an external
64-bit relocation to inherited `IOGraphicsAccelerator2::scrubEvents()`.
That complete KC body traverses shared list `+0xa88` and resource list
`+0xb00`, invoking per-object virtuals `+0x128` and `+0x228`. Those callbacks
and iterator lifetime/locking are still separate review obligations; the
scrub call is not a GPU idle or producer-lock proof. Fast2 `setEventStamp`
and `mergeEventExcluding` update packed event metadata and cached completed
stamp `+0xf8`, read mapped stamps through `+0x28`, and can delegate to wait/
restart methods when event slots are full. Complete body hashes now pin both
reviewed functions locally; their callbacks and restart/termination bypasses
remain open. No runtime coverage hook, completion predicate or driver behavior
changed. The KC fixture stays local; CI only checks its Python syntax and the
payload import identities, not the absent KC content.

Scrub callback follow-up: archived Shared2 virtual `+0x128` resolves to its
resource-list scrub; Resource2 virtual `+0x228` resolves to its scrub, which
first visits a distinct storage resource if present, then scrubs optional
`+0x90` event pairs, four events in backing `+0x38`, and events in optional
memory objects `+0x80`/`+0x88`. Memory scrub walks its mapping list and invokes
event-machine virtual `+0x270` on each mapping's event `+0x38`. Fast2 `+0x270`
resolves to `scrubEvent`, not finish/wait. Complete symbol-bounded body hashes
and these three effective base virtuals are now locally pinned. Concrete
Intel Shared/Resource/EventMachine vtables also resolve these slots through
the exact inherited imports in both payloads, now CI-pinned. Further resource
subclass override and storage-resource lifetime coverage is still incomplete.

Fast2 scrub iterates eight packed event entries. It clears an entry if the
signed requested-minus-cached stamp is nonpositive, or if a mapped completed
stamp read makes it nonpositive. **It also clears an outstanding entry when
accelerator termination counter `+0xdc8` is nonzero, regardless of that read
still being behind the requested stamp.** It can therefore return true with
no hardware completion proof; only unresolved entries on a nonterminated
accelerator make it return false. This additional software completion bypass
is pinned explicitly and must be excluded from future idle/reclamation proofs.
Scrub modifies event metadata/cache, not the GPU-completed dword directly.

The two relevant list iterator constructors and next/previous methods were
also read in full: they copy a head/tail pointer and advance through shared
`+0x10` or resource `+0x50`. Their complete local byte contracts prove these
methods do not acquire a lock or retain the returned object. Consequently the
scrub iterator does not itself pin nodes against concurrent removal; upstream
serialization remains required. No runtime hook, release policy, GPU test,
VM configuration or host GPU state changed in this review.

Producer lock-name follow-up: complete archived `lock_busy` only increments
accelerator counter `+0x158` via `OSIncrementAtomic`; `unlock_busy` decrements
it via `OSDecrementAtomic` and can tail-dispatch notification virtual `+0x738`
on transition from one. Neither counter operation is mutual exclusion.
`acceleratorDidLock`/`acceleratorWillUnlock` conditionally obtain the registry
entry ID and emit `kernel_debug` records, not lock acquisition/release. Their
complete bodies and paired BootKC import identities are now locally pinned.
The decrement implementation is verified `lock xadd -1` returning the old value.

Actual local copies of `acceleratorLock`/`acceleratorUnlock` were found and
read: lock increments waiter counter `+0x90`, locks the object pointer `+0x88`,
decrements the waiter counter, marks busy and dispatches virtual `+0x850`;
unlock dispatches `+0x858`, clears busy and unlocks `+0x88`. SystemKC stubs
`0x10012`/`0x10018` resolve through BootKC to `IOLockLock`/`IOLockUnlock`
(also aliased to kernel mutex functions). These imports are pinned locally.
All four lock and five unlock local copies are now individually body-hash
contracted, with exact symbol-copy inventory rather than arbitrarily selecting
one duplicate symbol. Producer-to-lock caller reachability remains unfinished.
An exploratory symbol-bounded direct-call scan found unlock/relock edges in
Context2 `getDataBuffer`, GLContext2 `read_buffer` and Surface `surface_read`,
plus SharedUserClient2 `connectClient` lock/unlock. These are candidate edges,
not a certified exhaustive graph or proof of lock coverage across those bodies.
Review must account for temporary unlock windows instead of assuming a method
remains serialized solely because its entry path acquired the mutex.

Crucially the complete inherited `isLockedByCurrentThread` body just returns
true. It is now locally pinned so future integration cannot silently treat
that query as proof of ownership of `+0x88`. Reacquiring the native mutex
without knowing the producer caller's lock state would risk deadlock; busy
counts, notification methods and this always-true query are not substitutes
for the required producer serialization proof. No runtime lock/hook changed.

`Context2::getDataBuffer` follow-up: its full symbol-bounded `0x9e4`-byte body
was read, including allocation, reuse, list mutations, mapping and cleanup
paths. A local body-hash contract now fixes that reviewed identity, but does
not certify every called allocator/mapping method. There are **two** explicit
unlocked wait windows: `0x14b6f1b4..0x14b6f1df` uses the local accelerator
unlock/lock copies, while `0x14b6f4c2..0x14b6f503` inlines `IOLockUnlock` and
`IOLockLock` on accelerator `+0x88`. The latter also performs busy accounting,
notification virtuals and waiter-counter updates outside those two call sites.
Thus a helper-call-only graph misses an actual unlocked interval.

Each path initializes a 64-byte stack event through virtual `+0x140`, copies
the selected resource event through `+0x1b0`, releases the mutex, and calls
`+0x178` on that stack event. Local Fast2 vtable identities pin these as
`initEvent`, `copyEvent`, and `finishEventUnlocked`; the last implementation
and its lock/wait callbacks still require review. After reacquisition, both
paths reload the buffer-list head `slot+0x38` rather than directly trusting
the pre-wait selected resource. Exact lock targets, unlocked wait calls and
post-wait reloads are separately pinned. Context/slot lifetime across the wait,
upstream callers' lock admission, mapping lifetime and hardware completion
remain unproved. The verified behavior requires producer bridge integration
to distinguish its own invocation lifetime from broader native caller locking.

Unlocked finish follow-up: the complete Fast2 `finishEventUnlocked` body was
read and locally hash-pinned (`0x194` bytes). It loops eight event entries,
checks cached and mapped completed stamps with signed subtraction, but skips
an outstanding entry if accelerator termination counter `+0xdc8` is nonzero.
For a pending nonterminated entry it invokes wait virtual `+0x238`. Nonzero
returns retry; `kIOReturnTimeout` (`0xe00002d6`, confirmed in SDK IOReturn.h)
first calls `signalHardwareError(reason=3, channel)` and then retries too.
This caller supplies no finite retry count or independent deadline. A zero
wait return resumes accounting, not an extra hardware completion read here.
The result accumulates converted elapsed wait time rather than returning a
completion boolean; getDataBuffer uses it for accounting. Termination skipping,
wait dispatch and error-request target are additionally pinned.

The complete `signalHardwareError` body (`0x112` bytes) was also read and
locally hash-pinned. It locks event-machine mutex `+0x50` via IOLockLock,
compares/updates a channel's software restart request in array `+0x98`,
and invokes event-source `+0x80` virtual `+0x1d8` only for a stronger request,
then unlocks. This is not accelerator mutex `+0x88`, GPU reset acknowledgement
or DMA-stop evidence. Downstream event-source processing and full wait method
locking/lifetime still require review. These findings preserve the V267 error
fail-stop boundary; they do not implement normal completion, finite recovery
or certify that a guest panic protects the PF. No runtime behavior changed.

Full wait follow-up: base `waitForStamp` (`0x314` bytes) and its timeout
`disable_stamp_interrupt` cleanup (`0x48` bytes) were read completely and
locally hash-pinned. Event-machine mutex `+0x50` protects waiter array `+0x48`
increments/decrements and first/last-waiter virtuals `+0x240`/`+0x248`; it is
released before sleeping. Paired BootKC identities now resolve the sleep path
to `clock_interval_to_deadline`, `assert_wait_deadline`, and `thread_block`.
The stamp is reread after wait registration before blocking, and after wakeup;
signed requested-minus-completed determines progress. Device restart state
`event+0x94` yields SDK `kIOReturnDeviceError` (`0xe00002e9`). Timeout cleanup
decrements the waiter under the mutex and returns SDK `kIOReturnTimeout`.
Termination still permits zero return before any stamp read or during the loop.
Accelerator byte `+0xc9c` bit 0 additionally selects a direct polling branch
without the sleep/deadline path; its flag policy remains unreviewed.

Base Fast2 virtuals `+0x240`/`+0x248` resolve to six-byte no-op methods, now
locally pinned. This must NOT be generalized to the retained Intel driver:
its IGAccelEventMachine overrides both at `0x16176`/`0x16238`, tail-calling
overloads at `0x16182`/`0x16244`. Preliminary disassembly shows software
waiter/mask accounting and paths into scheduler virtuals/event-source/helper
operations. Their complete graph and hardware safety remain an explicit next
review gate, not silently certified by the inherited no-ops. Mapped stamp
pointer lifetime across sleep, native producer locking and actual completion
remain unfinished. No VM, VF state or driver runtime behavior changed.

Concrete Intel stamp-IRQ follow-up: all four EventMachine wrappers/overloads,
two Scheduler4 methods, two CommandStreamer4 methods, four singular bridge
methods, three periodic timer methods and interrupt-type index lookup were
read fully. Both payloads now pin the 16 complete symbol-bounded bodies, four
effective virtual targets and eight direct typed graph edges. This closes the
previous preliminary override-body review, not all downstream API semantics.

The EventMachine overloads update software waiter/mask state (`+0xd90`,
`+0xd40`, `+0xd80`) and select either Scheduler4 stamp methods or periodic
timer source `+0xd30`. Scheduler4 resolves the channel's command streamer;
CommandStreamer4 updates its software refs/mask and calls bridge **singular**
enable/disable. The type-index lookup selects a 12-byte descriptor at bridge
`+0x680`; the traits overload only increments/decrements descriptor `+8`
under its native lock. These reviewed singular bodies do not directly access
MMIO. They must not be confused with plural `enableInterrupts` or replaced
solely because their names contain "interrupt". No new no-op route was added.

The periodic path uses software event-source set `scheduler+0x438`, mutex
`+0x440`, timer `+0x448`, and user count `+0x450`. Its callback iterates source
objects, invokes their `+0x1e0` virtual, and rearms the timer with argument 1000
while the set is nonempty. The bodies are pinned, but imported set/iterator/
timer API identities, source construction, callback virtual meaning and object
lifetime are still review gates. This is not proof of host-safe callbacks,
correct interrupt delivery, completed GPU work or a validated boot candidate.

Inherited implementation found locally (2026-10-04): archived Tahoe 25G229
`SystemKernelExtensions.kc`, SHA-256
`5cb1be1dc530b4b953a33943567589101d3ac46bb8cf90728566ee7e5b1fa214`,
contains IOAcceleratorFamily2 at fileset offset `0x14b65000` with a readable
embedded symbol table. `tools/tahoe_ioaccel_mapping_contract_test.py` verifies
its identity, fileset/symbol uniqueness, segment mapping and exact reviewed
instruction prefixes; run it separately against that local KC. The proprietary
KC is not committed or required by remote CI. This corrects the prior working
assumption that no local inherited implementation could be inspected.

At `0x14bb76ec`, `getGPUVirtualAddress` tests mapping flags `+0x10 & 0x40`:
the clear branch returns `+0x98`; the set branch delegates through backing
memory `+0x18`, virtual `+0x158`. Do not infer GGTT solely from the returned
field or confuse its flag with packet global-GTT selection. At `0x14bb788c`,
`complete` is only `dec dword [+0x0c]` and return: it does not wait, poll a GPU
stamp, deregister a context or invalidate a page table. At `0x14bb7896`,
`finishEvent` tail-dispatches accelerator `+0x380` event-machine virtual
`+0x188`, passing mapping event storage `+0x38`. The latter event machinery
must be reviewed before claiming mapping-free waits guarantee GPU quiescence.
These exact binary contracts do not certify allocation, DMA order or runtime.

Event follow-up: the archived KC uses chained kernel-cache pointer format 11;
XNU `EXTERNAL_HEADERS/mach-o/fixup-chains.h` and
`osfmk/mach/dyld_kernel_fixups.h` define the 30-bit target and 2-bit cache level.
The Fast2 vtable's reviewed slots are unauthenticated level-1 references:
`+0x188` to Fast2 `finishEvent`, `+0x238` to base `waitForStamp`, and `+0x148`
to Fast2 `cleanEvent`. The local fixture test pins these encoded fields and
symbol identities rather than treating the encoded pointer as a live address.

Fast2 `finishEvent` (`0x14b95810..0x14b959a0`) visits eight event entries,
skips channel -1, compares requested minus cached/read stamp as signed 32-bit,
reads stamp storage through event-machine `+0x28`, and calls virtual `+0x238`
for outstanding work. A nonzero result calls `handleFinishChannelRestart`
then retries; the end invokes `cleanEvent`. Importantly, `waitForStamp` at
`0x14b77954` loads accelerator `+0x10`, sets return value zero, and branches
straight to its return epilogue when accelerator dword `+0xdc8` is nonzero,
without reading stamp storage. Consequently a successful event wait is not
unconditionally proof of hardware completion. The meaning and writers of
`+0xdc8`, restart handler behavior and reset propagation remain to be reviewed.
The fixture now checks the complete finishEvent byte identity plus this early
success branch/epilogue. No forced-idle shortcut or runtime route was added.

Lifecycle follow-up excludes one tempting but incorrect attribution: inherited
accelerator `enableAccelerator`/`disableAccelerator` do not write `+0xdc8`.
They test accelerator `+0xc92 & 8`, conditionally start/stop the event machine's
hardware-progress timer, then set/clear accelerator `+0xc78 & 2`. The timer
helpers update event machine `+0x74` and invoke its timer object at `+0x60`;
they also do not write the accelerator's early-success field. The local KC
fixture pins all four complete helper bodies. Preserving these native calls
in the VF wrapper remains necessary for software progress monitoring, not a
GuC completion or hardware-quiescence proof.

Symbol-bounded IOAccel disassembly found direct reads of `+0xdc8` in submit,
progress check, wait, pageoff, event test/scrub, finalization and accelerator
wait functions, but no direct writer in the examined decoded instructions.
The Intel payload's explicit `0xdc8` accesses belong to WOPCM MMIO reached
through accelerator `+0x1240`, not this accelerator member. No field writer
or meaning is therefore claimed: alias-pointer stores, wider initialization,
undecoded/external code and termination propagation remain to be checked.
The driver must not forcibly clear this unknown inherited state.

Termination follow-up now resolves a writer: `deviceTerminatedUnlocked`
(`0x14ba2434`) adds `0xdc8` to the accelerator pointer and calls stub
`0x10132`. The stub's encoded level-0 import resolves to BootKC
`OSIncrementAtomic`, whose reviewed implementation is locked xadd of 1,
returning the previous value. This indirect-pointer writer was not visible
in the earlier displacement-only scan. `requestTerminate` directly invokes
the helper; the helper returns early if the old count is nonzero or `+0xdd0`
is nonzero, otherwise invokes event-machine virtual `+0x250` and further
termination callbacks. Thus `+0xdc8` is incremented by termination and enables
the previously identified non-hardware wait success path. Counter reset,
all termination callbacks and their GPU-stop guarantees are not yet proven.

The local fixture optionally accepts the archived BootKC as its second argument
and verifies SHA-256
`5cba9e36ceed5d73e1d569d1772bc46fecbd0359f824db689863e686d856ea3b`,
kernel symbol identity, stub bytes and encoded import, the BootKC `__HIB`
unslid base, segment mapping, and the atomic implementation. BootKC and
SystemKC use different address bases; blindly indexing the BootKC by the
encoded target or using its `__TEXT` base is incorrect. This evidence confirms
why event success after termination cannot substitute for GuC acknowledged
deregistration. No termination counter manipulation or runtime shortcut added.

Termination callback review: Fast2 virtual `+0x250` resolves to
`IOAccelEventMachineFast2::deviceTerminatedUnlocked` (`0x14b969f4`). It walks
channel pointers in `+0x28`, copies software dwords from event-machine `+0x104`
(stride `0x18`) into the pointed-to stamp storage, executes `sfence`, then
dispatches the base termination virtual. The full body and concrete virtual
identity are now pinned by the local fixture. This path does not wait for
GuC disable/deregister acknowledgement. The base termination method tails
another channel loop issuing virtual `+0x228` with argument zero; remaining
virtual semantics are not certified yet.

Safety implication: advancement of completed stamp memory can be a CPU write
during termination, not a GPU post-sync write. A future VF completion/idle
mechanism must distinguish this path and reject termination/restart/fault
state before treating stamps as hardware evidence. Current enabled contexts
remain conservatively busy; no stamp-only idle shortcut was introduced.
This does not by itself fix termination admission, backing reclamation, or
prove the entire termination callback graph is DMA-safe.

Base callback follow-up resolves channel virtual `+0x228` to
`IOAccelEventMachine2::signalStamp` (`0x14b77ffa`). It calls the same kernel
stub twice, using the channel stamp address and event machine itself as wake
events. Cross-KC symbol resolution confirms that stub imports
`thread_wakeup_prim`; these are software wakeups, not GuC actions. It also
notifies a software object at `+0xa8`, conditionally signals another event
source at `+0x78`, and emits tracing. The local fixture pins the concrete
virtual, complete signalStamp bytes and the BootKC wakeup import identity.
Remaining notification-object/virtual details and the separate accelerator
termination callbacks still need review. Nothing here supplies a hardware
idle or DMA-stop acknowledgement, so software-terminated stamps remain
excluded from a future hardware-completion baseline.

Restart follow-up: inherited `restart_channel` (`0x14b77dbc..0x14b77ff8`)
uses event virtual `+0x218` for progress and `+0x220` for timeout recovery.
On the normal recovery branch it stops the progress timer, calls the timeout
virtual, optionally invokes returned object's virtual `+0x160`, then restarts
the timer even when the returned object is null. Its common tail clears the
channel entry at `+0x98`, clears restart state `+0x94`, updates channel `+0x90`,
and signals its event source. These software changes are not GPU-stop evidence.
The local KC fixture pins the complete restart_channel bytes.

Crucially the actual Intel event-machine vtable overrides `+0x220` with
`IGAccelEventMachine::eventTimeout`, now verified in both payloads by the
offline lifecycle test. Do not substitute the base Fast2 eventTimeout when
tracing this route. The pinned Scheduler4 progress method returns true,
leading the Intel timeout path to encodeDebugInfo and a null recovery result.
The current VF physical-debug replacement is a void no-op: it avoids raw
diagnostic MMIO, but does not turn this timeout into a reliable protocol fault
or bounded wait outcome. This remains a failure-propagation gap to fix at a
verified timeout boundary, not by fabricating completion or repurposing every
debug capture as a fatal error. No new runtime route was added in this review.

Both pinned accelerator payloads resolve `IGAccelMemoryMap` vtable `+0x128`
through an external unsigned 64-bit relocation to
`IOAccelMemoryMap::getGPUVirtualAddress`. The mapped-buffer getter delegates
to this slot; its unresolved zero word on disk is not a GPU address and does
not establish GGTT versus PPGTT semantics.

`IGMappedBuffer::initWithOptions(IGAccelTask*, size, bool, options)` calls the
imported `IOAccelSysMemory::withOptions`, obtains a mapping through memory
virtual `+0x138`, and checks mapping virtual `+0x138`. If that check fails,
the imported `IOGraphicsAccelerator2::freeToPrepareMapping` must succeed
before the mapping is published at buffer `+0x30`. This is an admission
contract, not evidence that later explicit unmapping cannot invalidate it.
The offline lifecycle test now pins these relocations and publication anchors
for both payloads. Inherited IOAccel allocation/address semantics and their
relationship to stamp packet address-space flags remain unproven; no DMA
address rewrite, VM boot, or deployment is authorized by these checks.

The next feature-source review resolves `populateAccelConfig`'s RIP-relative
`PPGTT` property lookup (default 1) and bit-8 assignment in the 64-bit feature
word at accelerator `+0x1190`. This is byte `+0x1191` bit 0. The already-pinned
`getGTTWriteMode` returns its inverse; main/compute stamp encoders put this
value into PIPE_CONTROL's global-GTT bit. `IGMappedBuffer::getMappingOptions`
returns 7, but the inherited IOAccel interpretation of these option bits is
not available in this payload. The new test pins property identity, default,
feature publication and the mapping-options getter for both variants.
`IGMemoryManager::commitIntoPageTableForTask` obtains GPU address/length from
mapping virtuals `+0x128/+0x168` and walks task `+0x268` page tables, calling
`IGHardwarePageTable::commitRange`; mapping allocation and complete task
page-table ownership must still be traced before certifying address-space
consistency. No registry property was changed on the guest or host.

## V266 validate packet backing bounds before registration (offline)

Mapping lifetime follow-up: the separate task method
`releaseStampAndScratchPages` performs two reference releases and clears task
`+0x280/+0x288`; it does not directly invoke GPU mapping teardown. The mapped
buffer's `free` calls imported `IOAccelMemoryMap::finishEvent`, mapping virtual
`+0x140` (external relocation to `IOAccelMemoryMap::complete`), then releases
and clears buffer `+0x30`. These names do not prove GuC/GPU completion and do
not replace acknowledged context deregistration. Both payloads now pin these
relocations and teardown anchors.

`IGSharedMappedBuffer::free` first calls the imported system-memory CPU unlock
and clears `+0x38`, then dispatches to mapped-buffer free. Its separate
`unlockForCPUAccess` method performs that same CPU unlock and clears `+0x38`
without a GPU mapping-complete call. Thus retaining the shared buffer is not
proof that its CPU mapping remains available: explicit unlock must be excluded
or synchronized for all active context/stamp users. A direct-call scan found
no calls to these methods in the pinned text, which does not exclude virtual,
inherited, or external callers. That reachability/lifetime obligation remains
open. No new suppressing route or guessed idle signal has been added.

Subsequent local-vtable inventory narrows that obligation: neither explicit
CPU-unlock helper nor task stamp/scratch cleanup appears in any defined local
vtable in either accelerator payload. Mapped/shared-buffer destructor slots
are instead `+0x90`, pointing to their respective `free` methods. The offline
test now checks this inventory. Thus local virtual dispatch to those two
specific helpers is excluded; independently imported system-memory unlock,
external direct callers and inherited mapping invalidation are still not
excluded. The framebuffer's symbol inventory contains no reference to either
helper. These findings narrow the search, not certify overall lifetime safety.

Retaining a buffer does not prove an encoded destination lies within it.
The native packet path uses signed stamp index at ring `+0x38`, a 64-byte
slot stride and an eight-byte scratch post-sync store. Direct attach previously
checked context-image bounds but did not check these packet backing lengths.
V266 rejects a negative stamp index, a slot extending beyond task stamp buffer
length, or a scratch backing shorter than eight bytes, before any backing retain
or REGISTER_CONTEXT publication. The subtraction/division formulation avoids
overflow for malformed lengths. It does not validate the final GPU address,
address-space flag or backing memory-descriptor integrity.

The offline model compares nearby slot/buffer endpoints against an independent
widened multiplication oracle, including scratch sizes 0/7/8/page-size,
negative indices, INT32_MAX and UINT64_MAX lengths. Source contracts require the
check before direct-record retain and registration. PF behavior, transport
framing and the 93-route inventory remain unchanged. Address-space correctness
and true enabled-context completion are still unresolved; no runtime safety
claim follows from this numeric check.
Both payload contracts and the full offline suite passed on 2026-10-04
(`/tmp/ngreen-static.9zgLQ9`). No deployment, VM start or GPU-state write occurred.

## V265 retain packet stamp/scratch backing independently (offline)

The packet audit establishes two more concrete backing dependencies:
task `+0x288` provides the stamp address, and task `+0x280` provides the scratch
address used by main/compute PIPE_CONTROL prerequisites. The direct GuC record
previously retained image/ring only and relied on native task/event ownership
to keep these packet destinations alive. That complete native owner graph has
not been proven; dependency-registration success is not GPU completion.

V265 adds independent retained stamp and scratch buffer pointers to each direct
record. Both must exist and match on repeated attach, and both are retained
before registration publication. Deregister completion plus final native
reference retirement and the absence of a protocol fault remain prerequisites
for release, which occurs outside the simple lock. Identity clearing, tombstone
lookup/reuse and bootstrap-unowned checks now include all four backing objects.
Only buffers are retained, not the task or context, avoiding a new task/context
reference cycle. There are no buffer copies or additional H2G messages; the
change adds two pointers per record and paired object references per lifecycle.

The event model checks that DEREGISTER_DONE preserves all four references and
that final release clears them. Source contracts pin retain-before-register and
release-after-unlock. This protects the identified packet destinations; it does
not prove the correctness of address-space flags, scratch initialization, all
other resource references or firmware DMA quiescence. PF and the 93-route
inventory are unchanged. No deployment or VM start is authorized by these checks.

Publication review also confirmed the existing FAST sender writes the context
tail only after space/credit admission, synchronizes it, writes the CTB packet,
synchronizes before publishing CTB tail, then synchronizes before GuC notification.
This is the observed code ordering, not a substitute for controlled hardware
visibility or successful command-execution evidence.
Both payload contracts and the complete offline suite passed on 2026-10-04
(`/tmp/ngreen-static.vKUdLD`); controlled runtime validation remains outstanding.

## V264 retain the actual DMA ring through deregistration (offline)

Packet-encoder follow-up uses the local primary i915 reference pinned at
`c613c76e2e7023b1617bed346b9d35bf871fb958` in
`references/i915-sriov-current` (`strongtz/i915-sriov-dkms`), specifically
`gt/intel_gpu_commands.h` and `gt/gen8_engine_cs.c`. Main and compute ring
vtable `+0x140` resolve to their own `commitStampCommand` implementations.
Both construct a six-dword `PIPE_CONTROL` (`0x7a000004`) with post-sync QW
write and CS stall (`0x01104498` before address-space/notify adjustments),
derive destination from stamp GPU base `+0x28` plus signed index `+0x38` times
64, and place the supplied stamp in the immediate data. Their preceding
scratch-target packet derives its address from task scratch GPU backing.
Both paths have two scratch getter calls, two GTT-mode queries and four
conditional `writeBuffer` sites, now pinned in both payloads.

The base encoder instead builds `MI_FLUSH_DW` with a store operation
(`0x13004003` before notify/address adjustments), using the same stamp-base
and index-stride destination. The source/header comparison establishes the
intended GPU store, not its execution or platform-specific correctness. The
address-space flags, scratch lifetime, packet visibility before notification,
successful transport publication, completed-event handling and sequence
wraparound must still be verified together before changing the conservative
enabled-context idle response. None of these offline anchors proves a Metal
command actually completed.

Stamp producer follow-up: base, main and compute `writeStamp` dispatch their
packet encoder at ring vtable `+0x140`, then CPU-write the submitted value at
stamp slot `+0x8`, ring `+0x44`, and the pending-stamp flag at `+0x48`.
`sleepForStamp` instead reads slot `+0x0` at the same 64-byte index stride and
compares it with the requested stamp. The focused contracts distinguish these
fields across all three producers; the software `+0x8` write is not completion
evidence. Packet encoders, GPU visibility/order, full submission identity and
wraparound semantics still require review before an enabled-context idle fix.

Additional native wait-recovery primitives remain open: `clearEventWait`
(`0x434d0`) and `clearSemaphoreWait` (`0x43510`) both derive an engine register
offset from ring `+0x58`, write through accelerator MMIO `+0x1240`, and read
through cached ring MMIO `+0x50`. Neither has a dedicated current VF route.
Their direct/virtual callers and eligibility must be mapped before dynamic
admission; ordinary completion must not be implemented by invoking these
physical wait-clear helpers. These are observed primitive bodies, not yet
proven reachable execution paths or diagnosed causes of the host crash.
A preliminary scan of defined vtables and nonzero-displacement direct branches
found no references to either exact entry point in the production payload.
That negative scan does not exclude address-taken, indirect or inlined uses;
it does not justify adding suppression routes without a verified caller graph.

Garbage-collection follow-up: `IGGCObject::release` queues its last-reference
object on first release, then invokes the object's `check` virtual (`+0x128`)
on a later release. For hardware contexts that slot resolves to
`IGHardwareContext::check`, which tail-dispatches Scheduler4 `+0x168` to
`isContextIdle`, then the routed GuC `isKmdContextIdle` descriptor query.
`collect` invokes checked release (`+0x28`) on queued objects. In contrast,
`forceCollect` uses `releaseNoCheck` (`+0x118`), and `drain` eventually falls
back to that same unchecked release after its bounded retry loop. These paths
cannot establish idle by themselves; successful descriptor deregistration and
independently retained ring/image backing remain necessary even if ordinary
collection normally waits. Contracts pin both checked and unchecked paths in
both payloads.

The current VF idle snapshot keeps enabled contexts busy even after execution
could have completed. That prevents premature success but is not a functional
completion mechanism: normal context reclamation can be delayed indefinitely
until explicit disable/shutdown or forced collection. This is an outstanding
driver functionality/performance gap, not a solved optimization. A future
completion implementation must use proven hardware/event evidence and preserve
deregistration safety; neither a fabricated idle response nor notification
registration is an acceptable replacement. Collector `+0x20` comes from timer
event-source creation, not the accelerator; its drain virtual must not be
interpreted through an accelerator vtable.

Follow-up task/stamp review: `getStampGPUVirtualAddress` and `getStamps` both
load task `+0x288`, then tail-call the GPU/CPU address getters respectively.
`initStampAndScratchPages` either clones that shared buffer from the accelerator's
kernel task or allocates `0x3000` bytes; task `free` releases and clears this
field. Ordinary context initialization retains its task, but context flag
`+0x6e` bit 0 skips that retain. Task `release` attempts notification/release
of four owned context slots (`+0x2a0/+0x2a8/+0x298/+0x290`) on its guarded
last-reference path. This is not proof that the notifications completed.

Context notification delegates through FIFO to ring notification. If the FIFO
reports success and the supplied event is non-null, the context retains its
task and sets `+0xc8` to 1. This second ownership acquisition must be considered
alongside the initialization retain; assuming that skipped initialization retain
means no later task reference would be incorrect. Both payload contracts pin
these getters, allocation/clone edges, four context slots, and the conditional
notification retain. Ring notification/event completion and the full task owner
graph remain open; this evidence does not justify adding or removing a retain.

Further ring notification tracing resolves the inherited event virtual through
its external Mach-O relocation, not a guessed zero on-disk vtable word:
`IGAccelEventMachine` slot `+0x1b8` (`0xcebc0`) binds to
`IOAccelEventMachineFast2::mergeEvent(IOAccelEvent *, IOAccelEvent *)`.
Ring `notifyComplete` returns false for a negative stamp index; otherwise it
returns true, and a non-null supplied event is merged with FIFO event `+0x138`.
There is no direct stamp wait or force-wake edge in that routine. Thus the
success that triggers context's task retain establishes event dependency
registration, not GPU completion or DMA quiescence. The two payload contracts
pin the relocation's symbol/type/width and the dispatch/return-path anchors.
The inherited merge implementation and final event callback ownership are still
unreviewed; dependency registration must not be substituted for their evidence.

Allocation-site tracing corrects V263's `+0xa8` interpretation: it is an
additional `IGSharedMappedBuffer`, not the ring object. Context initialization
stores `IGHardwareRingBuffer::withHardwareContext` at `+0xb0` and its FIFO
channel at `+0xb8`. Context `free` releases both **before** Scheduler4 cleanup
at `+0x138`. `initRingGPUVirtualAddress` follows context `+0xb0`, ring `+0x80`,
and writes that mapped buffer's GPU address to context image `+0x1024`.
Therefore V263's late fail-stop alone could not prevent early DMA ring release.

V264 adds a separate retained `ringBacking` to the direct GuC record before
REGISTER_CONTEXT publication. A repeated attach must match that ring backing
as well as its existing descriptor/image identity. Native ring/FIFO object
destruction can then drop their references without freeing the GPU ring pages.
The record releases both ring and image outside the simple lock only after
acknowledged deregistration, zero native references and no protocol fault;
the tombstone identity-clearing helper clears both pointers. Tombstone reuse
and bootstrap unowned-table checks also account for the ring reference.

The native context initializer calls Scheduler4 attach at `+0x128` without
checking its return value before continuing. An unsuccessful compensating
deregistration after partial registration must therefore fail-stop immediately,
not return false and rely on this native caller. Clean no-registration failures
still return false; propagating those failures through native context creation
is a separate unresolved issue, not a claimed successful allocation path.

The two payload contracts now pin the ring address source, unchecked attach
site and complete ring/FIFO-before-cleanup ordering. Source contracts pin retain
before publication and release after unlocking; the shutdown event tests retain
both pointers through DEREGISTER_DONE and clear them at final release. These
are offline checks, not runtime proof or a guarantee against host PF faults.
Both payload contracts and the complete offline suite passed on 2026-10-04
(`/tmp/ngreen-static.TeFS7J`). No VM start, hardware-state change or deployment
was performed.

## V263 context teardown DMA backing boundary (offline)

The Scheduler4 vtable distinguishes context cleanup at `+0x138` from ring
`bind` at `+0x1c8` and `unbind` at `+0x1d8`. The latter is an exact no-op in
the pinned payload. Context `free` invokes shared-private cleanup before
releasing additional mapped backing (`+0xa8`), context image (`+0x98`) and task
(`+0x58`), in that order. It already released the ring (`+0xb0`) and FIFO
(`+0xb8`); V264 above corrects the earlier ownership interpretation and closes
that early DMA ring release gap. The lifecycle contract pins the slots and
destructor anchors.
Earlier ring-init/free working notes must not equate bind/unbind with
descriptor attach/detach.

This ordering exposed a real failure-path gap: `vfDetachContextDesc` retained
only the context image on uncertain or timed-out retirement, then returned
through a void ABI. Native teardown could still release ring and task/stamp
allocations while GuC might retain their addresses. Image quarantine alone
does not establish safety for that graph. V263 now marks the protocol fault
and fail-stops the guest at all seven previously returning uncertainty sites:
missing bookkeeping, invalid identity, unavailable queue, duplicate final
detach, mismatched backing, untracked record and incomplete disable/deregister.
Existing post-shutdown assertions and successful retirement remain unchanged;
PF behavior and route inventory are unchanged.

This is a destructor safety boundary, not GPU reset, DMA cancellation or a
successful hardware-acceleration baseline. A guest panic cannot guarantee host
PF safety or undo already issued DMA. Independent host containment remains
mandatory; VM start and deployment remain prohibited. Partial initialization,
the special task-retain paths, and global DMA quiescence remain under review.
Both focused payload contracts and the complete offline suite passed on
2026-10-04 (`/tmp/ngreen-static.3k1coR`). CI and runtime validation are separate.

## V262 VF physical reset/replay exclusion (offline)

Follow-up review on 2026-10-04: native `waitForSpace()` has two direct
`waitTimeout()` sites. The wait routine polls context backing at ring `+0x18`,
head offset `+0x10`, queries the pinned Scheduler4 progress slot and has one
diagnostic edge to the V261-contained `debugGraphicsEngine()` graph.
`sleepForStamp()` polls the shared stamp backing at ring `+0x30` and returns
the outstanding-stamp comparison. Neither wait has a direct physical reset
or three-argument force-wake edge. The focused lifecycle contract now pins
these call sites and shared-memory anchors in both payloads. This evidence
covers these waiting routines only; their callers and backing lifetimes remain
separate review obligations. Normal waits are retained without fabricated
completion or additional suppression routes.

The adjacent Scheduler4 shared-private lifecycle was traced separately:
`initSharedPrivateData` (`0x1e0fc`) tail-calls
`IGHardwareGuC::AttachContextDescToGucContext` with context `+0x89`;
`cleanupSharedPrivateData` (`0x1e126`) calls `invalidateTLB` before tail-calling
`DetachContextDescFromGucContext`. These are the routed descriptor operations,
not the similarly named `IGGuC` shared-private allocation methods. The focused
contract pins these direct edges and their cleanup order in both payloads.
Ring initialization borrows the context image and task stamp CPU mappings;
the task stamp allocation at `+0x288` can be created or cloned from the kernel
task. Context teardown releases additional mapped backing at `+0xa8` before
its image at `+0x98` and task at `+0x58`; ring/FIFO are released earlier, as
corrected and addressed in V264 above. These local ownership observations do not establish global
DMA quiescence, all partial-init paths, or the special task-retain paths; those
remain open obligations. No dynamic safety claim or driver route change follows.

- The adjacent FIFO recovery graph is independent of the V261 diagnostic
  routes. `IGAccelFIFOChannel` vtable slot `0x200` points to
  `resetHardwareAndReplay()`. That routine dispatches ring-buffer slot `0x168`,
  resumes the scheduler twice and can submit/wait for two replay stamps. The
  ring slot is exactly `IGHardwareRingBuffer::resetGraphicsEngine()` in both
  admitted Tahoe payloads.
- The concrete reset body is physical engine ownership, not a GuC VF recovery
  operation. It has four three-argument force-wake edges plus one one-argument
  edge, writes engine controls through accelerator MMIO `+0x1240`, performs the
  `0x4a08/0x941c/0xcec4` reset/fault-clear sequence, replays a native reset
  register list and dispatches scheduler reset completion. A guest VF must not
  execute or claim success for any part of that sequence.
- A classified VF now quarantines the FIFO reset/replay root as a protocol
  fault and returns without replay. The lower physical reset primitive is also
  routed defensively, marks the fault and returns `false` for any independent
  caller. Neither wrapper invokes an original trampoline, private object field,
  force-wake or MMIO. PF reset/replay behavior remains native.
- The expanded binary/source contract pins both vtable slots, the replay
  virtual/direct call inventory, all five force-wake calls and representative
  destructive reset writes in both payloads. Route inventory is now 93 unique
  symbols (90 accelerator, three framebuffer). This is offline containment;
  no candidate was installed or loaded and no VM/PCI/SR-IOV state changed.

## V261 VF timeout and hang-diagnosis hardware exclusion (offline)

- `IGAccelEventMachine::eventTimeout()` was disassembled end-to-end in both
  admitted Tahoe payloads. Its scheduler virtual calls are now pinned to the
  exact Scheduler4 vtable slots: `checkForProgress()` returns constant true,
  pause/resume reach exact GuC no-ops, active-context discovery zeros all three
  outputs, and reset preparation is an exact no-op. The normal VF timeout path
  therefore reaches `IntelAccelerator::encodeDebugInfo()` directly while the
  state machine's software event handling remains useful.
- Native debug capture is hardware-active. `encodeDebugInfo()` calls a GuC
  collector that force-wakes and raw-reads `0xc184..0xc1a0`, then iterates
  engine collectors. Those take five force-wake transitions and enter
  `getInstDoneSlice()` four times. That helper reads, repeatedly programs and
  restores the global `0xfdc` selector while reading `0x7100`, `0xe160` and
  `0xe164`. It is therefore unsafe even if its captured values are never used.
- The timeout state machine also contains one physical
  `haltCommandStreamer()` and two `resumeCommandStreamer()` calls. Their native
  bodies write `0x10001`/`0x10000` to the engine's physical `RING_MI_MODE`, poll
  it up to 10,001 times and take force-wake on entry and exit. Scheduler4's
  current constant-progress branch makes these calls normally unreachable on
  this VF, but they are isolated defensively and their topology is contract
  pinned rather than assumed.
- IOAccel can request equivalent diagnostics outside `eventTimeout()` through
  `IGHardwareRingBuffer::debugGraphicsEngine()` and
  `IGAccelFIFOChannel::getHardwareDiagnosisReport()`. Both converge on
  `doHangAnalysis()` and `dumpHangAnalysis()`; the dump path reaches raw ring
  status and the broad RCS/BCS/VCS/VECS/system register dump through the ring
  object's cached MMIO base. A classified VF now replaces both shared hang
  boundaries, debug capture, and halt/resume with exact-ABI no-op/zero results.
  PF behavior and Tahoe's surrounding software timeout/event unwind remain
  native.
- The binary/source contract fixes all five timeout virtual slots, exact native
  no-op bodies, three debug-capture/one halt/two resume call sites, the complete
  GuC/RING/INSTDONE destructive graph, hang-diagnosis roots and raw dump anchor.
  Route inventory is now 91 unique symbols (88 accelerator, three framebuffer).
  The complete syntax, zero-finding Clang analyzer, strict ABI, Mach-O,
  exhaustive protocol and sanitizer suite passes at
  `/tmp/ngreen-static.BvklAb`. No candidate was installed or loaded; VM, PCI
  binding and SR-IOV state were not touched.

## V260 VF debug-sysctl control-plane exclusion (offline)

- The retained native start tail calls `IntelAccelerator::initSysctl()` after
  engine, trace-manager and block-fence setup. Pinned disassembly shows that it
  registers 55 debug OIDs before testing `TelemetryDisable`; all 52 leaf idvar
  OIDs point at `intelLong_sysctl`, whose write path enters
  `__idvarSetParamLocked`. The native initializer then publishes the
  accelerator into the global `l_accelerators` array and can enter the OA
  setup helper. This debug control plane is not required for Metal execution.
- The idvar dispatcher has direct telemetry/OA and trace-state edges. The trace
  turn-on path retains its own `TelemetryDisable` gate, but when that private
  gate is clear its graph includes timestamp reads at raw MMIO `0x2358/0x235c` and the
  `0x91bc` performance-counter write. Leaving a broad PF debug surface
  registered in a VF is therefore an unnecessary and fragile dependency on
  every private selector's individual gating.
- A classified VF now replaces both `initSysctl()` and its one native stop-path
  `unregisterSysctl()` with the same exact-ABI no-op. Pairing the two routes
  prevents teardown from unregistering OIDs that the VF never installed. PF
  start and stop remain native. The binary/source contract fixes start/stop
  call edges, the 55-register/55-unregister topology, all 52 handler pointers,
  global publication, idvar/trace/OA descendants, hardware anchors and the
  hardware-free replacement. Route inventory is now 86 unique symbols (83
  accelerator, three framebuffer).
- This remains static containment work. No candidate was installed or loaded,
  and the VM, PCI binding and SR-IOV state were not touched. The complete
  static suite passes at `/tmp/ngreen-static.1PeApO`.

## V259 pre-engine eDRAM detection exclusion (offline)

- The native memory-manager graph reaches physical registers before the routed
  engine boundary. `IntelAccelerator` vtable slot `0xaf0` constructs an
  `IntelTGLMemoryManager`; `IGMemoryManager::init()` clears capability bytes
  `+0x20/+0x21`, completes its GGTT/fence/page-pool setup, then unconditionally
  dispatches vtable slot `0x148` to
  `IntelTGLMemoryManager::detectEDRAM()`.
- Native detection takes physical render force-wake and raw-reads `0x120010`.
  A property-dependent branch takes force-wake again, programs
  `0x138128/0x138124`, waits, and reads `0x145910`. Suppressing the force-wake
  calls does not prevent those loads/stores. None of these registers belongs to
  a VF, and this all occurs before `startGraphicsEngine()`.
- The classified-VF route now replaces only `detectEDRAM()` and explicitly
  preserves the already initialized false/false capability state. It performs
  no original call, force-wake or MMIO. A PF retains native detection. The
  binary contract fixes the accelerator factory slot, exact TGL metaclass,
  memory-manager virtual slot, initial software state, all three raw-MMIO
  anchors and all four native force-wake calls in both pinned payloads. Route
  inventory is now 84 unique symbols (81 accelerator, three framebuffer).
- This is still static containment work; no candidate was installed or loaded,
  and the VM/PCI/SR-IOV state was not touched.
  The complete syntax, zero-finding Clang analyzer, strict ABI, Mach-O,
  exhaustive protocol and sanitizer suite passes at
  `/tmp/ngreen-static.hCgCPq`.

## V258 legacy hardware-fence exclusion (offline)

- Tahoe's tiled/aperture resource path can reach
  `IGFenceAllocator::allocate()` independently of the already isolated engine
  start. The allocator consumes a legacy fence slot and enters
  `IGFence::initWithOptions()`; that constructor takes physical render
  force-wake and writes both halves of the selected fence through accelerator
  MMIO base `+0x1240` at `0x100000 + index*8`. `IGFence::free()` repeats the
  same physical force-wake and register writes before returning the slot.
- A classified VF now replaces the allocator boundary with an exact null
  result. It does not consume the allocator bitmap, construct an object, claim
  a fence, or fabricate success. The reachable `IGAccelResource::addToAperture`
  caller already releases its aperture allocation and returns false on null;
  the display-pipe caller has an equivalent cleanup path and is independently
  unreachable because VF framebuffer probe/start is rejected. A physical
  function retains the native fence allocator unchanged.
- The lifecycle contract fixes the allocator-to-constructor edge, both direct
  callers, both raw fence-register write bodies, all four force-wake calls and
  the aperture caller's null-result branch in both admitted Tahoe payloads. It
  also requires the VF route to be a hardware-free `nullptr` wrapper. Route
  inventory is now 83 unique symbols (80 accelerator, three framebuffer).
- This remains offline containment work. No candidate was installed, no AuxKC
  was built, no VM or GPU test was started, and no PCI/SR-IOV state changed.
  The complete syntax, zero-finding Clang analyzer, strict ABI, Mach-O,
  exhaustive protocol and sanitizer suite passes at
  `/tmp/ngreen-static.JVMZEy`.

## V256 physical-engine exclusion and headless callback review (offline)

- The original `IntelAccelerator::startGraphicsEngine` is now treated as an
  explicit physical-only boundary. Both admitted payloads directly enter
  `SafeForceWake`, `initModeRegisters` and
  `initHardwareStatusPageRegisters`; the last routine writes per-engine and
  global HWS GPU addresses through accelerator MMIO base `+0x1240`. The VF
  route must replace that complete symbol, and its replacement is rejected by
  the source contract if it calls an original trampoline or names any of those
  physical-state primitives.
- Headless registration allocates a local 0x60-byte callback table. All eleven
  initialized entries are now pinned to their exact payload symbols, offsets
  and bodies. Coarse power, force-wake, protected-media notification, media
  load/prepare, client notification and GuC completion callbacks are exact
  void no-ops; DPSM and PM notification return zero; GuC pre-load returns the
  fixed unsupported status. None contains MMIO or another call edge.
- Guest sleep/wake dispatches through the same routed engine start/stop and
  audited interrupt-bridge enable/disable boundaries. Scheduler-4 vtable slots
  `0x118/0x120` remain its sleep/wake methods, and the retained scheduler
  firmware initializer tests its loaded byte at `+0x20` before the `+0x220`
  load-firmware call, setting that byte only after success. Repeated wake does
  not reconstruct GuC/CTB. This is static reachability evidence; no guest power
  transition or hardware test was run.

## V255 retained native-start success-path review (offline)

- Complete pinned disassembly found a previously uncontracted pre-engine edge.
  When accelerator feature byte `+0x1190` bit `0x20` is set, Tahoe's native
  `IntelAccelerator::start` calls `setAsyncSliceCount`. That routine acquires
  render force-wake, writes the encoded slice configuration directly through
  accelerator MMIO base `+0x1240` to register `0xA204`, then releases
  force-wake. This is PF-owned hardware state. The same feature selects legacy
  page-ownership operations that the VF bridge already rejects, but those
  checks occurred later, after the raw start-time write.
- A classified VF now rejects this unsupported feature before direct-GGTT
  bootstrap, PCI MSI allocation or native start. It does not clear the feature,
  no-op the write or claim legacy ownership support. The exact native start
  call and `MMIO+0xA204` store are machine-checked in both admitted payloads;
  source ordering requires the rejection before every VF/device mutation.
- The rest of the retained success tail was followed explicitly. Native start
  allocates each hardware-status page through two
  `IGSharedMappedBuffer::withOptions` call sites, which remain on the routed VF
  GGTT path, then reaches exactly one routed `startGraphicsEngine`. Its later
  DPSM timer takes scheduler vtable slot `0x160`; both scheduler-4 and
  scheduler-5 slots are pinned to their `isGpuIdle` symbols, and the VF routes
  derive the result only from the direct GuC context table. The local DPSM
  callback at table `+0x38` is an exact `return 0` body. The post-engine
  coarse-power callback at table `+0x00` is also an exact void no-op. These
  software callbacks therefore do not introduce another physical MMIO edge.
- The expanded source/Mach-O contract passes for both Tahoe payloads and the
  complete suite passes at `/tmp/ngreen-static.ckEaeN`. No kext was installed,
  no XML/PCI state changed and the VM remained shut off under the host hard
  hold.
- Checkpoint `1620588` passed GitHub Actions run `36391019577`. Its archived
  release zip SHA-256 is
  `53501a4889da430bc9e1eaf00bd09af1aa3a81710e34474284b9e972d18afb3b`;
  kext executable UUID is `0257DAE0-C610-3576-97BD-DA67E38E9C98` and SHA-256 is
  `65e8d0384b3a2f1cf6ecb4216ace74de0b6891bcbf392eb996f5c0c370f12404`.
  The Metal smoke SHA-256 remains
  `b69ef075a8062de2f94bfa30a4e8242ba5b11f695c4dae04bd003a4efca294d5`.
  These files are archived only under `../../build/artifacts/1620588`; none was
  installed, added to an AuxKC or loaded by the VM.
- The follow-up lifecycle contract also decodes the native `test $0x20` and
  conditional branch around `setAsyncSliceCount`, proving that the rejected
  feature bit is the exact pre-start trigger for that raw-MMIO path instead of
  relying only on a nearby call edge.

## V254 fail-closed host containment gate (offline)

- The canonical root preflight is read-only and hard-codes the only admitted
  domain, PF and VF. It requires the domain off with no autostart/managed save,
  all poweroff/reboot/crash actions and the guest watchdog set to `destroy`,
  exact Intel `8086:a7a8` identities and i915/vfio-pci bindings, one VF,
  distinct IOMMU groups, no surviving QEMU, authoritative current-boot kernel
  journal access and zero configured PF DMAR/i915 triggers. Missing privilege or
  unreadable evidence is a failure, never an implicit pass.
- The one-shot controller defaults to exit 64 unless given its exact arm token
  and a complete manifest. Even then, the same preflight runs before transient
  service creation and again inside it. A kernel journal follower and a separate
  systemd monotonic deadline must both be live before the controller's only
  exact-domain `virsh start`. Trigger, deadline, start failure, watcher failure,
  state loss or controller exit all converge on one locked, ten-second-bounded
  exact-domain destroy helper. A sleep inhibitor spans the run and the 20-second
  cooldown; postflight checks domain/QEMU state, PF/VF bindings, VF count, PF
  readability and any new trigger. No path rebinds PCI, writes sysfs, unloads
  i915 or reboots the host.
- The immutable tab-separated manifest pins full Git commit, CI run, artifact
  zip, extracted kext executable, Mach-O UUID, final AuxKC, EFI backup and the
  exact inactive XML. The verifier rejects dirty worktrees, symlinks, hash/UUID
  mismatches, malformed Mach-O commands, archive/extracted-executable mismatch
  and duplicate/unknown/missing fields. It runs before and inside the transient
  service so an intervening XML or input change cannot reach VM start.
- Source-order/default-refusal and positive/mutation tests are part of
  `check-static.sh`. The full suite passes at `/tmp/ngreen-static.bgvhzJ`.
  The real preflight currently rejects this host because the account cannot
  certify the kernel journal and the inactive domain still has
  `on_reboot=restart`, `on_crash=preserve`, and watchdog `reset`. The controller
  was not armed, no XML was changed and no VM/hardware operation occurred.
- Checkpoint `2f71f9d` passed GitHub Actions run `36389963833`. Its archived
  release zip SHA-256 is
  `b50b2f300cfe5e1a2a4a859be00f3cfe0b2d9fa5bbfc1fee4cfcf5ff47896473`;
  kext executable UUID is `DB98A0FC-9002-3D29-9F70-DB37EB9F9DE8` and SHA-256 is
  `0ef62570ca942dc56cba3c66bdffa54c57dbd06ef21a955785afb3f9e517875c`.
  The Metal smoke SHA-256 remains
  `b69ef075a8062de2f94bfa30a4e8242ba5b11f695c4dae04bd003a4efca294d5`.
  Relative to `d23f7a7`, the kext differs in exactly 17 bytes: its 16-byte
  `LC_UUID` and the final day byte of Lilu's `REL-100-2026-09-28` build string.
  Zeroing those two identity fields gives the same normalized SHA-256
  `23b7bb42cf3e15f5db6fd65c2d41c88692a4c2a0ccce4ba1668f48474fd971cf`
  for both builds, so there is no runtime-code drift. The artifact is archived
  only under `../../build/artifacts/2f71f9d`; it was not deployed or loaded.

## V252 static rollback and retained-bootstrap reachability review

- Both admitted Tahoe TGL accelerator payloads contain exactly one
  post-engine error-`0x215` branch in `IntelAccelerator::start`. Its instruction
  anchor clears the Boolean result and jumps directly to the final return block;
  unlike the surrounding start failures, it does not enter the virtual
  accelerator-stop cleanup after `startGraphicsEngine()` has already brought
  the VF GuC, CTB, interrupt bridge and IOAccelerator lifecycle live.
- The outer VF start wrapper now closes only that uncovered transactional edge.
  If scheduler firmware is live and the ordinary stop route has not begun, it
  invokes the captured `IntelAccelerator::stop` route with the same null
  provider used by Tahoe's common start-failure cleanup. That wrapper first
  publishes `gVfDeviceStopping`; native `finishAllStamps` remains intact, and
  the routed `stopGraphicsEngine()` must retire every direct context, perform
  the final heavy GuC TLB invalidation and establish `gVfDmaQuiesced` before
  the original start failure is recorded as terminal. Failure to establish
  both boundaries is fail-stop rather than a return into object teardown with
  live DMA.
- The lifecycle binary contract now proves the complete retained native
  bootstrap skeleton instead of checking only its top-level calls. Scheduler-4
  virtual slot `0x220` must remain `loadFirmware`; scheduler initialization must
  enter the software-only base scheduler constructor and construct command
  streamers; GuC initialization must retain work-history, doorbell, CTB,
  interrupt, firmware-load, transport-registration and UK-context descendants;
  and interrupt registration must still reach the native bridge. The contract
  follows work-history allocation into the mapped-buffer factory, CTB allocation
  through transfer-ownership, and the second-channel registration failure into
  deregistration and its MMIO sender. The routed firmware loader's deliberately
  retained `initSchedControl()` subtree is fixed as well: context-pool, log and
  additional-data allocations all terminate in mapped buffers, and every
  conditional ownership transfer remains intercepted. The wrapper must reject
  every partially populated native storage field that Tahoe itself fails to
  propagate. Every hardware-facing endpoint in that graph, including doorbell
  discovery and all GGTT map/unmap variants, is also required to appear in the
  VF route inventory. This prevents a future Tahoe payload or route edit from
  silently exposing the original physical GuC/MMIO implementation.
- Targeted source/Mach-O lifecycle checks pass for both pinned accelerator
  payloads. This is an offline proof only. No candidate was installed, no VF
  binding was changed, and the VM remained shut off after the host i915 crash.
  Dynamic validation remains prohibited by the V251 hold and the independent
  requirements in `HOST_CONTAINMENT_PLAN.md`.
- Driver correction `e3ef20d`, retained-descendant contract `d5a1786` and
  scheduler-storage audit `0858b97` all passed GitHub Actions (runs
  `36329893491`, `36330137491` and `36330311904`). The final run's release zip
  SHA-256 is
  `738fd0ae9f370c16f2b19b75807288ca136a353f70155ae4eb6433029716cc03`;
  its kext executable UUID is `9773B4C9-FF71-36B0-8107-910C0F318EC5` and
  SHA-256 is
  `db61632051011783ebd96506d33c3dc51c16991a47dbe324ed0a089fde7c3d46`.
  The artifacts are archived under `../../build/artifacts/0858b97` only. They
  have not been installed, added to an AuxKC or loaded by the VM.

## V253 native stop reachability review (offline)

- Checkpoint `d23f7a7` passed GitHub Actions run `36330781623`. The archived
  release zip SHA-256 is
  `9698ddeb9e3c58f0565df71f2c4bc33286b2e495648c81225babecb172900ebc`;
  its kext executable UUID is `9773B4C9-FF71-36B0-8107-910C0F318EC5` and
  SHA-256 is
  `db61632051011783ebd96506d33c3dc51c16991a47dbe324ed0a089fde7c3d46`.
  The Metal smoke executable SHA-256 is
  `b69ef075a8062de2f94bfa30a4e8242ba5b11f695c4dae04bd003a4efca294d5`.
  These files are archived under `../../build/artifacts/d23f7a7` only; none was
  installed, added to an AuxKC or loaded by the VM.

- The V252 rollback deliberately enters Tahoe's complete
  `IntelAccelerator::stop`, so its ordering was audited rather than assuming
  the routed `stopGraphicsEngine()` entry was the whole teardown. Before that
  engine boundary the pinned body finishes software event stamps and disables
  trace collection. The latter reaches `IGTelemetryManager::disableCollection`,
  which only sets its local disabled field for the stop-time option; it does not
  read GPU time registers or MMIO.
- Both admitted payloads have exactly one direct `stopGraphicsEngine()` call in
  native accelerator stop. The wrapper publishes `gVfDeviceStopping` before it
  enters that body. The routed engine stop must complete direct-context
  retirement and the heavy GuC TLB boundary before disabling the interrupt
  bridge and IOAccelerator. Native trace shutdown, sysctl removal and every
  canonical OSObject release in the stop body occur after this boundary.
- The retained free chain was checked separately. `IGHardwareGuC::free()` may
  deregister both CTB channels only through the routed MMIO sender, unregisters
  software interrupt sources, and releases mapped storage only after the outer
  stop has established DMA quiescence. Its conditional legacy ownership calls,
  and the same call in `IGHardwareGuCCTBuffer::free()`, remain intercepted by
  the VF ownership route. Existing UUID-bounded patches still remove the two
  native CTB-free `0xCEE8` physical-TLB accesses.
- These stop/free edges and their order are now executable Mach-O/source
  contracts for both payloads. This is not proof that a wedged PF will respond
  to shutdown; the host-side containment hold remains mandatory and no dynamic
  operation was performed.

## V251 host incident: wrong interrupt ABI selected on Raptor Lake

- The guest configured memory IRQ at `18:01:42.498967` (`page=0x402bc000`,
  GuC status `0x402bc190`, source `0x402bc419`). The host's first PF
  `00:02.0` DMAR write fault to address zero followed at `18:01:43.272503`.
  The first direct LRCA submission did not occur until `18:02:22.813452`.
  Therefore direct Metal/context submission cannot explain the first fault;
  the failure starts immediately after the memory-IRQ KLVs and accelerator IRQ
  enable. Guest evidence is in `../../build/diagnostics/ce166c8-runtime1`; host
  evidence is in `../../build/diagnostics/ce166c8-host-pf-hang`. The captured host
  kernel log SHA-256 is
  `fa934f74c6080459f256d936fc1e75d36a79051e3b72a99aef2c36c5ca338a97`.
- Installed i915 source `i915-sriov-dkms-2026.03.05.7` maps RPL-P device
  `8086:a7a8` to `adl_p_info`. That table inherits `GEN12_FEATURES` but does
  not set `has_iov_memirq`; only `mtl_info` sets both `has_iov_memirq` and
  `has_memirq`. `HAS_MEMORY_IRQ_STATUS` additionally requires a VF. Thus
  TGL/ADL/RPL VFs use `gen11_irq_handler` and the virtual `0x1900xx` register
  block; only MTL/ARL select memory IRQ.
- The old bridge unconditionally sent KLV `0x0901/0x0900`, rewrote LRCA image
  dwords `0x50..0x59`, replaced the native filter/read/enable/disable methods,
  and removed `GFX_MSTR_IRQ` access. Those operations implement
  `intel_iov_memirq` correctly only after the missing capability gate. On this
  RPL-P VF they selected an unsupported protocol and are the root-cause-level
  match for the observed timing.
- Pinned Tahoe disassembly shows that its native `IGInterruptBridge` implements
  the Gen11 VF protocol directly: master `0x190010`, bank status
  `0x190018/1c`, selectors `0x190070/74`, identities `0x190060/64`, and the
  enable/mask ranges `0x190030..0x190100`. Every encountered address is in
  i915's `vf_accessible_regs`. The only unsafe nested path is
  `IGScheduler4::{enable,disable}ErrorInterrupts`, which iterates command
  streamers and writes physical per-engine `RING_*` registers.
- V251 classifies interrupt transport by exact PCI ID. TGL/ADL/RPL retain the
  complete native bridge, including logical scheduler callback registration,
  and route only the two physical error helpers to a no-op. MTL/ARL retain the
  memory-backed routes. Memory-IRQ KLV configuration, page consumption and
  LRCA mutation now all fail closed unless `hasIovMemoryIrq(device)` is true.
  Submission readiness uses the selected transport rather than assuming
  memory IRQ on every VF. Disabling either transport immediately closes new
  GPU-producer admission, while bounded synchronous CTB polling remains
  available long enough to drain teardown completions.
- Offline contracts enumerate the native Tahoe bridge MMIO operands and reject
  any address outside i915's VF allowlist. Capability tests exhaust all 65,536
  PCI IDs and explicitly prove `a7a8` is virtual-MMIO while MTL/ARL are
  memory-IRQ devices. No VM or VF dynamic operation is permitted in this
  checkpoint.
- Correction commit `8c45437` passed GitHub Actions run `36328606392`, including
  the complete static/analyzer/sanitizer suite and native x86_64 build/link. The
  resulting kext UUID is `C238E026-440F-3340-98EB-9D954FC83D2D`; executable
  SHA-256 is
  `e15c29b099b54b18d9e45f81674634c8cf728f3e479b85b940f01dac755eeb25`
  and artifact zip SHA-256 is
  `a4ba9a56bce112a231026d8e315ff81f1fbabdd0ad6ad4d78267637ac85892d2`.
  The artifact is archived under `../../build/artifacts/8c45437` only. It has
  not been installed, added to an AuxKC, or loaded by the VM.

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
- config-read wrappers reject >16-bit spoof IDs and restrict substitution to
  the actual selected IOPCIDevice, not any name beginning with IGPU. The first
  implementation incorrectly modeled a 32-bit read at offset 2 as a sliding
  device/command window. PCI config reads are naturally aligned: bits 1:0 are
  ignored for a DWORD and bit 0 for a word. It also failed to distinguish an
  extended config page whose low offset happens to be 0..3.
- The shared pure helper now models those alignment and extended-page rules for
  both 16- and 32-bit reads. It passes all 65,536 device IDs, 16 pages and 256
  low offsets plus invalid-ID passthrough. This tests returned values, not
  physical PCI writes (none are performed by the helper).
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

### Preserve global IOAcceleratorFamily validation

- Removed the active global two-match masked branch bypass in
  IOAcceleratorFamily2 and the set_id_mode hook that cleared 0xff8073c0 for
  every surface whenever the CPU was not TGL. Neither was scoped to the
  selected PCI device/VF, and the former's comment referenced Sonoma rather
  than the target Tahoe binary without a payload/function allowlist.
- The surface mode API is documented in Apple's IOAccelSurfaceConnect.h:
  https://raw.githubusercontent.com/apple-oss-distributions/IOGraphics/main/IOGraphicsFamily/IOKit/graphics/IOAccelSurfaceConnect.h
  It contains surface color-depth/window/stereo/synchronization flags; the
  code's claim that masked bits were TGL-only GuC scheduling/preemption bits
  was unsupported. The vendored SDK also labels 0x4000 Surface2, included
  in the old destructive mask. Native mode/capability rejection is retained.
- Removed the dormant deviceStart failure-to-success wrapper, its unresolved
  original slot and commented obsolete f1/lock-route block. Git preserves all
  removed material. This does not prove native Tahoe accepts the driver yet:
  unsupported modes must be handled using correct caller/capability contracts,
  not by globally modifying unrelated GPU user clients.
- Full suite passes in /tmp/ngreen-static.vIOg5y. CI dccbe67 (36220356972)
  and f339637 (36220434223) succeeded. No VM or hardware access for tests.

### BAR mapping failure and publication

- setRMMIOIfNecessary now returns failure for missing PCI provider, mapping
  failure, zero/unaligned virtual address or a mapping shorter than a dword,
  rather than dereferencing a null map. An unpublished invalid map is released.
  It does not initiate a new map from interrupt context/disabled interrupts.
- Concurrent bootstrap callers atomically publish one lifetime-long map and
  its derived MMIO pointer; losing callers release only their unused mapping.
  No valid live mapping is replaced or freed. VF identification/mailbox/GGTT
  callers propagate failure. Required physical kext setup fails closed with
  a deliberate panic rather than continuing into unguarded native hardware
  routines. That is containment, not graceful device recovery.
- BAR2 diagnostic mapping now independently requires physical identity,
  even if a caller forgot its VF guard. Existing call sites still require
  aperture pointer/length checks; BAR2's own allocation concurrency and other
  direct/raw mapping paths are not certified by this change.
- Moved dynamic accelerator personality publication to the end of each
  hardware payload patch branch. addDrivers may trigger matching; it must
  not publish our new personality before required routes/patches are ready.
  Stock personality/start ordering and allocation failure still need review.
- Complete suite passes in /tmp/ngreen-static.RpztYc. IOAccel validation
  checkpoint e662427 CI 36220510624 succeeded. No runtime map fault injection
  or VM startup; preemption-disabled (but interrupts-enabled) mapping contexts
  and mapping lifetime across device removal remain open.

### Retired network logger and review coverage ledger

- Read kern_netdbg.cpp/.hpp completely. They were absent from Xcode Sources,
  had no live includes/callers, and were excluded by the offline syntax loop.
  Removed both, their two commented references and the syntax exclusion.
  All ten remaining top-level C++ units are now checked with no exclusion.
- Dormant logger defects included vsnprintf's would-have-written length used
  as the socket-send length beyond the 2048-byte allocation, shadowing the
  static port with a local variable, positive errno handling as if only -1
  were failure, repeated connects, unchecked/racy lock allocation and blocking
  network operations while holding a shared lock. No runtime logger path was
  removed; Git preserves it. These defects do not establish a freeze cause.
- Added SOURCE_REVIEW_COVERAGE.md to separate inventory, complete file reads,
  partial reviews, fixes and remaining open obligations. Expanded inventory
  at c312229 was 1305 source/build/metadata files; deletion leaves 1303.
  Full SDK/HookCase/Lilu/all-source review is explicitly still incomplete.
- Full offline suite passed in /tmp/ngreen-static.fixHJH. No VM boot.

### Preemption-disabled blocking contexts

- Checked XNU 12377.121.6 osfmk/kern/sched_prim.c: preemption_enabled()
  requires BOTH zero preemption level and enabled interrupts. Mach.exports
  exports _preemption_enabled, and the vendored kern/sched_prim.h declares it.
  This avoids unexported x86 get_preemption_level or guessed per-CPU offsets.
- vfCanUseSleepingLock and initial BAR0 mapping now reject non-preemptible
  contexts even when interrupts are enabled, retaining the interrupt-context
  check too. GuC synchronous waits also keep their workloop/gate checks.
- This closes the previously recorded missing entry check, not all possible
  lock-order/deadlock paths or a transition into atomic context inside callees.
  No runtime scheduling fault injection was performed. Full syntax/offline
  suite passed in /tmp/ngreen-static.qgjHzV; final kext linking remains CI's
  check. c312229 CI 36220723406 and d6b483e CI 36220850119 succeeded.

### Remove BAR-only GGTT transport inference

- Rechecked i915 intel_gtt.c i915_ggtt_require_binder: selection depends on
  direct-stolen access and MEDIA_VER_FULL==13.0, not BAR size. VF probe also
  explicitly has no GMADR aperture and uses nop_clear_range for both modes.
- Current bootstrap now requires one of 60 exact known media-12 PCI IDs
  (TGL/ADL/RPL) and a complete direct PTE window BEFORE RESET. The full
  65,536-ID classifier was compared against primary-source macro expansion.
- Removed fallback that sent an ABI-1.0 relay handshake merely because the
  direct BAR mapping was short. A short media-12 mapping fails; MTL/ARL are
  rejected before RESET until per-GT GMD/IP query and correct relay ABI are
  implemented. The user's 0xa7a8 remains in the direct transport set.
- Inactive relay helpers/shadow paths remain staged code, not supported
  admission: no bootstrap sets gVfBinderReady or allocates its shadow now.
  Their review/removal/reimplementation and media-13 support are unfinished.
  This corrects unsafe inference rather than delivering Gen11+ support.
- Full suite passes in /tmp/ngreen-static.M05QXM (classifier additionally
  source-compared in /tmp/ngreen-static.wBl01g). af82759 CI 36220976605
  succeeded, including kext link with the exported preemption check.

### Native GGTT DMA truncation and void-unmap admission

- Rechecked native mapRange 0x10324, unmapRange 0x10626 and mapRangeDummy
  0x10680: DMA address bits are masked with 0x7ffffff000. A wider/unaligned
  address was silently changed into a different DMA mapping. VF mapRange
  now validates the entire physical interval against that inspected 39-bit
  encoder; VF init similarly validates the dummy page. This is an encoder
  limitation, not a claim that modern Intel hardware only supports 39 bits.
- Added null receiver checks to VF map variants. New pure bounds checks pass
  121 high-address/alignment/overflow cases against 128-bit arithmetic, in
  addition to the existing GPU-range checks. These are not PTE flag/PAT or
  cache-coherency validation, nor a proof of the original mapping's ownership.
- VF void unmap now checks identity, transport/fault state and assigned GPU
  interval BEFORE native writes. Invalid/faulted unmap deliberately panics:
  silently skipping it would let callers free/reuse potentially live DMA
  backing. This is fail-stop containment, NOT graceful recovery/quiescence.
  Valid-range unmap still needs proven GPU idle, TLB invalidation and lifetime
  exclusion; the VM boot blocker therefore remains.
- Direct native PTE writes are still split high/low 32-bit stores. Rotated
  iterator arithmetic and physical iterator length, full object provenance,
  flags, and call-chain invalidation are still open. No runtime verification.
- Full suite passes in /tmp/ngreen-static.BeFUWU. Transport admission
  checkpoint 070cffa CI 36221130028 succeeded.

### Retired stubs and deeper context-allocation failure paths

- Removed unreferenced Gen11 wprobe, tgstart, initializeLogging and
  isConflictRegister stubs and their unused original slots/state, plus the
  unused shouldForceRPLBringupPlatform helper. Genx's active probe is retained.
- Removed the redundant native-result-preserving deviceStart hook, its slot
  and stale force-success messages. Native startup failure remains native;
  removing this hook is not evidence that other startup patches are safe.
- Further native workqueue inspection found init at 0x1e3da acquires its lock
  at 0x1e419, then a failed shared-buffer allocation branches from 0x1e43e
  to failure without the success-path unlock at 0x1e4ab. withOptions releases
  failed initialization, and free at 0x1e4e0 reaches IOLockFree. Together with
  createUkContext's unchecked withOptions result at 0x20628, this remains an
  OPEN allocation/unwind blocker; no partial null-check fix is claimed.
- getMemory at 0x13e8e returns IGAccelSysMemory, not IOMemoryDescriptor;
  getMemoryDescriptor at 0x13e9c performs the additional dereference. Any
  replacement must respect the inspected private type and calling convention.
- Stub-removal offline suite passed in /tmp/ngreen-static.FeplV2; the final
  deviceStart hook removal requires a fresh suite. No deployment or VM boot.
- Follow-up found the short-JE BCS readiness bypass still ACTIVE in
  patchesRPL, independently of the earlier removed force-success wrapper.
  Removed that patch and both short/long pattern sets and commented fallback.
  Earlier removal of fabricated return values did NOT restore this binary
  gate. Keeping the native branch can expose startup failures previously
  hidden; it does not implement BCS or establish readiness of other engines.
- Final full offline suite passes in /tmp/ngreen-static.RAACIo. VM remains
  shut off; no guest binary was replaced.

### Rotated GGTT iterator is not bounded by its declared interval

- Re-read complete native 0x103a0..0x10612. Initial destination is read from
  iterator+0x18 at 0x10535 and written at 0x1055c/0x10561 without a range
  check. Division by iterator+0x0c occurs only afterward at 0x10575.
  Later arithmetic clamps to the EXCLUSIVE end at 0x1058c; the physical
  segment loop can continue and write at that end. Physical addresses also
  pass through the same 39-bit mask without prevalidation of every segment.
- Therefore the previous declared-range check did not contain these writes.
  Non-physical rotated mapping now fails and faults before dereferencing
  either private iterator. Physical native behavior is preserved. This is
  explicit unsupported-path containment, not a functional rotated mapper;
  implementing validated iteration and caller failure/quiescence handling
  remains necessary before accelerated display/VM tests.
- Full offline suite passes in /tmp/ngreen-static.L605pe; no dynamic test.

### submitBlit return ABI and false-success rejection

- Complete native submitBlit at 0x2bc8e returns a boolean in AL: disabled
  capability clears r15d at 0x2bca8; an empty rectangle list reaches true at
  0x2bded; both completed native submission branches return r15b=1. Upper
  bits are not a valid unsigned-long status (r15 previously held a task).
- Corrected hook declaration/definition from unsigned long to bool. Every
  rejection/no-submission branch now returns false, including old mode 2,
  routeSel=3, invalid task and missing original. Removed IOReturn error-code
  returns: kIOReturnUnsupported ends in 0xc7 and is not boolean failure.
- Correction to historical comments: default mode 1 returned zero, which
  is boolean FAILURE, not fabricated IOReturn success. Mode 2/routeSel=3
  returned true without work; unsupported-code paths returned invalid bool.
- This is NOT sufficient end-to-end failure propagation. Direct callers
  at 0x6e38 (blitCopy), 0x55b04 (MSAA resolve), 0x820dd (copyBufferDMA),
  0x82a65 (submitSwapFlush) and 0x8351e (submitCopyForward) ignore the result.
  submitSwapCopy at 0x2d0a6 preserves it. All caller/event/stamp paths and
  task+0x298/global cached context ownership remain open review blockers.
- Full suite before redundant-branch cleanup passed in
  /tmp/ngreen-static.j5pPjI. c6f3442 CI 36221767518 and ea1ff8e CI
  36221834707 succeeded; no guest deployment or VM boot.
- Final cleanup suite passes in /tmp/ngreen-static.WaL4Cq.

### Typed parameter header: completed read, incomplete ABI verification

- Read all 624 lines of AppleIntelParams.hpp. Assertions cover selected
  early fields, not every generated tail. Clang record-layout dump shows:
  controller unk_0F0D actually 3856 (0xf10), unk_13CF 5076 (0x13d4),
  unk_14B2 5304 (0x14b8), unk_1541 5452 (0x154c), unk_1B17 6948 (0x1b24);
  framebuffer unk_4289 actually 17036 (0x428c), unk_44DE 17636 (0x44e4),
  unk_4B8C 19348 (0x4b94). Natural scalar alignment shifts subsequent fields.
- Whole main-source search found no direct references to these named fields
  outside their declarations. This does not validate raw-offset accesses or
  prove that every other field matches the payload. Do not add packed to
  guess widths: several fields may really be bytes, as prior fixes indicate.
- Generator extract_apple_params.py is only partially reviewed; its old
  AppleIntelPlaneRegCache register mapping at 0x100/0x104/0x154 conflicts
  with the current header's warning that those are inline plane members.
  Regeneration must NOT overwrite the reviewed header without reconciliation.
- Header blit3d_params_t.unk_00B8 is uint32_t, while inspected submitBlit
  reads/writes a byte at 0xb8. Exact full layout, generated provenance and
  all private object lifetimes remain open. No unproven packing/type rewrite.
- Info.plist read completely (124 lines). Its NootedGreenDriverProfiles,
  NootedGreenDriverProfileDefault and PreferredOrder have no consumer found
  in main source/tools; these metadata declarations do not implement profile
  selection or broad GPU support. SchedulerType does have a source consumer.
  Display injection is restricted by its vendor/product pair in probe, not
  a virtual-display implementation. Main product/bundle build settings match
  the personality placeholders. Runtime dependency/version and display
  behavior remain unvalidated; metadata retained rather than guessed away.

### Workqueue failed-init lock and accelerator-reference unwind

- Rechecked complete pinned workqueue init (0x1e3da..0x1e4c3) and factory
  (0x1e37e..0x1e3d9). Exactly two false exits exist: failed IOLockAlloc,
  or failed buffer allocation while owning the new IOLock. Accelerator+0x10
  was retained before either failure. A successful init releases its lock.
- Added a mandatory VF-only init route behind the existing payload UUID
  check. On false, require buffer+0x30 null; unlock/free/null the fresh lock
  and null/release the retained accelerator before native factory release.
  Unexpected failed state with a buffer is fail-stop, not guessed teardown.
  Successful initialization and physical GPU entry points are unchanged.
- Shared helper tested with all eight accelerator/lock/buffer presence
  combinations, expected callback ordering, untouched rejected state and
  repeated cleanup (no double unlock/free/release), under ASan/UBSan.
  Full syntax/offline suite passed in /tmp/ngreen-static.HggG00; CI includes
  the new test. These are model/compile checks, not live OOM fault injection.
- Confirmed superClass relocation at 0xd04a8 is OSObject, constructors use
  OSObjectC2 and native queue free returns without superclass free or an
  accelerator release. XNU OSObject::free performs instanceDestructed and
  delete. This patch fixes failed-init retained resources only: failed object
  destruction, successful queue teardown, createUkContext's unchecked null
  and reserved-ID rollback remain OPEN. VM is not ready for dynamic testing.

### Destroy only explicitly marked failed workqueue objects

- Resolved exported __ZN8OSObject4freeEv from KernelID (XNU Libkern.exports
  line 388), without guessing a private vtable slot or fake subclass cast.
  The pinned queue derives directly from OSObject; its deleting destructor
  at 0x1e268 calls OSObjectD2 and operator delete with size 0x48. XNU's
  allocation path uses Z_WAITOK_ZERO, matching the inspected fresh fields.
- VF init now rejects nonempty reinitialization. Early argument failures and
  successfully unwound native failures mark the unused process slot with an
  invalid pointer sentinel. The VF virtual-free hook recognizes ONLY that
  marker, requires accelerator/lock/buffer all null, consumes the marker,
  and calls OSObject::free exactly once without further object access.
  Normal queues still take native free; their DMA teardown is NOT fixed.
- Added all 24 resource-presence/process-marker combinations and repeated
  consumption tests. An initial test compilation exposed mixed nullptr/void*
  initializer-list deduction; corrected to explicit void*. Final complete
  suite passes /tmp/ngreen-static.B313NQ. 53c8b8b CI 36222252110 succeeded.
- Failed fresh queue resources/object now have a statically checked unwind;
  caller createUkContext still dereferences the factory's null result and
  leaks its reserved ID on other failures. No guest deployment or VM boot.

### Scheduler prerequisites and discarded allocation failures

- Native GuC init only allocates +0x40/+0xa08 locks when initInterrupts
  succeeds, and does not validate both lock-allocation results before loading
  scheduler storage. VF load now requires a non-null owner, both locks and
  a sleepable context before calling initSchedControl. VF create also requires
  confirmed Virtual identity and a pool lock before native allocContext locks it.
- initSchedControl at 0x20a9c checks setupContextPool but ignores boolean
  failures of setupLogBuffers (0x20af1) and setupAdditionalDataStructs
  (0x20af9), then unconditionally sets true at 0x20afe. Added postconditions
  for metadata, pool, log backing/descriptor/mapping and additional backing.
  This detects partial allocation, not complete mapping validity or graceful
  recovery. Faulted teardown may still deliberately fail-stop at GGTT unmap.
- Full suite passes /tmp/ngreen-static.echAXc. aed6ef4 CI 36222441174 passed.
- Further CTB init inspection found distinct unresolved OOM defects: failure
  of its second lock frees the first without clearing the pointer; buffer
  failure returns with BOTH locks held. CTB free also unconditionally releases
  accelerator+0x10, making pre-native rejection on a fresh object unsafe.
  These are newly identified blockers, not fixed by the workqueue-specific
  unwind. No live allocation failure tests or VM boot were attempted.

### CTB-specific failed-init unwind (not the workqueue contract)

- Rechecked CTB init 0x1f386..0x1f474 and free 0x1f564..0x1f600, including
  external lock relocations. If the second lock fails, 0x1f467 frees first
  lock but leaves +0x18 dangling. If backing allocation fails at 0x1f404,
  both locks remain held. Free iterates four lock slots and unconditionally
  releases accelerator+0x10; it omits superclass free as well.
- Added separate helper: second-lock failure clears the ALREADY freed first
  pointer without another free; backing failure unlocks in reverse order,
  frees/nulls both locks, then releases/nulls the accelerator. Impossible
  missing/aliased lock or non-null backing states are not guessed away.
- Fresh pre-native rejection and successfully unwound failures mark +0x90.
  VF-only CTB free accepts that marker only with no active/pinned CTB identity
  or retained resources, consumes it, and calls OSObject::free. Superclass
  relocation 0xd0be0 names OSObject; deleting destructor 0x1f240 invokes
  class destructor (including its vector) and operator delete for size 0xa8.
  Initialized/unmarked CTBs retain their old path and quarantine policy.
- All 16 accelerator/H2G/G2H/backing combinations plus duplicate-lock
  rejection tested under sanitizers; callback sequence and repeated cleanup
  checked. Full suite /tmp/ngreen-static.0GQQlN passes. 0aecf94 CI
  36222628968 passed. VM remains off; no live OOM injection/deployment.
- Channel-init pre-write address checks and post-channel protocol-failure
  teardown still need follow-up; these changes do not prove CTB lifecycle
  safe in all states or close the pending context-creation blockers.

### CTB addresses validated before any backing write

- Native ctChannelInit at 0x1f488 calls getVirtualAddress then memset through
  that pointer before the old wrapper checks it. Its descriptor writes also
  truncate GPU virtual addresses to 32 bits, so recovering a base from the
  descriptor could conceal a high-address alias. Re-read its complete body.
- VF channel setup now obtains CPU and full-width GPU addresses directly
  from resolved, inspected accessors (0x10b60 and 0x13e7c). Requires backing
  identity/length and the GPU mapping object before invoking accessors;
  validates CPU page alignment/overflow, allocated length, full assigned
  GGTT interval and exclusive GuC top BEFORE clearing/writing backing.
  Native channel init is preserved for PF only, not called by VF anymore.
- Recreates the native +0x88 state copy, initializes modern channels 0/1,
  and explicitly nulls unused channel aliases 2/3. Publication still follows
  complete layout initialization and retained CTB/backing ownership.
- A native init that succeeds but then fails layout validation is retained
  once if channel setup did not already pin it. This deliberate quarantine
  prevents legacy free/unmap after fault, not a memory-reclamation solution.
- Added 257,049 CPU/GPU interval cases checked against 128-bit arithmetic,
  plus valid 32-KiB mapping and same-low-32-bits/high-GPU-address regression.
  Full suite passes /tmp/ngreen-static.efNv3E. 413a577 CI 36222819921 passed.
  Physical mapping provenance, DMA coherency/quiescence and real allocation
  fault injection remain unverified. VM off; no deployment.

### Bounded legacy proxy ID retirement

- Native releaseContextId 0x2117c indexes pool/metadata without validating ID,
  reservation bit or used count, then decrements unconditionally. New VF route
  checks complete pool byte capacity, count<=1024, ID<count, nonzero bounded
  occupancy and allocated bit before mutation. Invalid/duplicate retirement
  is fail-stop; physical calls remain native. Valid retirement clears only
  bit 0, decrements once and clears the same two metadata pointer slots.
- Verified both direct callers already hold GuC+0x40: releaseContext locks
  at 0x211db; DetachContextDesc locks at 0x221aa before call 0x2221d. No new
  recursive locking. setupContextPool 0x20bef allocates count*0x20 metadata.
  This is proxy bookkeeping only, not GuC deregistration or DMA release.
- Added 4,096 exhaustive retirement cases, double-release attempts, all
  invalid-input branches, byte canaries and preservation of unrelated flags.
  Full suite /tmp/ngreen-static.vaG9Uj passes; 0919cd3 CI 36222972562 passed.
- Native detach still decodes/indexes its packed hash value BEFORE this
  retirement call; malformed IDs/engine slots can fault there. Therefore this
  bounds check does not validate its entire caller. createUkContext rollback
  and all concurrent hash/object lifetime obligations remain unfinished.

### CPU interval representability for proxy pool bookkeeping

- Both allocation and retirement now validate the complete CPU address interval
  before record pointer arithmetic, in addition to backing length and count.
  A non-null address near UINTPTR_MAX could previously wrap despite sufficient
  claimed byte capacity. The exclusive end must remain representable.
- Added synthetic-address regressions at counts 1, 2 and 1024; invalid calls
  preserve counters/cursors/output IDs without dereferencing these addresses.
  This does not establish mapping residency, provenance or concurrent lifetime.
  Full static/sanitizer suite passes /tmp/ngreen-static.kYN96n.
- 4a9c093 is confirmed on the remote branch; CI 36223149074 passed. VM remains
  off, and no new driver has been deployed.
- Further review found isRealTGL is still derived from guest CPUID model in
  kern_green.cpp, while many GPU topology/display/task hooks consume it.
  CPU model is not GPU identity; virtual CPU spoofing can select the wrong
  branch. Firmware admission already uses PCI identity separately. Global
  replacement is pending review of these heterogeneous consumers, not treated
  as a verified fix or evidence of successful PF/VF support.

### Packed context descriptor reads

- The pinned native binary places SGfxContextDescriptor at
  IGHardwareContext+0x89. Existing VF attach, detach, idle and submission code
  accepted that address as const uint32_t* and directly used descriptor[0/1].
  The address is intrinsically unaligned, so those C++ lvalue accesses had
  undefined behavior even though x86 movl tolerates unaligned memory.
- All local consumers now decode the two little-endian words through byte
  loads. Native PF calls retain their original ABI and pointer. Added 1,024
  bit-position/alignment cases (offsets 0..15) with input-preservation checks;
  full sanitizer/static suite passes /tmp/ngreen-static.lGyQyH.
- This removes the alignment assumption only. The native object's eight-byte
  readability, lifetime and immutability while the caller owns it remain ABI
  preconditions; byte loads do not provide an atomic snapshot.
- The same decoder now covers three legacy PF/RPL repair paths that read the
  descriptor directly at object+0x89. A separate four-byte helper removes a
  strict-aliasing assumption from the color selector diagnostic. DMC arrays
  are indexed in their declared uint32_t type instead of char-pointer recasts.
- Fixed a seven-argument/six-conversion surface diagnostic and unsigned-long
  start return formats, removed duplicate masked-write macros that weakened
  single evaluation, and made the buddy table standard C++14 aggregate data.
  The bit iterator now receives bits rather than bytes.
- Static checks now fail on Gen11 format, alignment or aggregate-order
  regressions locally and in CI. Added 80 unaligned little-endian word cases;
  full suite passes /tmp/ngreen-static.t5KKSp. These compiler properties do
  not establish MMIO protocol correctness or safe dynamic execution.

### Pre-native context attach admission

- Native AttachContextDesc indexes the shared proxy pool, an engine-group
  bitmap and an LRCA record before returning. The former wrapper called it
  first and validated the descriptor afterwards, so rejection could not
  protect those accesses.
- VF attach now requires Virtual identity, initialized transport/accessor,
  pool lock and metadata, mapped pool storage covering count*0x5b00, bounded
  used/next counters, a context backing object, complete assigned-GGTT/GuC
  interval, supported class and engine instance, and the minimum LRCA image
  length before invoking native code. Failed admission makes no native call.
- VF detach revalidates the immutable proxy-pool snapshot before the native
  private detach indexes it. Because this is void teardown whose caller may
  release DMA pages, corrupt state is fail-stop rather than a silent return.
  The native hash entry itself is still private and not independently decoded;
  hash allocation/corruption and full object-provenance proof remain open.
- Full static/sanitizer suite passes /tmp/ngreen-static.pHlcvS. 0e27f5c CI
  36223942541 passed. VM remains off and no kext was deployed.

### Transactional VF legacy proxy context creation

- Pinned createUkContext 0x204c4 reserves an ID before allocating its shared
  process backing. A backing OOM jumps straight to failure without releasing
  the ID. It also dereferences the workqueue factory result at 0x20628 before
  checking it; getMemory at 0x205b3 has the same unchecked result problem.
- The VF route no longer enters that function. Physical devices remain native.
  It resolves and reuses native allocContext/releaseContext and the two object
  factories, but owns the transaction: reserve ID; allocate one-page shared
  backing; validate full CPU/GPU mapping; obtain and validate the complete
  physical segment (the original checkpoint used a raw vtable slot; a later
  public-XNU-ABI substitution was also wrong and is superseded by the exact
  private-symbol correction recorded below); allocate/validate the
  8-KiB workqueue; only then publish the record and four metadata pointers.
- Every post-reservation failure releases queue, process backing and context ID
  in that order. OOM returns 0x400 without poisoning transport; malformed
  ownership/mapping marks a protocol fault before rollback. Packed process+4
  and record+0x1c fields use explicit little-endian byte stores, not unaligned
  C++ lvalues. Added read/write canaries across offsets 0..15.
- Native successful WorkQueue::free releases its mapped buffer and lock but not
  the retained accelerator or OSObject base allocation. On VF the legacy queue
  is never sent to GuC (direct-LRCA CTB submission is separate), so the wrapper
  verifies native cleared buffer/lock, consumes and releases the accelerator
  retain, clears the borrowed process pointer and invokes OSObject::free.
  Failed-init marker cleanup remains a separate state machine.
- Offline coverage now includes 176 unaligned read/write cases and 32 published
  workqueue destruction states in addition to failed-init/CTB unwind. Full
  suite passes /tmp/ngreen-static.1si7P1; 25d20b2 CI 36224136022 passed.
- This transaction still relies on the pinned Tahoe private object/vtable ABI
  and allocator/factory lifetimes. If a future path publishes the legacy WQ to
  GuC, separate firmware deregistration and DMA-quiescence proof is mandatory.
  No dynamic hardware result is inferred; VM remains shut off.

### Native Tiger Lake admission uses GPU/PF identity

- `isRealTGL` selected more than one hundred native topology, display, task and
  submission branches from guest CPUID model 0x8c/0x8d. A hypervisor CPU model
  neither identifies the passed-through GPU nor distinguishes a PF from a VF;
  CPU spoofing could therefore send a TGL VF through physical-only code or make
  a physical TGL use later-generation compatibility repairs.
- The compatibility field now becomes true only when the original PCI device
  ID is in the explicit Tiger Lake table and the fail-closed VF capability
  probe classifies it as a physical function. CPUID remains diagnostic only.
  Non-TGL devices short-circuit before the BAR-backed identity probe.
- Exhaustive tests cover both PF/VF arguments for every 16-bit PCI ID; native
  admission is exactly `physicalFunction && isTigerLake(id)`. Full suite passes
  `/tmp/ngreen-static.N78vyX`, including strict Gen11 warnings and sanitizers.
- This fixes the global selector, not the semantics of every branch that reads
  it. Those heterogeneous consumers remain in the all-source review ledger.
  No driver was deployed and the VM remains shut off.

### Retired unreferenced 2017 firmware headers and null build entries

- `IGGucBinary.h` and `IGHucBinary.h` contained roughly 244 KiB of opaque 2017
  Apple GuC/HuC arrays. Repository-wide symbol/include inspection found no
  consumer; Xcode only listed them in its Headers phase, so they could not
  participate in firmware selection, VF transport or runtime execution.
- Removed both headers and their file/build/product-header references. Also
  removed three PBXBuildFile records whose file references were already null.
  All deletions remain recoverable from Git. The active DMC blobs and native
  GuC firmware ownership are unchanged.
- This reduces obsolete review/package surface but is not firmware semantic
  verification or a hardware test. Native macOS project parsing/build remains
  the CI gate; VM stays shut off.

### Central VF MMIO admission boundary

- Per-call-site guards cannot contain a forgotten physical-register diagnostic:
  the common `readReg32`/`writeReg32` helpers previously allowed every aligned
  BAR0 offset present in the VF's 16-MiB mapping. That recreated the same class
  of risk the individual force-wake, engine and display guards are meant to
  prevent.
- Added the exact fixed Gen12 VF register ranges from the pinned i915
  `intel_uncore.c:vf_accessible_regs`. Common helpers now serve GGTT through the
  existing shadow transport first, then permit a direct VF access only inside
  that allowlist. A denied VF access faults the transport instead of silently
  continuing after a missing physical MMIO side effect. PF access is unchanged;
  an unknown identity remains fail-closed.
- Tests enumerate all aligned offsets through 0x1a0000 and require exactly the
  40 allowed dwords, with explicit holes, unaligned values, physical TLB and
  GGTT-offset rejection. This protects only calls using the common helpers;
  native binary accesses still depend on routed/byte patches and remain under
  review. VM remains shut off pending the complete static gate.

### Removed additional unrouted accelerator experiments

- Repository-wide reference review found no route or call site for the old
  `setAsyncSliceCount`, `initHardwareCaps`, WOPCM-range override and no-argument
  blit-support functions. One of them performed a raw private-pointer MMIO write
  and the others rewrote guessed TGL object offsets; none could affect the built
  driver because their original-function slots were never resolved.
- Removed those bodies, slots, their unused data table, the declaration-only
  reset hook and an unreferenced always-true stub. Active, signature-distinct
  blit capability routing and native firmware paths are unchanged. This is
  dead-code removal, not a claim that the corresponding hardware facilities
  are fully reviewed or implemented.

### Removed dormant register-access shims and repaired PF accessor capture

- A second reachability pass removed the unused instruction-scanning RCS
  bypass, forced depth/external-display/blit-success functions, no-op DBUF
  handlers, and unrouted 64-bit/register-access forwarding shims. Several had
  unresolved original slots or fabricated success values, although no active
  route reached them.
- The old `FBMemMgr_Init` body was also unrouted, so globals used by active PF
  plane/scaler hooks were never initialized there. Those hooks nevertheless
  overwrote native object fields with the null globals. Physical framebuffer
  controller/framebuffer entry points now capture the controller and its
  existing register accessor directly; plane/scaler repairs only write a
  non-null captured value and otherwise preserve native initialization.
- VF framebuffer probe/start is still rejected before any of these physical
  routes are installed. The PF repair is compile/static evidence only and must
  not be treated as physical display validation.

### Static-analyzer follow-up and disabled IRQ rewrite removal

- Re-ran Clang's path-sensitive analyzer over every built C++ unit. The only
  actionable pointer report was the physical register wrapper repeatedly
  loading the global NGreen singleton after a null check; it now captures and
  validates one local pointer before use.
- Removed the Gen11 engine-IRQ reprogramming function whose two routes had long
  been disabled after causing a boot hang, plus a disabled Genx function that
  called native subsampling detection and then discarded it to force true.
  These are distinct from the active VF memory-IRQ bridge.
- Removed diagnostic-only CDCLK-encoding locals that compile out with DBGLOG;
  the selected PLL-frequency argument and active CDCLK behavior are unchanged.
  The first fresh analyzer pass reduced the built-source findings from ten to
  one and exposed the same global-singleton pattern in the controller accessor.
- Both controller read/write accessors now capture and check their singleton and
  original route once. Missing originals use the bounded common helper. MMIO
  size is validated while still signed, and a final aligned dword at
  `size - 4` is accepted rather than incorrectly falling through. The final
  analyzer pass reports zero findings across all ten built C++ units in
  `/tmp/ngreen-analysis.afkusT`; this is not a concurrency or hardware proof.
- The same analyzer pass is now a mandatory part of `tools/check-static.sh`,
  and CI calls that single complete gate instead of maintaining a shorter,
  drifting copy of its tests. Any analyzer warning or nonzero analyzer exit
  fails the gate; native Xcode build/link remains a separate subsequent job.

### Synchronous GGTT unmap invalidation

- Re-disassembled the pinned Tahoe TGL binary.
  `IGHardwareGlobalPageTable::unmapRange` at `0x10626` only replaces PTEs with
  the dummy entry. The caller
  `IGHardwarePageTable::releaseRange` at `0x144ac` invokes that virtual method
  and then `IntelAccelerator::flushHardwareAfterGttUpdate` at `0x2d1d8`.
  The latter only ORs a pending bit at accelerator offset `0x1340`; it is not
  a synchronous invalidation or a DMA-completion boundary. The release chain
  continues through `IGMemoryManager::releaseFromPageTableForTask` at `0xf6aa`
  and `IGAccelMemoryMap::releaseFromGPUPageTable` at `0x11304`.
- Compared this with the pinned i915 `gen12vf_ggtt_invalidate`: a ready VF uses
  `intel_guc_invalidate_tlb_guc`, action `0x7000`, target type 3, heavy mode and
  `FLUSH_CACHE`, then waits for matching `0x7001`. Heavy mode guarantees that
  in-flight transactions are globally observed before completion. The exact
  request word is `0x80000003`; physical `GEN12_GUC_TLB_INV_CR` remains outside
  the admitted VF path.
- The routed GuC object is now captured from the UUID-pinned
  `hostToGuCAction` receiver and its identity must remain stable. Direct GGTT
  initialization publishes its global-page-table receiver too; map, dummy-map
  and unmap reject every other receiver before native code can index it.
- After a valid VF unmap, shadow relay (when applicable) is mandatory, direct
  PTE stores are drained with the same x86 `sfence` used by Apple's physical
  invalidator, and a serialized heavy GuC invalidation must receive its exact
  sequence completion before the void call may return. Failed enqueue,
  timeout, unsafe wait context, stopped transport or prior protocol fault are
  fail-stop: releasing backing without proof of quiescence would risk stale
  DMA and another host-wide freeze.
- Initialization rollback before CTB has ever been enabled is the only
  no-invalidation case. The irreversible `ever enabled` bit is published
  before submission admission, so a concurrent unmap cannot mistake an active
  or formerly active transport for early rollback. All 16 lifecycle-state
  combinations are exercised by the pure helper test. The full static gate
  passed in `/tmp/ngreen-static.AxDbow`, including zero analyzer findings,
  28,561 GGTT interval cases and the existing sanitizer suites.
- This closes the valid active-transport unmap ordering hole; it does not make
  device shutdown graceful. CTB/memory-IRQ callback synchronization, engine
  stop and release of quarantined backing remain open. Once CTB is stopped,
  an attempted GGTT release deliberately panics rather than authorizing DMA
  reuse. The VM therefore remains off pending that teardown proof and the
  remaining all-source review.

### VF interrupt callback drain barrier

- CTB teardown now closes a single atomic callback-admission gate before
  firmware transport disable. Hardware filter, software GuC event handling and
  the direct readAndClearInterrupts path all acquire a counted token before
  touching the shared CTB/memory-IRQ backing.
- Gate closure preserves callbacks already admitted and rejects every later
  entry. Teardown waits for the admitted count to reach zero before accepting
  the first legacy CTB deregistration. The second legacy channel
  acknowledgement requires both firmware disable confirmation and a drained
  callback gate.
- The pure state machine exercises 8,194 open/closed/count combinations plus
  saturation, underflow and close-with-two-active-callback transitions under
  ASan/UBSan. This proves the CPU callback lifetime barrier only. It does NOT
  prove engine stop or that GuC has stopped writing the memory-IRQ page, so the
  CTB/memory-IRQ backing remains quarantined and VM startup remains blocked.

### CTB producer/consumer teardown ordering

- Teardown now has a distinct submission-stop phase. New register/enable/schedule
  work is rejected, while context disable/deregister and TLB invalidation remain
  admissible long enough to receive their required G2H completions.
- The H2G queue lock is the final producer linearization point: the transport is
  marked stopped only after the H2G ring is empty, G2H reply credits are zero,
  and no synchronous TLB completion is outstanding. IRQ callback admission is
  closed only after that boundary, preventing teardown from starving its own
  MODE_DONE/DEREGISTER_DONE/TLB_DONE waiters.
- The memory-IRQ enable vector is masked before callback drain and CTB disable.
  This is not a device-DMA-stop acknowledgement. In-flight GGTT operation
  synchronization and a verified VF reset/device-quiescence boundary remain
  blockers before any VM boot test.

### Context retirement and final VF DMA boundary

- Re-disassembly of Tahoe's `IntelAccelerator::stop` shows that it first calls
  `finishAllStamps`, later invokes `stopGraphicsEngine`, and only afterward
  releases scheduler/GuC and interrupt objects. The new accelerator-stop route
  records final-stop intent without blocking the stamp drain. At the later
  engine-stop boundary, it closes a counted attach/detach/submit gate and waits
  for all already-admitted operations before taking a stable context snapshot.
- Every direct-LRCA context state now has one explicit shutdown action. Enabled
  contexts receive MODE_DISABLE and wait for MODE_DONE; registered/disabled
  contexts receive DEREGISTER and wait for DEREGISTER_DONE; already-pending
  transitions must finish before classification is repeated. Tombstones keep
  their backing pinned until the last native reference is detached. A pure
  model exhaustively checks all nine states and the full
  enabled-to-tombstone sequence.
- After all contexts are deregistered, the driver issues the existing heavy
  GuC TLB invalidation and waits for its exact sequence completion. Only then
  does it publish the final DMA-quiesced boundary. Later teardown unmaps can
  skip per-range invalidation because new submission/mapping is closed and no
  GuC context remains capable of consuming a translation. The GGTT lifecycle
  model now checks all 32 transport/fault/quiescence combinations.
- CTB deregistration also invokes this sequence as an idempotent partial-init
  fallback before sealing H2G, masking memory IRQs and draining callbacks. The
  CTB object and shared memory-IRQ backing remain deliberately retained: the
  context/TLB proof does not establish that firmware can never write transport
  bookkeeping after its disable acknowledgement.
- Tahoe's native engine-start/stop success convention is nonzero; the prior VF
  bypass returned `kIOReturnSuccess` (zero), which is failure under that ABI.
  Both VF bypasses now return one. Physical-device engine bodies are unchanged.
- Full syntax, zero-finding Clang analyzer, strict ABI warnings and all
  sanitizer protocol suites pass in `/tmp/ngreen-static.H6UvwS`. This closes
  the identified graceful context/GGTT teardown ordering gap, but remains
  static evidence. The remaining all-source review, CI/Xcode linkage and a
  controlled boot are still required before dynamic hardware claims.

### Validated VF rotated GGTT mapper

- Reconstructed the complete private call contract from Tahoe
  `IGHardwarePageTable::commitRange` (`0x14090`) and
  `IGHardwareGlobalPageTable::mapRangeRotated` (`0x103a0`). The range iterator
  is 0x20 bytes with source counter/width/height and destination cursor at
  `+0x18`; the physical iterator supplies a prepared `IOMemoryDescriptor`, its
  page-rounded length and segment options.
- VF mapping now requires an exact `width * height` page matrix, a zero initial
  source counter, the expected initial rotated cursor, equal GPU/physical
  lengths, a range fully inside the PF assignment, and a stable global-page-
  table/PTE-aperture receiver. Every physical segment is preflighted for
  progress, page alignment, overflow and the pinned native 39-bit encoding
  before any store.
- The replacement writes one aligned 64-bit PTE per source page using the
  recovered permutation `column * height + (height - 1 - row)`. This removes
  the native split high/low exposure and the prior exclusive-end write. A
  second validated descriptor walk performs the mapping; an unexpected
  second-pass failure rewrites every already-touched destination to the pinned
  dummy page before returning failure. PF calls remain entirely native.
- The pure model verifies 4,096 width/height matrices and 4,326,400 pages as a
  one-to-one, in-range permutation, plus malformed assignment, geometry,
  cursor, length and overflow cases. This establishes iterator arithmetic and
  store bounds, not dynamic IOMMU coherency or successful rendering. The full
  syntax/analyzer/strict-ABI/sanitizer gate passes in
  `/tmp/ngreen-static.xUExOx`; macOS Xcode linkage remains the remote CI gate.

### Main-device admission, GMS sizing and patch-group preflight

- Read `kern_green.cpp/.hpp` again against Lilu's actual `routeVirtual`
  contract and Linux `fddfc3ec3179` (2026-09-26)
  `arch/x86/kernel/early-quirks.c` Gen9 stolen-memory decoder. Device discovery
  is mandatory for every later callback, so a
  failed `DeviceInfo::create()` now fails closed instead of leaving a null
  global GPU. Both PCI config virtual routes and their captured originals are
  also mandatory; their former ignored return values could admit a partially
  spoofed device. The impossible wrapper fallback now returns PCI all-ones,
  not a fabricated zero-valued configuration word.
- The old GMS decoder handled only `0x00..0x0f`, `0x20`, `0x30`, `0x40` and
  `0xf0..0xfe`. Intel's Gen9+ encoding defines every `0x00..0xef` value in
  32-MiB units, so legal values such as `0x10` (512 MiB) were silently reduced
  to the 128-MiB compatibility floor. The new pure decoder implements the
  complete primary-source formula and rejects reserved `0xff` or a byte count
  that cannot fit the private 32-bit immediate instead of wrapping. All 256
  encodings are compared to an independent 64-bit oracle; the existing
  instruction/corruption suite still covers every target register.
- BAR2 no longer publishes separately mutable pointer and length fields.
  Concurrent initializers atomically publish one immutable `IOMemoryMap`, and
  each caller derives a matched local address/length snapshot after validating
  address alignment and minimum size. A scanout probe's `surfAddr + size`
  check was widened to 64 bits as part of converting every caller. Interrupt
  or preemption-disabled contexts never initiate a potentially sleeping map.
- Removed Backlight, MCCS and IOGraphics branches that had never been
  registered since the repository's initial import, along with their dead
  wrappers, stale panel-data ownership bug, unused meta-class bridge and
  unused PCI/CPU fields. Legacy opt-in property seeding no longer repeats the
  same writes and now reports allocation/property failures instead of claiming
  a change unconditionally. `BIT`/`REG_BIT` use unsigned shifts.
- `LookupPatchPlus::applyAll` now validates every member's image bounds,
  signature and required count before the first mutation. This prevents the
  normal partial-group case in which a later Tahoe-version signature is
  missing. It cannot roll back a lower-level write/protection failure or a
  previously installed function trampoline; mandatory callers still fail
  closed. The optional TCON groups now check the result and never log success
  after a partial/failed group.
- The complete syntax gate, zero-finding analyzer, strict Gen11 ABI warnings
  and all sanitizer models pass in `/tmp/ngreen-static.BByji8`. This is static
  evidence only; no new kext was installed and the VM remained off.

### Display-merge publication and DYLD surface reduction

- Re-read `DisplayMergeNub.cpp/.h` end-to-end. The recursive merge now operates
  on a full provider property-table snapshot and publishes that table only
  after every local allocation, iterator and recursive operation succeeds.
  Newly introduced source dictionaries are copied and recursively bounded too,
  so mutable source dictionaries are not attached directly to the provider.
  The unused `start()` override, which returned success without calling
  `IOService::start`, was removed. This closes locally generated partial
  publication; it cannot serialize unrelated concurrent registry writers, and
  the legacy deliberate metaclass keep-loaded reference remains unchanged.
- Re-read `DYLDPatches.cpp/.hpp` end-to-end and removed every hardcoded Sonoma
  14.7.1 CoreDisplay prologue/control-flow patch. Those patches had no exact
  shared-cache UUID/build admission, were inactive on Tahoe, and included
  stubs that could falsify Metal success. The unreachable CoreLSKD branch,
  commented-out V50 bundle redirect, broad ICL Metal ID bypass and obsolete
  DYLD boot arguments were removed rather than silently enabled.
- The `_cs_validate_page` route now leaves ordinary executables and GPU bundle
  pages untouched. It retains only exact composite media-model string
  substitutions on shared-cache pages plus atomically gated bundle-discovery
  logs. Route state and inputs are checked, property-publication failure is
  reported, and patch helpers reject null/empty operands. These string patches
  still require controlled Tahoe media validation; they do not demonstrate
  Metal, decode or encode acceleration.
- README deployment/status text was corrected to stop advertising retired
  user-space hooks or CPUID as GPU identity. The current `isRealTGL`
  compatibility field is selected from original TGL PCI identity plus physical
  PF ownership; a VF never takes the native physical-TGL path.
- The complete syntax gate, zero-finding analyzer, strict Gen11 ABI warnings
  and all sanitizer protocol models pass in `/tmp/ngreen-static.dZrW0I`. No
  kext was installed and the VM remained off.

### Retired duplicate renamed-ICL handler

- Read `kern_genx.cpp/.hpp` end-to-end and traced every symbol/reference. This
  handler matched only a custom `/Library/Extensions` binary with bundle ID
  `com.xxxxx.driver.AppleIntelICLLPGraphicsFramebuffer`; it duplicated the
  maintained `Gen11` ICL implementation for Apple's real bundle ID. Nothing
  outside six unreachable Gen11 proxy wrappers depended on its implementation.
- The path had been present since the initial import and still actively
  replaced `hwSaveNVRAM()` with a constant-zero stub, suppressed client
  attribute `0x923` without a documented ABI, routed `ReadRegister32` through
  a behavior-free trampoline, and applied three old control-flow/platform-ID
  byte patches without UUID/build admission. Its hundreds of commented
  experiments and unsolved original pointers were not a defensible alternate
  PF driver.
- Removed the registration, dispatch, both sources, Xcode entries and the dead
  Gen11 proxy wrappers. The Apple-ID ICL and TGL handlers in `kern_gen11` remain
  available; VF framebuffer probe/start rejection there is unchanged. The
  obsolete runtime instruction rewriter used only by Genx was removed from
  `kern_dvmt_patch.hpp`; at that checkpoint the Linux-derived GMS decoder and
  its 256-value oracle test remained active (they were later removed with their
  last unconsumed caller, as recorded below).
- This deliberately drops compatibility with that unversioned third-party
  renamed bundle. The code is recoverable from Git history; reintroduction
  would require an exact binary identity, a documented need for each patch and
  native sleep/wake validation rather than restoring the unsafe stubs.
- The complete syntax gate, zero-finding analyzer, strict Gen11 ABI warnings
  and all sanitizer protocol models pass in `/tmp/ngreen-static.gYkH6h`. No
  kext was installed and the VM remained off.

### Gen11 relocation-reachability cleanup

- Compiled `kern_gen11.cpp` as an unoptimised Mach-O object with function/data
  subsections, then compared every defined `Gen11` method against its call and
  function-pointer relocations. Public `init()` and `processKext()` are expected
  cross-object roots; private methods with no relocation were then checked
  against the header and every route/solve table before removal.
- Removed 39 unreachable private methods plus their captured-original slots and
  stale route comments. They included constant-success panel/timing wrappers,
  forced 785.4-MHz timing mutation, manual IRQ reset programming, raw DBUF/MBUS
  programming with hardcoded four-channel LPDDR assumptions, a resume-time ring
  reinitializer, phantom-framebuffer stubs, an undocumented AGDC structure
  rewrite and another `0x923` client-attribute bypass. None had an installed
  route or live caller in the built object.
- Repeated the relocation pass after removal. Only the two cross-object public
  roots remained without an internal relocation; a newly exposed no-op
  `raReadRegister32` trampoline was removed in the second pass. The one live
  call to an always-true `isPanelPowerOn` helper was made explicit as the
  existing property publication, eliminating the misleading fake probe without
  changing that wake-stage behavior.
- This establishes source-level reachability, not semantic safety of the
  remaining active routes. In particular, active display, power, wake and
  accelerator hooks still require line-by-line contract review before boot.
- The complete syntax gate, zero-finding analyzer, strict Gen11 ABI warnings
  and all sanitizer protocol models pass in `/tmp/ngreen-static.TuPYo9`. No
  kext was installed and the VM remained off.

### Native lifecycle and protected-media restoration

- Removed the active `enableVDDForAux` route to an empty function and both
  production/non-production `hwShutdown` routes to a constant-zero function.
  Those replacements skipped native panel-power and shutdown work while
  falsely reporting success. The corresponding dead declarations and stale
  commented cold-function routes were removed as well.
- Removed four framebuffer sleep/wake routes which called through and then
  forced the unadmitted private field at `this+0x49e0` to `4` while publishing
  synthetic LCD power properties. The physical framebuffer binary is not
  protected by the accelerator UUID admission gate, so this layout write was
  not justified on Tahoe. Identified VFs are rejected before framebuffer
  routing; preserving the native PF lifecycle does not remove a VF feature.
- Removed the ICL and TGL PAVP callback routes which returned success for
  command `4` without performing the protected-media operation. Native PAVP
  handling now remains authoritative; acceleration readiness must not be
  inferred from a fabricated DRM result.
- Repeated the unoptimised Mach-O relocation audit after cleanup. All 130
  defined `Gen11` symbols except the expected cross-object roots `init()` and
  `processKext()` have an internal call or function-pointer relocation in
  `/tmp/ngreen-reach.fozKMQ`.
- The complete syntax gate, zero-finding analyzer, strict Gen11 ABI warnings
  and all sanitizer protocol models pass in `/tmp/ngreen-static.ki6tdp`. No
  kext was installed and `macos-tahoe-sriov` remained off.

### Native blit producer admission and task ownership

- Re-disassembled the UUID-pinned Tahoe TGL `IGAccelTask` lifecycle. Its four
  independently owned context slots are Blit2D `+0x290`, Blit3D `+0x298`,
  depth resolve `+0x2a0` and color resolve `+0x2a8`; `initWithOptions` clears
  all four and `release` separately notifies/releases them. The prior wrappers
  copied Blit3D `task+0x298` into unrelated base-class `task+0xb8` storage and
  could return the accelerator's borrowed kernel task after user-task factory
  failure. Both behaviors violated the inspected layout/ownership and are gone.
- VF `submitBlit`, `barrierSubmission` and their context getters now admit
  native CPU command production only when identity, GGTT, memory IRQ, both CTB
  mappings and enabled transport are live, while stopped/fault states are all
  absent. The pure gate exhaustively tests all 512 combinations and admits
  exactly the one complete state. No wrapper reports successful work without
  calling the native producer.
- The pinned `submitBlit` returns true for an empty vector before touching its
  parameter/task objects; real work selects a native 2D or 3D context and then
  dereferences that context's FIFO at `+0xb8`. The wrapper preserves the empty
  operation, validates real-work inputs, primes both possible native contexts
  without editing private task slots, and propagates the native boolean.
  Later-generation physical GPUs still explicitly reject the historically
  hanging route-selector 3 until generation-specific EU payloads exist; VF is
  allowed to validate it through the GuC/direct-LRCA bridge.
- The complete Tahoe `barrierSubmission` body gets Blit2D, Blit3D and depth
  contexts unconditionally and may get color, then performs real event/FIFO
  barriers. Both PF compatibility and VF paths now require those objects/FIFOs
  and call the original; the former V130 constant-success and warm-up modes are
  deleted. V120/V142 task substitution and return-mode boot arguments, plus the
  conditional physical BCS interrupt switch, are also deleted. Every admitted
  physical native producer now enables both RCS and BCS completion bits; VF
  continues to use only its isolated memory-IRQ bridge.
- `AppleIntelParams.hpp` now records all four verified task context slots
  instead of calling only `+0x298` a generic context. This evidence is confined
  to the pinned payload UUID; dynamic allocation, GuC completion and renderer
  behavior remain to be validated. The VM remains off until the remaining
  active-route semantic review is cleared.

### Accelerator payload identity and aligned-store patch determinism

- The TGL accelerator branch installs private-object routes and exact byte
  patches regardless of PF/VF ownership. Previously only a VF required the
  pinned Tahoe payload UUID; a physical GPU could apply the same offsets and
  instruction rewrites to an unknown binary. PF and VF now share the same
  fail-closed UUID admission. Supporting another OS payload requires adding
  independently verified ABI evidence, not relaxing this gate.
- Direct search of the pinned Mach-O found all aligned stores under review
  inside `blit3d_submit_rectlist` (`0x334b0..0x35960`): three contextual sites,
  six `[r9]`, four `[r9+0x20]`, and one `[r9+0x40]`, for 14 total. The former
  code applied one match, mutated the image, then used increasing skip counts
  against the shrinking match set. It therefore selected non-consecutive
  sites; overlapping V140/V141 patterns could no longer match after the generic
  opcode had changed, while their optional failures were only logged.
- The six non-overlapping signatures are now preflighted together against the
  original image with exact counts `1/1/1/6/4/1`, then applied as one mandatory
  group. Missing/version-mismatched sites stop payload admission. Redundant
  contextual/movapd declarations and inactive GT1/capability experiments were
  removed. This preserves the intended aligned-to-unaligned SSE store semantic;
  it is not proof that generated GPU commands execute correctly.
- The physical force-wake wrapper's legacy call-15 dump can temporarily remap
  `GGTT[0]` while collecting diagnostics. It is now unreachable in the default
  production path and requires the explicit experimental monitor opt-in. Full
  removal or a non-invasive snapshot remains part of the continuing PF audit.
- Full syntax, zero-finding analyzer, strict ABI warnings and all sanitizer
  models pass in `/tmp/ngreen-static.zeJcpr`. No VM or hardware test was run.

### Active-route and mandatory-lifecycle cleanup

- Removed three zero-effect accelerator routes and the unreachable
  post-return workaround tail in the extended-context wrapper. The VF context
  bridge now solves `IGMappedBuffer::getMemory` directly instead of routing a
  pass-through wrapper.
- Removed the ICL/TGL `initCDClock` and TGL hotplug-frequency pass-through
  routes. Native dispatch remains authoritative; the separate probe/sanitize
  mutation still requires its own review.
- Bootstrap symbols and accelerator start/stop lifecycle routes are mandatory
  for the UUID-pinned Tahoe payload on PF and VF. A partially solved private
  ABI can no longer continue to service publication or teardown.
- `/tmp/ngreen-static.FYOPcq` passes the complete static suite. The VM remains
  off because the physical start path still contains contradictory legacy
  GDRST, permanent EMR-mask and recurring IRQ-timer experiments.

### Removal of unaudited physical reset and partial-context protocols

- The remaining physical `start`/engine lifecycle was not a coherent hardware
  protocol: it could issue GDRST despite a later comment recording that GDRST
  permanently killed the ring, mask every error every 50 ms, rewrite BCS and
  CSB ownership behind the native scheduler, temporarily remap `GGTT[0]`, and
  retain recurring callbacks without a stop-side cancellation owner. Those
  experiments and their timers are removed.
- Physical engine lifecycle is native. VF engine lifecycle remains a contained
  success only because ring/reset ownership belongs to PF/GuC; final stop still
  fails closed unless every direct-LRCA context and DMA mapping is quiesced.
- Removed the hard-coded physical LRCA slot repair and partial Blit3D object
  construction. Native task context slots are no longer cross-substituted or
  cached without ownership. VF submit/barrier admission continues to require
  its complete native contexts and propagates native results.
- Static source search now finds no GDRST, global cache flush, blanket EMR
  mutation, GGTT page-zero remap or recurring thread-call allocation in the
  Gen11 implementation. `/tmp/ngreen-static.aikyPk` passes all checks. This
  clears that static blocker only; controlled Tahoe VF runtime remains pending.

### Tahoe submit backpressure contract

- Re-disassembled the UUID-pinned Tahoe payload. `IGScheduler4::push` directly
  returns `IGHardwareGuC::submitWorkItem`; its caller
  `IGHardwareRingBuffer::submitToRing` branches to a cold panic when that value
  is false. There is no equivalent of i915's stalled-request/tasklet retry.
- The VF CTB producer previously retried only eight exponentially delayed
  polls (about 12.75 ms total). Ordinary H2G ring pressure or outstanding G2H
  credits could therefore be misreported as a fatal submission failure even
  though the transport remained healthy.
- Producer admission now sleeps for at most one second while GuC consumes H2G
  and the independent G2H path returns credits. It never sleeps from an IRQ or
  non-preemptible context. Invalid descriptors still fail immediately; a real
  timeout records a protocol fault before returning failure, so the changed
  LRCA tail cannot be ambiguously retried after a fatal transport boundary.
- The circular-ring one-empty-slot rule is now a pure checked helper and is
  exhaustively compared with an independent cursor-walk oracle. This proves
  5,189,283 producer states in addition to the existing ring tests. The full
  syntax/analyzer/strict-ABI/sanitizer gate passes in
  `/tmp/ngreen-static.T5Wvol`. This proves CPU arithmetic and the bounded-wait
  decision only, not GuC forward progress.

### VF-only bootstrap task classification

- Re-review found that the V214 `isKernelGPUTask` wrapper used `isRealTGL` as
  its exclusion test. That flag distinguishes native Tiger Lake from a newer
  compatibility device; it does not distinguish an SR-IOV VF from its PF.
  Consequently a later-generation physical GPU with an unassigned
  `IntelAccelerator+0x150` could be given the VF-only synthetic kernel-task
  classification.
- The override now requires a VF identity proven from VF_CAP. Native Apple
  classification is authoritative for physical and invalid/unknown devices.
  A 32-state pure model confirms there is exactly one non-native admission:
  a present task and accelerator on a verified VF before its kernel task is
  assigned. This is a static identity boundary, not runtime task validation.

### VF memory-manager segment construction

- The pinned `IGMemoryManager::initSegments` body was re-disassembled. It only
  writes four `IGAddressRange` pairs at `+0xa0..+0xd8`, but derives the first
  two from physical stolen memory and BAR2. More importantly, its caller
  `IGMemoryManager::init` does not test the returned boolean before continuing
  into dummy-page and global-GTT initialization.
- The VF wrapper no longer executes those physical reads and then overwrites
  their result. It constructs global and allocator ranges solely from the
  PF-provisioned, page-aligned, below-4-GiB GGTT assignment; the 32-bit unified
  allocator is the intersection with Apple's `[1 GiB, 0xfe000000)` policy.
  The native 48-bit canonical PPGTT range remains unchanged because that is
  virtual address space, not the VF GGTT aperture. PF executes the original.
- An unusable VF segment plan is zeroed and records a protocol fault. This is
  required because a plain false return is ignored; the fault makes the later
  routed global-page-table initializer fail and causes native memory-manager
  unwind. A sampled 169-case model compares the plan to 128-bit arithmetic.
  The complete syntax, analyzer, strict-ABI and sanitizer suite passes in
  `/tmp/ngreen-static.kKR87B`; no VM or hardware action was taken.

### Removal of unreachable GGTT relay/shadow transport

- The VF admission path accepts only device IDs classified as known
  media-version-12 direct-GGTT platforms and requires the complete 16-MiB
  BAR0 mapping before RESET. Its successful endpoint always selected direct
  PTE writes. No code allocated `gVfGGTTShadow`, and no bootstrap transition
  ever set `gVfBinderReady`; the nominal relay could therefore never publish
  a PTE in any admitted state.
- Removed that unreachable VF2PF relay opcode, shadow store, generic GGTT MMIO
  shims and all post-map synchronization forks. Global-GTT init now has one
  admitted VF endpoint: Apple's object initialization receives NootedGreen's
  complete BAR0 mapping with both native clearing loops deliberately empty,
  and map/dummy/rotated/unmap routes retain their assignment, receiver, DMA,
  ordering and completed GuC TLB-invalidation checks.
- This deletion is not a media-13 implementation. MTL/ARL remain rejected
  before RESET until per-GT GMD/IP discovery and their documented GGTT update
  transport can be implemented and tested as a separate protocol. The full
  syntax, analyzer, strict-ABI and sanitizer suite passes in
  `/tmp/ngreen-static.UDoTVY`; no VM or hardware access occurred.

### Removal of physical scanout causation experiments

- The TGL framebuffer route table still installed eight Sonoma-era panel
  experiments described as diagnostics. They were not read-only: together
  they cleared undocumented controller bytes, overwrote CRTC seam/scaler
  fields, forced plane tiling and stride, changed full-reprogram decisions and
  permanently cleared a framebuffer color-pipeline flag. The selected
  `configurePlane` branch also contradicted its route comment: it compiled an
  X-tiled value while claiming to force linear scanout.
- Removed V400--V408 and their route/original slots as one coupled experiment.
  Apple's native scaler, DSC, CRTC, watermark, color-pipeline and plane builders
  are authoritative again. Also removed the V201 `hwSetupMemory` probe, which
  sampled BAR2 scanout pixels and direct GGTT PTEs only for logging.
- These routes were already unreachable for an SR-IOV VF because physical
  framebuffer probe/start is rejected before the TGL framebuffer patch set.
  Their removal instead narrows and stabilizes the shared physical-GPU code
  surface. `/tmp/ngreen-static.2hbOxj` passes the full offline suite; no VM or
  hardware action occurred.

### Removal of framebuffer policy and register-diagnostic interception

- Removed three physical-framebuffer routes that replaced policy rather than
  implementing a documented hardware requirement. They forced aperture memory,
  rewrote WindowServer's `wsrv` state from 1 to 3, and repeatedly disabled PSR
  while sampling raw display registers. The associated `getOnlineInfo` hook was
  a logging-only native trampoline. Native framebuffer/WindowServer policy is
  authoritative again; a VF never reaches this framebuffer patch set.
- Removed the V93 opt-in parser and the stale multi-buffer option left after
  their scanout-redirection implementations were retired. The separate generic
  zero-SURF safety guard remains under review because it is a fail-safe against
  an enabled plane fetching GGTT page zero, not a display-mode policy.
- `FastWriteRegister32` and its original-function slot were logging-only after
  prior experiments had been disabled. DMC SRAM writes now use the common
  aligned, allowlisted and BAR-length-checked accessor. `raWriteRegister32`
  retains only its required null-accessor fallback and native call-through.
- The force-wake register helpers were never routed to Apple functions, so their
  original pointers could never be populated. They now use the common bounded
  BAR0 accessor and no longer decode unverified private controller offsets or
  emit broad RCS/power-well passthrough logs. `/tmp/ngreen-static.cyj2sZ`
  passes the complete static suite; no VM or hardware access occurred.

### Restore native DisplayPort negotiation and modeset comparison

- Removed the physical TGL framebuffer's AUX-read wrapper. It logged arbitrary
  sink data, hid `LINK_STATUS_UPDATED` from Apple's recovery path, and changed
  the maximum link rate for a malformed/obsolete revision test after a
  successful native read. DPCD bytes now reach the native driver unchanged.
- Removed both lane-count routes. They treated the already-programmed
  `DDI_BUF_CTL` width left by GOP as a capability and overwrote Apple's
  DPCD/timing result, including a private `AppleIntelPort+0x148` field. A stale
  link state can no longer force extra, unnegotiated lanes into a new modeset.
- Removed the TGL `hwRegsNeedUpdate` wrapper and the ICL byte patch which forced
  the opposite decision. The wrapper rewrote pending transcoder parameters to
  their live values and explicitly depended on the removed four-lane override;
  both physical paths now preserve Apple's native compare/reprogram decision.
- The now-unused hand-written DPCD view and stale CRTC usage annotations were
  removed. `/tmp/ngreen-static.dZ50PB` passes syntax, analyzer, strict ABI and
  every offline protocol/sanitizer model. A VF does not load this framebuffer
  patch set, and no hardware or VM action occurred.

### Retire manual display firmware, power-well and CDCLK ownership

- The opt-in DMC wrapper was not a bounded firmware loader. In addition to
  copying a TGL or ADL-P payload to SRAM, it wrote fixed power-well, DDI/PHY,
  transcoder timing, link M/N, panel, PSR and display-context values captured
  from one machine, then invoked a different native ICL initialization and
  replayed selected values afterward. Its DC-exit helper could repeat another
  fixed subset whenever a hooked display comparison observed a low-power state.
- Removed that wrapper, the extra AUX invocation, the replay state and the
  `ngreen-dmc` policy. The embedded TGL/ADL-P payload sources, declarations,
  Xcode entries and byte-comparison script had no remaining consumer and were
  removed. This does not claim firmware compatibility; it makes the admitted
  native physical driver the sole DMC owner and preserves the existing rule
  that an SR-IOV VF owns no display firmware.
- Removed the physical power-well replacement that wrote request bits and
  returned without observing hardware acknowledgement, plus DDI/AUX/display
  wrappers that only stamped a private pointer. Removed the PowerWell initializer
  that forced `fAlwaysOn` and substituted `fMMIO` after native initialization.
- Removed unconditional CDCLK disable/reprogram and the route used to bypass a
  PCU verification result. Clock selection and error handling are native again.
  `/tmp/ngreen-static.ZxzxZm` passes the complete static suite. The Linux host,
  VF and VM were not touched; release-link/Xcode metadata remains gated by CI.

### Pin and minimize the TGL framebuffer ABI

- Recovered both local Tahoe 16.0.32 framebuffer payload identities instead of
  inferring a variant from whether one C++ symbol happened to exist. The
  production payload is UUID `B6078743-F795-3202-A55E-EA690FA40949`, SHA-256
  `a562cff08e08bda3eab6f4f550368f46d50155f2c6341af460377be6b2444f56`;
  the debug payload is UUID `B96A8448-9D17-3F78-B76D-15A0B359A9A0`, SHA-256
  `285ee7a9c3d6c9a9647013f44fb40f312b1cb42879974ef0542544cf049442b5`.
  Unknown payloads now fail admission before any private-layout operation.
- Every active framebuffer byte sequence was located in both admitted binaries
  and mapped back to its containing symbol. The old set altered power-well,
  topology, mode-setting, HPD/NVRAM, link-training, TCON and infoframe control
  flow rather than adapting one documented generation boundary. All were
  removed except the exact-one `ReadRegister64` correction: its original
  bounds arithmetic admitted four bytes before an eight-byte load, and the
  replacement requires all eight bytes. Each UUID has its own complete
  sequence so a near match or count drift fails atomically.
- Removed the global framebuffer-controller/accessor cache and all Plane/Scaler
  init/update/write interception. A null native accessor is no longer replaced
  from a different object's private offset, and failed or incomplete native
  initialization is not converted into apparent success. Also removed the
  platform-table, controller-start, OS-information and workaround hooks, their
  stale DMC/platform structures and the now-unreferenced generated
  `AppleIntelParams.hpp` runtime header.
- An SR-IOV VF still rejects the physical framebuffer before this branch. The
  remaining pinned correction is therefore for the shared physical-GPU path,
  not a virtual-display implementation or a VF acceleration result. The full
  syntax/analyzer/strict-ABI/sanitizer and protocol suite passes in
  `/tmp/ngreen-static.tsNTId`; the VM remained off.

### Remove the unversioned ICL fallback

- No Tahoe `AppleIntelICLLPGraphicsFramebuffer` or `AppleIntelICLGraphics`
  payload exists in the workspace from which to establish an exact UUID,
  instruction count or object ABI. Nevertheless the `-ngreenicl` branch
  applied four platform-ID rewrites, three Sonoma kernel-cache SKU/control-flow
  patches, fixed 1x8x8 topology at private offsets and a GuC firmware route.
- Removed both ICL kext registrations, their process branches, boot argument,
  topology and firmware wrappers/original slots. Also collapsed accelerator
  personality publication to its only admitted TGL identity. Actual Ice Lake
  hardware already has an Apple-native driver and is now left untouched.
- This intentionally narrows claims rather than treating an unverified fallback
  as Gen11 support. The RPL VF continues to use the UUID-pinned TGL accelerator;
  future payload families require their own identity, disassembly and protocol
  admission. `/tmp/ngreen-static.hYAh0w` passes the complete offline suite and
  no VM or hardware access occurred.

### Isolate the accelerator VF protocol from the physical path

- The pinned TGL accelerator branch now classifies PCI ownership once after
  BAR0 admission. GGTT replacement, GuC/CTB translation, direct-LRCA context
  handling, memory interrupts, legacy-doorbell rejection, reset/ring
  containment and their byte patches are installed only for a verified VF.
  A PF keeps Apple's native implementations for all of those entry points.
- Removed the hand-ported physical force-wake implementation and its duplicate
  domain/range tables. It was installed from a policy flag rather than a
  proved generation ABI and could poll/write the wrong power domain. The three
  force-wake entry points are now VF-only no-ops, matching i915's rule that a
  VF creates no guest-owned force-wake domains.
- Once a route is VF-only, its physical fallback is unreachable. Removed those
  branches and every original-function slot that existed only to support them.
  Originals are still retained where the validated VF bridge deliberately
  executes native allocation, mapping, detach or submission side effects.
  Removed the duplicate physical `getGPUInfo` wrapper and the now-meaningless
  `ngwegcoex` policy; physical topology is no longer overwritten after native
  discovery.
- TGL-generation selection is now independent of PF/VF ownership. A TGL VF no
  longer receives the later-generation topology or unaligned-store patches
  merely because `isRealTGL` correctly excludes VFs. The remaining RPL
  topology constants are still target-specific and remain an open portability
  item before any general support claim.
- Both 1,825,024-byte accelerator payload variants pass the pinned
  `BA3AA1C0-FE6B-33B3-9D85-73F848394E3D` UUID parser. A sanitizer test now
  proves their only ten differing bytes are the two `com.xxxxx` versus
  `com.apple` bundle-identifier strings; executable bytes are identical.
  The complete syntax, analyzer, strict-ABI, sanitizer, protocol and on-disk
  payload suite passes in `/tmp/ngreen-static.hpt32k`. The VM remained off.

### Replace target topology constants with PF runtime discovery

- Re-disassembled the complete UUID-pinned `IntelAccelerator::getGPUInfo` body.
  A VF cannot directly read its five consumed PF-owned registers: slice enable
  `0x9138`, geometry DSS enable `0x913c`, EU disable `0x9134`, VDBOX/VEBOX
  disable `0x9140`, and timestamp crystal-clock configuration `0x0d00`.
  The prior fixed EU/DSS/L3 patches therefore hid only part of the invalid-VF-
  read problem and left media-engine enumeration and timestamp math exposed.
- Implemented the current i915 early MMIO transport exactly at its protocol
  boundary: GuC action `0x5005`, four-dword messages, ABI handshake opcode
  `0x01` at version 1.0, runtime opcode `0x10`, CRC32-derived four-bit magic,
  GuC-success/origin and echoed-magic validation, DATA0 MBZ enforcement, and
  three offsets per request. The query requests the complete eight-register
  TGL/ADL early allow-list used by the PF, after VF RESET and GuC version
  negotiation but before CTB creation.
- A pure decoder derives one media-12 slice, DSS/SS and EU counts, L3 bank pairs
  and enabled media mask. Zero/all-ones/reserved clock encodings, missing media
  engines, empty topology and values beyond media-12 limits fail admission.
  No device ID or CPU model is used as topology evidence.
- The validated snapshot is injected only into a verified VF and only at six
  exact anchors inside the solved `getGPUInfo` symbol boundary. Apple retains
  its native popcount and timestamp calculations; only raw inaccessible loads
  become immediates. L3's SKU branches are collapsed to one store whose value
  comes from `MIRROR_FUSE3`. PFs keep all native register reads, and the prior
  fixed `8 SS × 8 EU` overrides are removed.
- Both on-disk accelerator variants contain every anchor exactly once and in
  the same function order. Protocol/topology invalid-state tests, the binary
  anchor test, syntax, analyzer, strict ABI and all existing sanitizer models
  pass in `/tmp/ngreen-static.ynqe08`. The VM remained off; the relay exchange
  and resulting Metal/media behavior still require controlled runtime proof.

### Narrow the accelerator SKU admission patch

- The remaining accelerator SKU patch did more than admit the published
  `0x9a49:0x8086` compatibility identity: it removed the original sentinel
  branch and changed a conditional acceptance jump into an unconditional one.
  Any value reaching the pinned body could therefore enter GT2 initialization.
- The replacement now changes only the immediate in Apple's existing
  `0x9a40:0x8086` comparison to `0x9a49:0x8086`. The sentinel, second native
  `0x9a48` comparison, failure call and both conditional branches remain byte
  for byte native. The exact anchor occurs once in each admitted payload and
  is covered by the on-disk patch test.

### Bound physical-TLB isolation to live VF call paths

- Re-inventoried every instruction that accesses the PF-owned
  `GEN12_GUC_TLB_INV_CR` (`0xCEE8`) in both admitted accelerator payloads:
  ten immediate writes, one ECX write, one R8 write, and their twelve posting
  reads. A sanitizer-backed on-disk test pins every file offset and proves the
  production and debug executable bytes have the same inventory.
- The old patch changed all matches over the complete Mach-O image. The VF now
  solves seven still-executed native bodies and their adjacent symbol bounds,
  then applies one exact write/read anchor inside each body. Five routines whose
  public entries are completely replaced are deliberately left byte-for-byte
  intact. This makes the entry route, rather than an unbounded byte sweep, the
  isolation boundary.
- `IGGuC::invalidateTLB() const` was one of those fully routed routines but did
  not have a VF replacement. Its four static call sites were disassembled:
  GuC load/control initialization occurs before CTB readiness and follows
  i915's pre-ready no-op rule; runtime GGTT bind/free uses the captured
  `IGHardwareGuC` and the existing synchronous GuC v70 invalidation, guarded
  against IRQ, non-preemptible and completion-workloop contexts.
- Removed the obsolete V223 byte/dword conversion and V224 legacy-HXG patches.
  Their only original CTB send/receive bodies are unreachable on a VF because
  both public entries are replaced by the bounded modern producer/consumer.
  Keeping mutations in dead native implementations added version coupling
  without changing the active protocol.
- G2H reply-space credits are now returned immediately after a structurally
  valid `MODE_DONE`, `DEREGISTER_DONE`, or `TLB_INVALIDATION_DONE` frame leaves
  the ring, before lifecycle interpretation. This matches
  `intel_guc_ct.c:ct_handle_event`; a stale completion can no longer leak
  reserved space and deadlock CT drain, while state mismatch and accounting
  underflow still fail the protocol closed.
- The v70 registration request, KMD flag, single-LRC zero work-queue fields,
  engine mask, LRCA descriptor, context image offset, VF memory-IRQ register
  state, completion lengths, and credit sizes were rechecked against current
  i915 source. The complete local static/analyzer/strict-ABI/sanitizer and
  pinned-payload suite passes in `/tmp/ngreen-static.D8Vh7O`. The VM remained
  shut off; this is static evidence only.

### Bound every remaining instruction patch to its owning function

- Rechecked every active `LookupPatchPlus` instruction mutation in the Gen11
  implementation. The doorbell `DISTRDB` replacement is now confined to
  `IGHardwareGuC::readDoorbellSQIDIConfig`; the SKU and six relayed-fuse
  anchors share the solved `IntelAccelerator::getGPUInfo` body; and all fourteen
  aligned-store opcode changes are confined to `blit3d_submit_rectlist`.
- Extracted the rect-list anchors into a shared header and added an on-disk
  inventory test. Both admitted accelerator payloads have the same fourteen
  sites at the pinned offsets. This includes the short opcode match inside one
  `movapd`, which intentionally converts it to the corresponding unaligned
  form along with the thirteen `movaps` stores.
- The sole physical-framebuffer instruction fix is now bounded by the two
  `AppleIntelRegisterAccessManager::ReadRegister64` overload symbols. A new
  test checks the production and debug UUID independently, proves that only
  its matching RIP-relative anchor exists, and pins that anchor inside the
  disassembled function range.
- `IGGuC::invalidateTLB` now also recognizes the proven final shutdown state.
  Before CTB enable it follows i915's initialization no-op; during active DMA
  it still requires a synchronous GuC completion; after all contexts are
  deregistered and the final heavy invalidation publishes `gVfDmaQuiesced`,
  later native object destruction does not attempt to use an already sealed
  CTB or manufacture a protocol fault.
- No instruction byte patch in `kern_gen11.cpp` now searches or mutates the
  complete image. The full syntax/analyzer/strict-ABI/sanitizer and all four
  pinned-payload anchor suites pass in `/tmp/ngreen-static.oHyihM`. The VM
  remained shut off.

### Admit only the six implemented GuC v70 FAST requests

- The VF CTB producer previously accepted any host-origin request up to 31
  dwords and inferred reply-space reservations from only the action number.
  A malformed internal caller could therefore publish an unsupported action or
  reserve credits for a request whose body did not match the expected reply.
- A pure pre-publication validator now admits only the six requests constructed
  by this bridge: single-LRC register, context-policy update, schedule,
  schedule-mode, deregister and full GuC TLB invalidation. It checks exact
  lengths, context-ID bounds, KMD/single-engine registration fields, zero
  work-queue fields, engine class/mask, 32-bit LRCA form, exact KLV order and
  bounded values, mode, and heavy/full/flush invalidation flags.
- Retirement admission and G2H credit reservations are returned by that same
  validation result. The sender no longer carries a second action-only model
  that can drift from the accepted body. Unsupported or malformed bodies fault
  the VF before acquiring the queue lock or writing the H2G ring.
- A sanitizer test accepts both i915 timeout profiles and every valid emitted
  request, rejects every alternate length through 13 dwords, and mutates each
  constrained field plus IDs, modes, invalidation flags and unknown actions.
  The complete local suite passes in `/tmp/ngreen-static.weYYxH`; the VM stayed
  off. This validates host-side request construction, not GuC execution.

### Replace physical GGTT PTE encoding with the media-12 VF contract

- Re-disassembled Tahoe `IGHardwarePageTable::attributeBits`, `commitRange`,
  `updateRange`, and all four concrete global-page-table mutations. Apple can
  pass only flag bits 1, 3, 4 and 7 (`0x9a`), and its mapper retains those bits
  while writing the PTE high and low dwords separately.
- Current i915 identifies Gen12 GGTT bits 4:2 as the PF-owned VFID and bit 1 as
  local memory. Its direct media-12 VF uses `gen8_ggtt_pte_encode`, which writes
  system-memory address plus present and does not insert a VFID. The target
  integrated GPU has no local-memory address space. Passing Apple's physical
  cache attributes through was therefore not a valid VF encoding.
- Normal, rotated, dummy and unmap paths now validate the exact Apple attribute
  mask and write only the admitted 39-bit system DMA address plus present.
  They share exact BAR0/PTE-base and pinned-dummy validation. The three native
  originals formerly retained only for PTE stores are no longer callable.
- Every PTE is written through an aligned volatile `uint64_t`. An optimized
  x86_64 Mach-O object was disassembled locally and each loop contains one
  eight-byte `movq`, eliminating the native transient address produced by its
  high-then-low stores. Write barriers drain the aperture before return.
- Before CTB has ever run, setup mappings need no invalidation because no GPU
  request can have cached them. Afterwards, each successful map/dummy/rotated
  mutation requires the same synchronous heavy GuC invalidation already used
  by unmap; unsafe wait contexts or unavailable transport fail before writes.
  Rotated rollback also invalidates its restored dummy mappings before faulting.
- The pure model exhausts the low nine flag bits, rejects every high bit, and
  proves emitted PTEs contain no LM/VFID. The complete syntax, analyzer,
  strict-ABI, sanitizer and payload suite passes in
  `/tmp/ngreen-static.mOqJrE`. This remains static evidence; VM stayed off.

### Validate the complete Tahoe direct-LRCA descriptor

- Re-disassembled every direct reference to the packed descriptor at
  `IGHardwareContext+0x89/+0x8d`. `initWithOptions` zeroes it, seeds low dword
  `0x309`, replaces address-mode bits 4:3, inserts the page-aligned GGTT LRCA,
  constructs SW-ID/engine-instance/engine-class in the Gen11 upper layout and
  conditionally toggles only coherent bit 5 and privilege bit 8. No persistent
  restore bit or SW-counter/reserved bit is created.
- `IGHardwareCommandStreamer5::submitExecList` copies that descriptor to its
  stack and conditionally adds bit 2 `FORCE_RESTORE` to the copy. It never
  writes the object. Current i915 independently defines the same flag and its
  Gen12 `lrc_update_regs` places it in `ce->lrc.lrca`; GuC v70 registration
  then sends that LRCA because later schedule actions carry only the GuC ID.
- VF attach now rejects malformed descriptors before Apple's native attach can
  index its private proxy pool. It requires a nonzero page-aligned LRCA field,
  valid plus normal priority, only the proven address-mode/coherent/privilege
  flags, zero upper reserved/MBZ/SW-counter fields, raw engine class 0..5 and
  an instance representable by the 32-bit GuC engine mask. The native Tahoe
  `{0,1,2,3,5,4}` Intel-to-GuC class table matches current i915 exactly.
- Registration preserves Apple's persistent fields and explicitly adds
  `FORCE_RESTORE`; the FAST-request validator requires that normalized form
  and still requires a zero high LRCA dword. This closes the former gap where
  direct GuC scheduling never received Apple's first-submit stack flag.
- Sanitizer tests exhaust all 4,096 low flag combinations, 1,536 class,
  instance and SW-ID tuples, each upper reserved bit and GuC normalization.
  The complete syntax, analyzer, strict-ABI, sanitizer and pinned-payload suite
  passes in `/tmp/ngreen-static.YyW8hV`. The VM remained shut off; firmware
  execution and completion are still controlled-runtime obligations.

### Correct and centralize media-12 VF memory interrupts

- Disassembled Tahoe's complete `IGInterruptBridge::readAndClearInterrupts`,
  its engine readers and callback dispatcher. The VCS reader tests media master
  bits 0 and 2, selects interrupt identities 1 and 4, and its constant callback
  table maps their user interrupts to `IGBitSet<46>` bits 3 and 4. These are
  VCS0 and VCS2; no VCS1 callback is admitted by the pinned payload.
- Current i915 independently assigns TGL/ADL media-12 the engine mask
  `RCS0|BCS0|VECS0|VCS0|VCS2`. Its engine table maps VCS0 to memory-IRQ source
  byte 32 and VCS2 to byte 34. The bridge incorrectly consumed byte 33, so a
  VCS2 completion could remain uncleared and its Tahoe callback could be lost.
- A shared pure table now defines all six engine routes: RCS0 `0->0`, CCS0
  `4->1`, BCS0 `15->2`, VCS0 `32->3`, VCS2 `34->4`, and VECS0 `63->5`.
  Runtime consumption uses that table rather than independent literals. GuC
  source 25 and the 16-byte status stride remain identical to i915.
- The Gen12 context-image LRM/LRI sequence is now emitted by the same reviewed
  helper. It validates the register-image span and aligned nonzero GGTT page,
  then writes only dwords `0x50..0x53` and `0x55..0x59` with the i915/Tahoe
  ring mask, status and source addresses. The stale unused off-by-one
  `ARRAY_SIZE` macro was removed.
- Sanitizer coverage checks all 64 source offsets, all 64 combinations of the
  six admitted engines, explicitly rejects 33 and admits 34, verifies every
  context dword/canary and all invalid span/page cases. The complete local
  syntax/analyzer/strict-ABI/sanitizer/pinned-payload suite passes in
  `/tmp/ngreen-static.LuWFhe`. The VM remained shut off; actual interrupt and
  media completion behavior is still a controlled-runtime obligation.

### Admit exact media-12 context engines and publish LRCA tails atomically

- Re-disassembled Tahoe `IGHardwareContext::initWithOptions` including its
  complete six-way `IGHwCsType` jump table and descriptor construction. The
  only target pairs are RCS `(class 0, instance 0)`, CCS `(5,0)`, BCS `(3,0)`,
  VCS0 `(1,0)`, VCS2 `(1,2)`, and VECS `(2,0)`. Current i915 identifies the
  same hardware classes/instances and GuC classes `{0,4,3,1,1,2}`.
- Descriptor admission formerly allowed OTHER class and every instance below
  32 merely because they fit GuC's one-hot engine mask. It now accepts exactly
  the six real media-12 engines. REGISTER_CONTEXT validation uses the same
  table, so an unsupported class/mask pair cannot reach H2G through another
  internal caller.
- Submission now revalidates the packed high and low descriptor, requires its
  decoded engine to equal the native `IGHwCsType` argument, and compares class
  plus instance with the retained attach record before touching the context
  image. This closes the former low-dword-only identity check.
- The LRCA ring tail formerly changed before final state admission and could
  remain advanced when H2G backpressure or publication failed. Tail publication
  now occurs inside the serialized sender only after ring space and G2H credits
  are reserved, immediately before the CT frame and descriptor tail are made
  visible. Every false return leaves the context tail untouched.
- Offline tests exhaust all 1,536 raw class/instance/SW-ID tuples, every
  one-hot GuC class/mask candidate, unsupported masks, all persistent/reserved
  bits, and cross-check context types with memory-IRQ callback routes. The full
  syntax/analyzer/strict-ABI/sanitizer/pinned-payload suite, including the
  sender publication refactor, passes in `/tmp/ngreen-static.WbgPac`.

### Fail closed on unsupported or fatal GuC G2H events

- Re-disassembled Tahoe's `IGHardwareGuCCTBuffer::gucToHostAction` and
  `handleSoftwareGuCToHostInterrupt`, then compared the modern event path with
  current i915 `ct_process_request`/`ct_handle_event` and xe G2H dispatch. The
  Tahoe handler only copies a frame and returns a legacy action; the VF wrapper
  intentionally ignores that return, so an unclassified modern event was
  previously removed from the ring without any implementation handling it.
- A pure event classifier now admits only the completion actions generated by
  requests this bridge implements: exact three-dword MODE_DONE, two-dword
  DEREGISTER_DONE and two-dword TLB_DONE HXG messages. Their CT receive-space
  credits are derived from the same classification and returned before payload
  interpretation, matching i915's deadlock-avoidance ordering.
- Exact two-dword CONTEXT_RESET and four-dword ENGINE_FAILURE notifications are
  recognized only to log bounded payloads and quarantine the transport. This
  bridge has no context-replay or GT-reset implementation, so forwarding either
  to Tahoe's legacy parser would be a false recovery. Unsupported actions,
  malformed lengths, unexpected FAST responses and credit underflow likewise
  fault immediately.
- After the routed parser consumes a bad/fatal frame and sets the fault flag,
  the software interrupt worker now stops the drain immediately. It cannot feed
  later frames through a host/firmware state already known to be divergent.
- Sanitizer coverage exhausts all 65,536 action values, HXG lengths 0..32,
  event DATA0 variants, both origins and every type. The full local suite passes
  in `/tmp/ngreen-static.43sH6y`; the VM remained shut off. This is parser and
  state-containment evidence, not firmware-delivery or acceleration evidence.

### Seal both CTB directions only after G2H is empty

- Re-audited the CTB partial-init and shutdown paths against the two native
  queue locks. A channel initializer can publish the retained shared backing
  before a later native init failure unwinds the object's locks. The hardware
  filter and software event entry previously checked only broad GGTT/backing
  state, so a later interrupt could still enter quarantined native storage.
- G2H consumer admission now requires the complete classified VF transport:
  GGTT, memory IRQ, CPU/GPU CTB mapping and firmware enable must all be live,
  while sealed or protocol-fault states are rejected. `submissionStopped` is
  deliberately not a rejection because expected disable/deregister/TLB
  completions must remain drainable during orderly teardown. All 512 state
  combinations are checked by the shared offline admission model.
- `vfStopSubmissionAndSealCtb` formerly serialized only H2G and considered the
  transport settled when H2G, reply credits and the TLB waiter were empty. A
  G2H frame already written by GuC but not yet handled could therefore be made
  invisible by the subsequent stopped bit.
- Sealing now validates the exact H2G and G2H mappings, requires distinct native
  locks, and acquires them in the single H2G-to-G2H order. Both descriptors must
  be valid and both rings empty, with no reserved reply credits or active TLB
  waiter, before the stopped bit is published. The consumer releases G2H before
  lifecycle handling and no reverse two-lock edge exists. A lost callback now
  reaches the bounded shutdown timeout instead of silently discarding a frame.
- The full syntax/analyzer/strict-ABI/sanitizer/pinned-payload suite passes in
  `/tmp/ngreen-static.PrRSBU`; the VM remained shut off. Hardware interrupt
  ordering and GuC disable acknowledgement remain controlled-runtime evidence.

### Preserve complete direct-context identity through detach

- Disassembled Tahoe `IGHardwareGuC::AttachContextDescToGucContext`
  (`0x21e12`) and `DetachContextDescFromGucContext` (`0x22102`). Native detach
  first keys its hash with only descriptor-low LRCA page; if that page is not
  present it returns without clearing a proxy slot. If present, it decodes the
  descriptor stored by the earlier native attach, clears that slot, optionally
  releases its legacy ID, and removes the hash node.
- The direct bridge previously stored only descriptor low/class/instance and
  final detach searched only by LRCA page. If its direct record was missing it
  still invoked the native function, which can be a no-op and provides no proof
  that PF GuC stopped using the caller's soon-to-be-released backing. A mutated
  page can also name another native hash record.
- Direct records now retain both packed dwords, including the Gen11 SW-ID, plus
  class, instance and the exact mapped-buffer object. Duplicate attach, submit,
  ordinary detach and post-shutdown detach use one shared complete-identity
  predicate. Retired records clear both dwords only when their retained backing
  is actually released.
- A valid detach with no direct record takes an extra reboot-lifetime retain on
  its verified full-size GGTT backing, faults submission, and leaves both direct
  and native bookkeeping quarantined. An identity mismatch likewise leaves the
  known retained record and native LRCA hash untouched; it does not risk using
  the inconsistent key to remove another context. This deliberately leaks on a
  corrupt path to prevent DMA-after-free or cross-context teardown.
- Tests mutate descriptor low, high/SW-ID, GuC class, engine instance, backing
  identity and null backing in addition to the existing exhaustive descriptor
  flag/engine suite. The full local gate passes in
  `/tmp/ngreen-static.eQvzbn`; the VM remained shut off.

### Validate MODE_DONE runnable state before lifecycle advance

- Current xe `handle_sched_done` interprets the third MODE_DONE dword as the
  resulting `runnable_state`: `1` completes enable and `0` completes disable.
  The direct bridge already required the exact three-dword event shape but
  discarded this payload and retired whichever local pending token was oldest.
  A corrupt or out-of-order `1` could therefore complete a local disable and
  permit DEREGISTER while firmware still reported the context runnable.
- MODE_DONE lifecycle handling is now one pure transition. A pending enable
  accepts only runnable `1`; otherwise a pending disable accepts only `0`.
  Enable retains ordering priority when teardown has queued both tokens, so a
  disable acknowledgement cannot overtake the earlier enable acknowledgement.
  Enable completion is admissible only from PendingEnable, Enabled or the
  ordered dual-pending PendingDisable state; disable completion additionally
  requires PendingDisable. Undefined runnable values, reversed order,
  impossible flag/state pairs and events with no pending token leave state
  unchanged and enter the existing protocol-fault quarantine.
- The sanitizer model exhausts all nine lifecycle states, four pending-token
  combinations and runnable values `0`, `1` and an invalid `2` (108 cases).
  The complete syntax/analyzer/strict-ABI/sanitizer/pinned-payload suite passes
  in `/tmp/ngreen-static.EXgXQm`; the VM remained shut off. This proves host
  transition logic, not firmware ordering or execution.

### Remove the redundant VF descriptor proxy and LRCA hash

- Re-disassembled Tahoe's descriptor attach at `0x21e12`, hash add at
  `0x2200a`, original submit at `0x21baa`, detach at `0x22102`, and KMD-idle
  query at `0x223cc`, then enumerated every call to the hash lookup routines.
  Descriptor attach allocates a legacy proxy ID and engine slot, calls a
  void-returning hash add, ignores both its earlier `contains` result and the
  add allocation result, then always returns true. A 24-byte node allocation
  failure therefore leaves mutated proxy storage with no detachable LRCA key.
- The only LRCA-hash consumers are native attach, original legacy submit,
  native detach and the native KMD-idle query. Their VF entries are all fully
  routed: direct submit uses its retained context table, direct idle uses the
  same lifecycle snapshot, and direct detach owns firmware retirement. No
  routed VF path consumes the proxy descriptor or hash value.
- VF descriptor attach/detach now bypass those native bodies entirely. The
  direct table is allocated and registered transactionally, duplicate attach
  changes only its checked reference count, and final detach releases the pin
  only after exact GuC deregistration. Original-function slots, proxy-pool
  validation at descriptor attach/detach, and their two bounded `0xCEE8`
  instruction patches were removed. The separate scheduler process context
  remains bounded by its existing transactional allocator and is not confused
  with a direct LRCA descriptor.
- The on-disk TLB inventory still pins every physical-register instruction in
  both admitted payloads; its reachability model now records five callable,
  bounded-patched bodies and seven entry-isolated bodies. The complete local
  suite passes in `/tmp/ngreen-static.eQEAkK`; the VM remained shut off. This
  establishes static reachability and host ownership, not runtime firmware
  registration success.

### Validate every GuC MMIO success payload

- Compared the direct bootstrap callers with current i915
  `intel_iov_query.c`, xe `xe_gt_sriov_vf.c`, and their shared GuC ABI
  headers. GuC origin and RESPONSE_SUCCESS type are transport properties, but
  each action also defines a DATA0 contract that must be checked before later
  scratch dwords are trusted.
- VF_RESET requires DATA0 to be zero. The bridge previously accepted any
  success payload and continued into MATCH_VERSION, so a protocol-invalid
  positive result could be mistaken for a completed reset. MATCH_VERSION and
  both CONTROL_CTB paths already required zero and now use the same named
  predicate; SELF_CFG likewise uses the exact parsed-KLV count of one.
- QUERY_SINGLE_KLV encodes returned length in DATA0[15:0] and requires
  DATA0[27:16] to be zero. The old 32/64-bit helpers compared only the low
  length and would consume response words despite nonzero reserved bits. Both
  now require the exact length and all reserved bits clear before reading a
  value.
- A freestanding sanitizer model checks zero/self-config single-bit mutations,
  every 16-bit query length for expected lengths 0..4, and every reserved bit:
  327,769 payload contracts. The complete local gate passes in
  `/tmp/ngreen-static.M3UvCc`; the VM remained shut off. Transport timing and
  actual firmware responses remain runtime evidence.

### Validate the complete legacy CTB translation request

- Re-disassembled Tahoe `registerCommandTransportBuffers` at `0x203e6` and
  `deregisterCommandTransportBuffers` at `0x208b0`. Registration supplies the
  exact backing GPU address (G2H at base+0x400, H2G at base), descriptor size
  0x40 and channel. Deregistration does not carry that address: it carries the
  CTBuffer object's registration token at `+0x38` for both channels.
- A shared pure contract now verifies exact action, length, address/token,
  descriptor size and channel before translating 0x4505/0x4506 to the modern
  VF self-config/control ABI. It also requires the GuC object's `+0xa10` CTB to
  be the retained CTB object; arbitrary internal storage cannot trigger the
  shutdown sequence.
- All rejected calls publish a failure response. The old short-registration
  diagnostic read request dwords 1..3 even when the validated length was below
  four; it now logs only the bounded length and cannot overread the request.
- A structurally valid registration whose modern self-configuration fails now
  also publishes a nonzero failure response.  It formerly returned `false`
  while writing response zero, an internally contradictory ABI that could let
  a future non-null caller continue with an unconfigured channel.
- The 57-case sanitizer contract and complete static suite pass in
  `/tmp/ngreen-static.XBmX89`; GitHub CI run `36279158723` also passes. The VM
  remained shut off.

### Preserve context engine identity until the final native detach

- `DEREGISTER_DONE` previously retained LRCA, descriptor and backing for a
  device-shutdown late detach, but cleared the record's GuC engine class and
  instance immediately. `matchesRecord` correctly includes both fields, so
  every non-RCS0 CCS/BCS/VCS/VECS context would later fail identity validation
  and panic despite successful firmware retirement.
- The completion transition now changes only pending flags and lifecycle state.
  Full descriptor, engine and backing identity remains on the tombstone while
  native owners exist. The single final backing-release boundary clears all
  identity fields together.
- The pure lifecycle model tests DEREGISTER_DONE from all nine states, proves
  every identity field and reference count are preserved, and proves final
  release clears only the intended identity. The complete local suite passes in
  `/tmp/ngreen-static.8xCSQH`; the VM remained shut off pending CI and further
  lifecycle/source review.
### Reject stale or duplicate TLB completions immediately

- The G2H parser returned reserved receive credits before interpreting a
  structurally valid `TLB_INVALIDATION_DONE`, as required for transport drain,
  but a sequence mismatch only produced a capped log.  Such an event could
  consume the credits reserved for a different request and leave the current
  waiter alive until its one-second timeout.
- Completion admission now requires an active waiter, exact event/wait
  sequence identity and the exact previous done sequence.  The final atomic
  predecessor-to-current transition remains the publication point; any stale,
  duplicate or racing completion immediately marks the VF protocol fault.
- The pure model covers inactive, wrong-waiter, wrong-event, duplicate,
  skipped-predecessor and 32-bit wraparound cases.  This is a fail-closed
  transport correction; real GuC completion delivery still requires the
  controlled runtime phase.

### Remove the unintegrated physical link-training writer

- `IntelDPLinkTraining.cpp/.hpp` had no product caller or route; the only
  consumer was an offline translation-table test.  The file nevertheless
  compiled raw combo-PHY MMIO writes into the kext while explicitly leaving
  the C--F DKL PHY implementation absent.
- A physical device already retains Apple's native link-training lifecycle in
  the UUID-pinned framebuffer.  A VF is rejected by that framebuffer before
  any physical display access.  There is therefore no valid ownership point at
  which this disconnected writer could run.
- The implementation, header, table-only test and every Xcode reference were
  removed together.  This reduces unreviewed physical MMIO surface without
  changing an active PF or VF path; the sources remain recoverable from Git.
- The same reachability pass found no caller for the old BAR2 mapper, any of
  the WA/whitelist register helpers, four pre-Gen11 derivative flags or the
  obsolete LRCA-repair note in `kern_green.hpp`.  They were removed rather
  than retained as unaudited alternate register paths.  `BIT`/`REG_BIT` remain
  because the existing Gen11 constant declarations still consume them.
- Removing the link-training writer also made the generic register RMW helper,
  physical plane-SURF intervention, unused ACPI/Framebuffer declarations and
  compatibility getter unreachable.  The only remaining `readReg32`/
  `writeReg32` consumers are the admitted VF GuC mailbox and notification
  registers, all constrained by the fixed VF MMIO allowlist.

### Restore the native display-pipe contract and remove the dead ABI catalogue

- Both bundled TGL accelerator personalities declare `DisplayPipeSupported`
  and `TransactionsSupported` as boolean true. The manual injected personality
  instead hardcoded numeric zero, while the documented `ngreendp0`/`ngreendp1`
  selector had no caller. That mismatch could force WindowServer onto a
  degraded software-composition path.
- The injected dictionary now preserves the native boolean-true contract. The
  dead selector and its README switches were removed. This closes a static
  personality mismatch; it is not yet runtime evidence of accelerated display.
- `kern_gen11.hpp` had grown to 1,083 lines even though the product consumed
  only the address-range type, six engine values and the active route class.
  Old workaround/ring/reset/display macros, topology tables, connector and
  framebuffer structs, bit walkers and six undefined direct-MMIO declarations
  were removed. The fully replaced VF submit route also no longer captures an
  unused original implementation.

### Publish the complete native accelerator personality

- The former manual personality did not merely override PCI matching. It
  reconstructed only a subset of the bundled entry and omitted `Development`,
  `Debug`, `IOGVAHEVCDecodeCapabilities` and
  `IOGVAHEVCEncodeCapabilities`. That made scheduler/media behavior depend on
  which personality happened to bind and undermined VideoToolbox validation.
- Publication now finds exactly one IOCatalogue entry for the actually loaded
  custom or Apple bundle identifier, shallow-clones its complete dictionary and
  changes only `IOPCIPrimaryMatch` to the admitted 0x9a49 compatibility
  identity. Missing catalogue state, duplicate candidates, missing required
  nested dictionaries, allocation failure or `addDrivers` failure all prevent
  personality publication.
- Current XNU `IOCatalogue::findDrivers(OSDictionary *)` proves the returned
  ordered set retains matching catalogue dictionaries while holding the
  catalogue read lock; `OSDictionary::withDictionary` retains all nested
  values, so releasing the result does not invalidate the clone. An offline
  plist contract test checks both bundled variants for the complete display,
  development, debug and HEVC media properties.
- The CI path filter now covers all of `tools/**`. Previously several protocol
  tests executed by `check-static.sh` could be changed without triggering the
  workflow because only an obsolete hand-maintained subset was listed.

### Preserve native standalone startup without the physical-FB delay

- Disassembly of `IntelAccelerator::registerWithFramebufferController` shows a
  fixed 30-second `IOService::waitForMatchingService` call before its native
  standalone path. An accelerator-only VF intentionally rejects and omits the
  physical framebuffer, so every start paid that full delay before setting the
  standalone callback state.
- Current XNU proves a zero timeout still performs the existing-service lookup,
  installs/removes the one-shot notification safely and returns immediately if
  no framebuffer is present. A VF-only, UUID-admitted patch therefore changes
  only the 30-second immediate to zero. Apple's exact no-framebuffer branch,
  `initLocalCallbackSupport`, failure propagation and PF behavior remain intact.
- The instruction is bounded to
  `registerWithFramebufferController`..`initHardwareWorkarounds`; an offline
  inventory proves its one exact offset in both admitted payloads and proves
  the replacement does not already occur on disk.
- `NootedGreenDriverProfileDefault` and `NootedGreenDriverProfiles` were also
  removed from the plugin plist. No source, build tool or runtime path consumed
  them, and their advertised ICL fallback contradicted the UUID-pinned TGL-only
  implementation.

### Remove unadmitted global media/model mutations

- The remaining `DYLDPatches` path was not a GPU transport shim. By default it
  routed the kernel-wide `_cs_validate_page`, published a fabricated Mac Pro
  board ID and rewrote matching Tahoe shared-cache pages from `board-id` or
  `hw.model` lookups to that property. It also rewrote a composite DRM model
  string using the current SMBIOS bytes without shared-cache UUID/build
  admission.
- The target iMac20,2 identity already has Intel media support, and the cloned
  TGL accelerator personality now preserves the exact HEVC decode/encode
  capability dictionaries. Keeping a second unversioned model spoof would make
  VideoToolbox results impossible to attribute and expand every process's
  code-signing-page attack surface. The route, fake property, boot argument,
  bundle logs, source files and Xcode entries were removed.
- The AppleGraphicsDevicePolicy `board-id` to `applehax` whole-image string
  patch had the same problem: no Tahoe binary identity, no VF ownership role
  and no headless-accelerator requirement. Its kext registration and mutation
  were removed too. Native AGDP policy is now authoritative.
- `kern_model.hpp` was also deleted after confirming its entire cosmetic
  device-name table and `getBranding` function had no consumer. Capability
  admission remains in the tested `kern_gpu_capabilities.hpp` tables instead
  of a second stale supported-device-looking list.
- `DisplayMergeNub` was likewise unreachable in the product. Its sole
  personality required `DisplayVendorID=0x01053aff`, outside the 16-bit EDID
  manufacturer-ID domain, and its only effect was to rename that impossible
  match to `AppleBacklightDisplay`. It had no VF, accelerator or virtual-display
  role; the metaclass, personality and Xcode entries were removed together.

### Restore Lilu's batch route preflight

- The local `RouteRequestPlus::routeAll` loop called Lilu `routeMultiple` once
  per symbol. This defeated Lilu 1.7.2's first pass, which resolves every
  symbol in a batch before installing the first trampoline; a missing late
  GuC/IRQ symbol could therefore leave earlier routes installed before the
  mandatory admission panic.
- No product route or solve request used the wrapper's pattern fallback. All
  route arrays now use `KernelPatcher::RouteRequest` and one native
  `routeMultiple` call; all symbol arrays use `KernelPatcher::SolveRequest` and
  `solveMultiple`. The dead derived request types, overloads and per-item loops
  were removed.
- The exact pinned Lilu source confirms batch symbol preflight but provides no
  group rollback if trampoline allocation or memory-protection handling fails
  after writes begin. Every caller remains mandatory and panics before the
  accelerator personality is published, so a half-installed payload is never
  admitted to matching; this is the strongest boundary exposed by this Lilu
  API without maintaining a private patcher fork.

### Remove guessed platform-property seeding

- The `ngreenforceprops` compatibility path hardcoded a 0x9a49 platform/device
  identity, 1.5-GiB unified-memory size, an all-zero 0xEA `saved-config`, laptop
  ACPI paths and display/audio labels for every opted-in Gen11+ machine. None
  of those values came from the selected PF/VF, GuC relay or Apple payload.
- The target OpenCore profile already publishes its explicit compatibility
  identity before the driver loads. `kern_green` captures the original PCI ID,
  validates PF/VF capability and restricts config-read spoofing to the selected
  iGPU; the guessed fallback was therefore redundant on the target and unsafe
  as a general Gen11+ feature.
- The parser, two-pass registry writer, boot argument and README option were
  removed. Missing required platform properties must now fail matching or be
  fixed in firmware/OpenCore, rather than being silently fabricated in-kernel.

### Close core-device ownership and remove non-behavioural startup paths

- Pinned Lilu 1.7.2 confirms `DeviceInfo::create()` stores borrowed registry
  pointers and `DeviceInfo::deleter()` only deinitialises its external-GPU
  vector before deleting the inventory. NootedGreen retained no independent
  owner even though later kext callbacks lazily map BAR0 through `iGPU`.
  `processPatcher` now retains the selected `IOPCIDevice` for the lifetime of
  the non-unloadable routed plugin before deleting the temporary inventory.
- The GMS value was decoded into `stolen_size`, but no production source read
  that field. It neither configured Apple's memory manager nor the direct-VF
  GGTT range. The early config-space read, field, helper and isolated oracle
  test were removed instead of retaining a misleading pseudo-contract.
- The IOAcceleratorFamily2 registration performed no solve, route or patch; it
  only logged that native validation was preserved. Removing it eliminates an
  unrelated forced callback without changing native IOAcceleratorFamily2.
- Guest CPUID likewise only contributed log text and could not classify the
  passed-through GPU. Native-TGL selection now records only the original PCI
  identity plus independently proven PF ownership. The disabled custom
  IOService/catalogue skeleton and unused local bit macros were also removed.

### Superseded: public DMA-segment ABI substitution

- This checkpoint incorrectly treated the object returned by `getMemory()` as
  an `IOMemoryDescriptor`. Earlier complete disassembly had already established
  that it is an `IGAccelMemory` owner; the correction and runtime evidence are
  recorded in the current section at the end of this audit.
- `vfCreateUkContext` obtained what it assumed was an `IOMemoryDescriptor` backing from the
  UUID-admitted TGL payload, then invoked the kernel object through a hardcoded
  vtable byte offset `0x158`. The kernel object's vtable is an XNU ABI, not part
  of that payload UUID. More importantly, Tahoe's x86_64 declaration takes
  `offset`, `length` and `options`; the raw function type omitted `options`, so
  its value came from an unspecified argument register.
- Current XNU and the pinned SDK both expose the three-argument virtual method.
  The bridge now calls `IOMemoryDescriptor::getPhysicalSegment` directly with
  `kIOMemoryMapperNone`, then applies the existing nonzero, length and native
  DMA-address bounds checks before publishing any proxy record. This change is
  historical only and has been removed.

### Match the IOService start return ABI

- The routed `IntelAccelerator::start(IOService *)` is an `IOService::start`
  override and therefore returns `bool`, but NootedGreen declared its wrapper
  and trampoline cast as `unsigned long`. On x86_64 a boolean callee guarantees
  AL, not a sanitized 64-bit RAX; reading the full register could convert a
  native start failure into apparent success and publish a failed service.
- The wrapper now has the exact boolean signature and all local failure paths
  return `false`. Native failure remains unmodified and `registerService()` is
  reached only after a true return from the UUID-admitted original body.

### Match the firmware and engine return ABIs

- The pinned Tahoe binary calls `IGHardwareGuC::loadGuCBinary()` and
  `IntelAccelerator::startGraphicsEngine()` and immediately tests AL; their
  native epilogues produce zero or one in EAX. The native
  `stopGraphicsEngine()` epilogue likewise writes one only to AL. These are
  boolean interfaces, but the three VF replacements were declared as
  `unsigned long`.
- All three replacement declarations and definitions now return `bool`, with
  explicit `true`/`false` paths. This removes dependence on unspecified upper
  return-register bits and keeps each routed function's C++ ABI identical to
  its UUID-admitted target.

### Preserve the complete multithreaded force-wake ABI

- The routed symbol `SafeForceWakeMultithreaded(bool, unsigned int,
  unsigned int)` encodes both numeric arguments as `j` in its C++ name, while
  the VF no-op replacement declared the final context argument as `uint8_t`.
  Although the value is intentionally ignored for a VF, calling through a
  mismatched C++ function type is not a supported ABI contract.
- The replacement now accepts the complete 32-bit context argument. It still
  suppresses every force-wake operation only on the UUID-admitted VF path;
  physical devices retain all three native force-wake entry points.

### Do not fabricate a fallible barrier ABI

- Both direct calls to `barrierSubmission` in the pinned Tahoe payload ignore
  RAX, and the native body has return paths that do not establish any return
  value. It is a `void` function. The previous wrapper declared `uint8_t` and
  returned zero when transport or native contexts were incomplete, but neither
  caller could observe that supposed failure and continued without the barrier.
- The wrapper now has the exact `void` ABI. Invalid arguments, an unready VF
  transport, or missing native FIFO contexts are fail-stop conditions because
  silently returning would allow command execution to proceed without required
  barrier/event side effects. An admitted request still enters the complete
  native Tahoe body.

### Make void lifecycle failures fail-closed

- `initDoorbells()` cannot return the unsupported partial-quota condition to
  its caller. It now records a protocol fault for a null object, failed
  bootstrap or non-256 topology, so the immediately following firmware/scheduler
  admission cannot continue with an uninitialized allocator.
- `DetachContextDescFromGucContext()` is also void. When its descriptor or
  backing identity is malformed, any discoverable caller-owned backing is now
  retained for the rest of the boot before returning. The known table record
  was already retained, but that alone did not protect a mismatching second
  object from native destruction and possible late GuC DMA.

### Complete void DMA and ownership failure boundaries

- The same rule also applies before detach can acquire its H2G queue or even
  reach valid context bookkeeping. Those early paths now recover and retain a
  discoverable image backing before recording a protocol fault; a void native
  caller can no longer release untracked pages immediately after the wrapper
  returns.
- Both `IGHardwareGuC::invalidateTLB()` and `IGGuC::invalidateTLB()` are void.
  Their pinned native callers immediately continue updating or retiring shared
  GPU data and never observe a result. Before CTB has ever run they retain
  i915's pre-ready no-op, and after final device quiescence no second request is
  needed. At every live post-CTB boundary, however, a failed heavy GuC
  invalidation is now fail-stop instead of returning with stale translations.
- `IntelAccelerator::transferOwnership()` is likewise void. Native is a no-op
  when legacy ownership flag `0x20` is clear, which remains the admitted VF
  behavior. If that flag is unexpectedly set, the replacement now fails before
  one of its many callers can continue as if the unsupported PCI-config page
  ownership exchange had succeeded.

### Route the exact framebuffer ABI for each admitted UUID

- Offline Mach-O symbol-table inventory found 60 unique installed private
  routes: 57 accelerator symbols and three framebuffer symbols. Every symbol
  must be unique in its applicable pinned payload, and both accelerator bundle
  variants must agree on its address.
- The production framebuffer UUID defines
  `AppleIntelFramebufferController::probe(IOService*, int*)`; the debug UUID
  instead exposes `AppleIntelBaseController::probe(IOService*, int*)`. The old
  VF containment batch always requested the debug symbol, so production would
  panic during routing rather than reject the physical framebuffer cleanly.
  UUID validation now precedes containment, and each payload routes its own
  exact probe plus the common framebuffer-controller start entry.

### Make failed context registration compensation observable

- A direct VF context can reach `REGISTER_CONTEXT` and then fail while
  publishing `UPDATE_CONTEXT_POLICIES`. The attach wrapper returned `false`
  to Apple and attempted a compensating `DEREGISTER_CONTEXT`, but discarded
  both the enqueue result and the wait for `DEREGISTER_DONE`. Its retained
  backing prevented a use-after-free, yet the driver could continue with GuC
  ownership unresolved and no explicit transport quarantine.
- Failed attach is now considered locally retired only when registration never
  reached GuC, or when the compensating deregistration was both sent and
  observed at the tombstone state. Any other result marks a protocol fault and
  deliberately keeps the context image pinned. All eight combinations of
  register/send/completion are covered by the freestanding shutdown model.
- The complete syntax, analyzer, strict-ABI and sanitizer/protocol suite passes
  in `/tmp/ngreen-static.4YeXtu`; the VM remained off.

### Make offline reverse-engineering tools fail closed

- The retired AppleIntelParams runtime header had no consumer, but its old
  1,717-line Ghidra generator still advertised removed route signatures and
  could recreate structures with already documented width/alignment conflicts.
  The generator, C seed/parser output and two generated headers were removed;
  the small `.gdt` database remains inert archival data only.
- Read all current Linux MMIO mapper programs. Exact duplicate provenance for
  one symbol no longer creates false ambiguity, while distinct near-score
  symbols still require review. Linux-source promotion now requires a real
  platform-relevant hit rather than trusting an input `platform_match` bit.
  Approved input is revalidated for canonical platform, unique address/symbol,
  complete provenance and safe comments before a header can be emitted.
- Ghidra access classification now uses the reference type of the operand that
  actually contains the MMIO scalar; every MOV is no longer guessed to be a
  write. Nearby-mask discovery cannot cross the current function boundary.
  Batch output tags include a digest, preventing sanitized relative paths from
  silently overwriting one another.
- A deterministic host test parses every current Python utility, round-trips
  the sample mapping/header and exercises conflicts, source promotion,
  ambiguity, provenance and comment injection. It runs from `check-static.sh`.

### Close the Xcode and CI product boundary

- The project still contained an always-out-of-date shell phase whose script
  was empty but claimed to generate the deleted `kern_fw.cpp`. A second phase
  ran on every build, recursively removed every zip in the target product
  directory, stripped the executable and rebuilt an archive even though CI has
  an explicit, bounded staging step. Both stale phases were removed.
- Debug explicitly disabled the null-dereference, divide-by-zero and dead-store
  analyzers while asking Xcode to run its analyzer, disabled symbols and enabled
  deployment post-processing. Debug now enables those analyzers and symbols
  without stripping; Sanitize likewise retains symbols and avoids deployment
  stripping. Release keeps its deliberate deployment processing.
- `OSBundleRequired` now matches the root-load contract used by the local Lilu
  and WhateverGreen kexts rather than advertising `Safe Boot`. The Lilu version
  requirement remains within the bundled 1.7.2 compatible/current range.
- A new deterministic project contract parses the plist and XML metadata,
  checks all four product sources, proves all 29 headers are transitively
  reachable, validates all three target configurations, scheme/workspace and
  CI filters/stages, and rejects shell build phases. It is part of the full
  static gate, which passes in `/tmp/ngreen-static.tyWTnd`; no VM boot occurred.

### Replace and remove the HookCase compatibility boundary

- Read all seven HookCase source/build files. Its historical IDT, sysent and
  DYLD interposition engine is inside a disabled block; the active service only
  calls its superclass. The appended graphics provider was nevertheless a
  hard runtime dependency. It declared XNU's five-argument
  `gpu_accumulate_time` and one-argument `gpu_describe` as zero-argument no-op
  functions, reversed the 32-bit write bound, underflowed all small MMIO
  lengths, excluded valid 64-bit tail accesses, dereferenced a callback whose
  assignment was commented out, stored a descriptor at an unproven Apple
  object offset and wrote all nine DSB chicken registers before display state
  existed.
- XNU `12377.121.6` proves both telemetry prototypes and implementations.
  NootedGreen now exports those exact ABIs and resolves the originals strictly
  from `KernelPatcher::KernelID`, preserving kernel tracing and per-thread GPU
  accounting without granting the Apple payload `com.apple.kpi.private`.
  Accelerator dependency metadata points to NootedGreen. The complete macOS
  kext link/build passed in GitHub Actions run `36284302650`.
- A deleted ICL framebuffer binary retained in Git history established the
  original register/time/port behavior. Relocations in both admitted Tahoe TGL
  binaries prove every missing 64-bit caller uses the GGTT pointer at controller
  `+0xCA0`; the table length is `+0xCEC`, DSB GPU offset `+0xC50`, size
  `+0xDCC`, and the port configuration pointer `+0x548`. Compatibility exports
  are admitted only for a pinned framebuffer UUID on a native TGL PF. RPL PF,
  VF and unknown identities route framebuffer probe/start to rejection.
- Current i915 defines a Gen8+ GGTT PTE as address plus Present; bit 1 is Gen12
  local memory and TGL bits 4:2 are the VFID. HookCase's `| 7` therefore wrote
  unrelated LM/VFID state. The replacement accepts only the exact 72-KiB DSB
  layout, allocates and zeros one contiguous buffer, validates its whole
  physical range and GGTT window, saves every old PTE, writes address+Present,
  verifies every readback and restores the entire range on failure. It owns the
  descriptor outside the Apple object and never writes the guessed `+0xCD8`
  field. Gen11+ uses an uncached GGTT mapping, so the former speculative
  chicken-register loop was removed; Apple's DSB commit path owns those
  state-dependent values.
- Linux names `0x1638A0` as `PORT_TX_DFLEXDPSP(FIA1)`; HookCase's comment that
  TGL should use HPD register `0x44470` was false. The replacement preserves
  the ICL-derived per-port bit mapping while checking both the configuration
  pointer and register sentinel. Register methods use the central checked BAR
  mapping, `getPMTNow` preserves the clock conversion, and the bounded
  `strnstr` plus three-entry DSB status object have their exact symbols.
- The provider tests prove all 17 required accelerator/framebuffer C/C++
  exports, both plists,
  all sixteen 64-bit caller relocations and both setup calls. Pure tests cover
  DSB boundaries, PTE address/flag rejection and every FIA port mode. The full
  syntax/analyzer/strict-ABI/sanitizer suite passes in
  `/tmp/ngreen-static.sFdng4`. HookCase build/artifact stages and all seven
  files were removed; history remains the recovery path. The VM stayed off.

### Close physical-generation admission and DSB partial-map boundaries

- The UUID-pinned TGL accelerator previously admitted every classified
  physical function. A later-generation PF could therefore receive the RPL-VF
  unaligned-store patch and then execute the remainder of Apple's TGL-native
  register path. Admission now accepts only a native TGL PF or a VF using the
  separately checked virtualization bridge. The full 65,536-ID capability
  model proves that every VF remains eligible for the later transport gate,
  while only exact TGL IDs are eligible as physical functions.
- The TGL DSB provider formerly checked only the first physical page before
  beginning 18 GGTT PTE writes. A physically contiguous allocation whose last
  page exceeded the encoder mask could fail after a partial mapping. It now
  validates the complete aligned interval and precomputes every PTE before the
  first store; readback failure still restores every old entry before the
  backing can be released. Boundary and overflow cases are part of the pure
  provider test.
- The now-unreachable later-generation physical `MultiForceWakeSelect`
  mutation was removed. A non-TGL PF is rejected before personality
  publication rather than retaining dead code that implied unimplemented PF
  compatibility.

### Mark the pinned kernel payloads as root-required collection members

- The first controlled Tahoe boot with NootedGreen and the TGL accelerator in
  dependency order loaded Lilu, WhateverGreen and NootedGreen but did not
  register or start `com.xxxxx.driver.AppleIntelTGLGraphics`. The VF remained
  on `IONDRVFramebuffer`, `system_profiler` reported `No Kext Loaded`, SSH came
  up in 13 seconds and the host journal contained only the expected VF FLRs.
- The accelerator metadata had no `OSBundleRequired` key. Both reviewed TGL
  kernel payloads now declare `OSBundleRequired=Root`. Their contract tests
  require that value together with the exact NootedGreen dependency and reject
  the removed HookCase edge. Later collection-level testing established the
  narrower meaning of this key: it admits the payload to a root-required KC;
  it neither forces an executable to start nor makes a SystemKC-dependent kext
  linkable in OpenCore's BootKC injection phase.
- This is collection-admission metadata, not evidence of acceleration. The
  controlled AuxKC work below supplies the required SystemKC link boundary and
  separately proves that executable loading still needs an explicit,
  patch-before-match trigger.

### Prove the AuxKC boundary and correct the first runtime patch range

- OpenCore 1.0.7's `TestKextInject` reproduced `Invalid Parameter` while
  inserting the pinned accelerator into Tahoe's BootKC. The unresolved set is
  made of IOAccelerator/IOGraphics symbols resident in the SystemKC. Tahoe
  `kmutil create -n aux`, linked against the exact BootKC and SystemKC UUIDs,
  successfully built the same payload instead. The controlled runtime AuxKC
  contains only the five requested driver kexts plus the two pre-existing
  HighPoint kexts; Lilu, VirtualSMC, NootedGreen and WhateverGreen all loaded
  from it without an OpenCore kext graph.
- `OSBundleRequired=Root` did not start the accelerator because its native
  personality deliberately matches `0xff208086`. A controlled
  `kmutil --load-style start-only` request therefore loaded the executable
  without publishing the unpatched native personality. NootedGreen caught the
  payload synchronously and failed closed before hardware start with
  `kextG11HWT Failed to apply base patches!`; the host watchdog stopped only
  the guest. The archived panic is SHA-256
  `5be47bb58aad77798a16359bf81ca53b11a964bea54cb2ac6d047c9ec536a5a2`.
- Direct Mach-O symbol/byte correlation found the defect: the exact-one SKU
  admission anchor is at `0x23c1d` inside `IntelAccelerator::probe()`
  (`0x238e2..0x23c94`), while the six fuse/runtime anchors begin at `0x284cc`
  inside `getGPUInfo()` (`0x2847e..0x28910`). The old code searched for the
  probe anchor inside the latter range, so its panic was deterministic even
  though the unbounded anchor test passed.
- The SKU change is now independently solved and bounded to `probe` through
  `encodeFailureStack`; only the PF-relayed fuse group remains bounded to
  `getGPUInfo` through `teardownDevice`. The Mach-O contract parses both pinned
  binaries' symbol tables and segment mappings, requires every pattern to lie
  in its real owner, and also verifies that production passes the matching
  range to each transactional patch group. The complete offline suite passed
  in `/tmp/ngreen-static.6wdP0t` and commit `8dcc104` passed macOS CI run
  `36287846558`.
- The rebuilt seven-fileset AuxKC is SHA-256
  `a023edfb6a0c10f28b151651f077c89582f616eb7364016de373ff3a23e5f0b8`;
  its NootedGreen UUID is `6C8234B2-04F0-3D81-B4F9-1915C96270E0` and was
  verified as the loaded image before the second start-only request. That load
  passed the corrected `probe` patch and reached the next fail-closed gate:
  `Failed to inject PF runtime fuses into VF getGPUInfo`. The archived panic
  is SHA-256
  `d1c7eddbd9998f770d5f0028fb28f674b7bad270e506c1f5a1ac2a3aeb17dd37`.
- Static overload reproduction identified the exact cause. The six runtime
  replacements are mutable stack arrays. Their five-argument
  `LookupPatchPlus(find, replace, arrsize(find), 1)` initializers preferred the
  array-template constructor, which interpreted the pattern length as the
  required match count and `1` as the skip count. The constant `probe`
  replacement selected the raw-pointer overload instead, explaining why only
  the second group failed. The six calls now use the unambiguous array form
  with count one. Array constructors no longer expose `skip`; intentional skip
  users must provide the raw `(size, count, skip)` form. The Mach-O/source
  contract rejects both the old call shape and a future ambiguous constructor.
  The complete suite passes in `/tmp/ngreen-static.iJN8VH`. A new macOS build
  and controlled load remain required before accelerator start is claimed.

### Preserve naturally aligned PCI identity reads in native probe

- Commit `c64b623` and macOS CI run `36288846991` produced NootedGreen UUID
  `F9BE895C-5C79-33E9-AB60-ABEDD3968422`. The rebuilt seven-fileset AuxKC is
  SHA-256 `74e3e030b29f81358ff717d5c2e8ed156f4bf7d2700881cf79a05b5a19c38137`.
  A start-only load without `-allow3d` completed all 19 VF routes, the PF
  runtime relay, TLB isolation and all fourteen unaligned-store conversions,
  then stopped at Apple's explicit boot-argument gate. The archived dmesg is
  SHA-256 `b027e9b2972e7c8f75d4f7aea2e0ef31c60babbef2cb7ce8fd5d6eb2b84cfbe3`.
- With `-allow3d` present, native `IntelAccelerator::probe` reached its SKU
  mapping and panicked on `0xa7a89a49`. The archived panic is SHA-256
  `f6affc7c78ab215d24a8e80f178420bf8955e977b32fc7848f16b12483e1f4c1`.
  The watchdog stopped only the guest and the host remained responsive.
- Pinned-binary disassembly proves that probe calls
  `IOPCIDevice::extendedConfigRead32(2)`, stores the complete return DWORD and
  later compares it with packed device/vendor identities. Apple's open-source
  IOPCIFamily 726.100.6 proves that extended reads set only the high extended
  register nibble, preserve the raw low offset, and forward it to the bridge;
  the public ABI specifies that DWORD bits 1:0 are ignored. Therefore offset 2
  still returns the DWORD aligned at zero (`device << 16 | vendor`).
- The old helper replaced the low half at offset 2, transforming physical
  `0xa7a88086` into the observed `0xa7a89a49`. The corrected helper changes the
  high half for every naturally aligned representation of the type-0 header,
  handles the corresponding 16-bit rule, and refuses an extended page whose
  low byte aliases the header. The exact panic regression requires
  `0xa7a88086 -> 0x9a498086`.
- The complete syntax, analyzer, strict-ABI, Mach-O and sanitizer suite passes
  in `/tmp/ngreen-static.4JsX4j`, including 33,586,689 PCI identity cases. A
  fresh macOS artifact and controlled AuxKC load remain required before native
  accelerator start is claimed.

### Preserve the native IOAccelerator enable/disable lifecycle on a VF

- Commit `cf8030d` passed GitHub Actions run `36289742462`; its NootedGreen
  UUID is `70AEFDF1-599C-3719-B7A1-9922B69D3F15`. The seven-fileset AuxKC is
  SHA-256 `6e4036598f3656ed78b24691590954717c43b67a0a2cdc4580b26307801e6b17`.
  A watchdog-bounded start-only load reached a registered and active
  `IntelAccelerator` without a panic. It negotiated GuC VF ABI 0.1.17.0,
  obtained the PF-provisioned 64-EU topology and complete GGTT/context/
  doorbell quotas, selected scheduler 4, configured memory IRQ and published
  WindowServer IOAccel clients. The archived dmesg is SHA-256
  `49ae32a4bab9346a7216d4903eeb65345fa67ebafd75aa308236124bf6398db6`.
- That publication was not a Metal success. `MTLCreateSystemDefaultDevice()`
  and `system_profiler SPDisplaysDataType` both blocked while the VM, SSH and
  GUI remained responsive. The root spindump (SHA-256
  `63c284d7f7d2ece22b1046d15316c211b85f710838804dc9f48b67690b3ec8e6`)
  resolves the kernel path as `IOAccelSharedUserClient::new_resource` through
  `IOGraphicsAccelerator2::acceleratorWaitEnabled()` and
  `waiting_for_fEnabled`. The block occurs before a GuC submission or GPU
  interrupt can be blamed.
- Tahoe KDK disassembly proves `configIsHeadless()` is only
  `displayMachine->getFramebufferCount() == 0`; there is no personality flag
  that makes resource creation bypass `fEnabled`. `enableAccelerator()` starts
  the hardware-progress timer when applicable and sets that state bit, while
  `disableAccelerator()` clears it symmetrically.
- Both admitted TGL payloads prove the missing owner. Their native
  `IntelAccelerator::startGraphicsEngine()` calls `IGInterruptBridge::enable()`
  and then `IOGraphicsAccelerator2::enableAccelerator()`; native stop calls the
  corresponding bridge disable and IOAccelerator disable in the same order.
  The VF replacement returned true without any of those four lifecycle calls,
  so native start reported success while every first resource waited forever.
- The VF replacement now retains its PF-owned engine/reset isolation but calls
  the already VF-routed interrupt-bridge lifecycle and Tahoe's named
  IOAccelerator enable/disable methods. The latter are resolved from the
  running `com.apple.iokit.IOAcceleratorFamily2` image instead of writing its
  private flags by offset. Start requires a complete GuC transport and both
  lifecycle APIs; missing state fails closed. Final stop still quiesces every
  context/DMA owner before disabling the bridge and accelerator.
- A new Mach-O contract parses both payloads' external relocations, proves the
  native bridge/IOAccelerator call order and checks the production wrapper for
  the same ordering. The full syntax, analyzer, strict-ABI, Mach-O and
  sanitizer suite passes in `/tmp/ngreen-static.5PbbMZ`. A fresh macOS build
  and controlled runtime must still prove that Metal proceeds beyond resource
  creation; this checkpoint does not claim completed GPU work.

### Contain the nested scheduler IRQ lifecycle and initialize IOAccel events

- Commit `640f087` passed GitHub Actions run `36290864027`; its NootedGreen
  UUID is `49D4E12B-82D4-3075-89FE-8734BBB5F04A`. The activated seven-fileset
  AuxKC is SHA-256
  `a2b03119761be8d49a761a682a3fa6f34048da8f3dff791a5d1df28c35bbed49`.
  A watchdog-bounded start-only load did not merely lose networking: the next
  boot saved `Kernel-2026-09-27-112300.panic`, SHA-256
  `5687995b9a56641ed4f32dc42712d761b57e543b4a519fe65e10dce49eb0dac9`.
  Native `IntelAccelerator::start()` had entered its failure rollback and the
  routed final-quiescence guard deliberately panicked rather than let native
  stop release possibly live VF DMA mappings.
- Re-disassembly of the complete nested enable path exposed the missed owner.
  `IGInterruptBridge::enable()` first reaches the already-routed bridge
  `enableInterrupts()`, but later calls scheduler vtable slot `+0x1a0`.
  For the forced VF scheduler 4 this is
  `IGScheduler4::enableInterrupts()`, which calls
  `enableErrorInterrupts()` and then invokes
  `IGHardwareCommandStreamer4::enableErrorInterrupt()` for every engine. That
  last routine directly writes the physical error-mask and error-enable MMIO
  registers. The disable path is symmetric. Thus isolating only the bridge
  entry points did not isolate the complete transitive IRQ protocol.
- Both scheduler-4 enable/disable entries are now routed, on a classified VF
  only, to the idempotent memory-IRQ mask handlers. The outer bridge still owns
  event-source/requestor bookkeeping; the nested route prevents physical
  command-streamer error-register access. PF execution remains native.
- The native engine-start tail also calls
  `IOAccelEventMachineFast2::initEvent()` for the two embedded events at
  accelerator offsets `0x11a8` and `0x11e8`, immediately after enabling the
  IOAccelerator. The VF replacement now resolves that named IOAcceleratorFamily2
  API and preserves both calls and their exact order. These are software event
  ownership transitions, not PF engine programming.
- The Mach-O lifecycle test now proves, in both admitted payloads, the bridge /
  IOAccelerator order and the transitive scheduler-to-command-streamer physical
  error-IRQ calls that require containment. The route inventory is 62 unique
  symbols (59 accelerator, three framebuffer). Full syntax, zero-finding Clang
  analyzer, strict ABI, Mach-O, exhaustive protocol and sanitizer tests pass in
  `/tmp/ngreen-static.AI7UAt`. Dynamic validation remains watchdog-bounded; the
  native safe-context-image tail after event initialization still requires
  explicit runtime evidence before Metal success can be claimed.

### Allocate the VF MSI before the legacy local-filter bridge

- Commit `102ec33` passed GitHub Actions run `36291885180`; its NootedGreen
  UUID is `4B5D949F-84E8-389F-95B3-FFD1C7ACA753`. The minimal three-fileset
  AuxKC (Lilu, NootedGreen and AppleIntelTGLGraphics) is SHA-256
  `e36f9f22337cce36e137f376a5771a884aa8f68dc85355f82f6998500a8de82c`.
  Both the active collection and loaded NootedGreen UUID were verified before
  a watchdog-bounded `start-only` request.
- The new scheduler containment passed every earlier bootstrap boundary: GuC
  ABI `0.1.17.0`, PF-relayed 64-EU topology, direct GGTT, context/doorbell
  quotas and scheduler 4 were all admitted. The final live kernel log stopped
  at `Failed to register with service. Using Local Filter Interrupt Source!`.
  The watchdog isolated the guest; the next boot preserved panic SHA-256
  `ac40712d2e1a2e2ac39fe3f9f1734b24128eda5fc0a3d30610c0d75a7da456f4`.
  As in the prior attempt, native start entered its common failure rollback;
  the final-stop guard then refused to release mappings without a quiescence
  boundary. The log stream is retained under
  `build/diagnostics/102ec33-runtime1` in the VM workspace.
- Complete `IGInterruptBridge::initInterruptBridge()` disassembly explains the
  boundary. A headless accelerator passes no framebuffer service, so Tahoe
  deliberately calls `createFilterInterruptEventSource()` on its PCI provider.
  The guest's VF publishes an MSI capability at config offset `0xac` with one
  64-bit vector (`IOPCIMSIMessageControl=0x100`) and has no legacy INTx route,
  but it has not yet published `IOInterruptSpecifiers` or
  `IOInterruptControllers` when the legacy TGL driver reaches this path.
- Apple IOPCIFamily 726.100.6 and the running 25G229 BootKC both export
  `IOPCIDevice::configureInterrupts(unsigned, unsigned, unsigned, unsigned)`.
  Its source allocates the requested MSI through the platform message
  controller, publishes the real interrupt controller/specifier arrays and
  initializes the device's MSI state. The VF start wrapper now calls that ABI
  for exactly one required/requested `kIOInterruptTypePCIMessaged` vector
  before entering native accelerator start. Failure is reported before the
  old driver can construct a filter against a nonexistent source; no interrupt
  property or vector number is fabricated.
- Lilu symbol lookup is scoped to a registered Mach-O, not the outer BootKC
  filename. NootedGreen therefore registers `com.apple.iokit.IOPCIFamily` and
  resolves the allocator from that fileset's bounded running address range;
  it deliberately does not ask `KernelPatcher::KernelID` for a PCI-family
  method.
- Early native-start rollback is now a separate, proven shutdown boundary.
  After closing and draining the context-operation gate, a never-enabled CTB
  may be declared DMA-quiescent only if every quota-sized context entry is
  empty and owns no backing/reference/pending token. The stable table is
  scanned without disabling interrupts. Any ownership still fails closed; a
  genuinely pre-transport rollback can unwind instead of panicking solely
  because its GuC object was never constructed.
- The lifecycle contract requires the exact exported symbol, proves MSI
  allocation precedes the original start, requires the exact one-vector
  request and checks every early-quiescence predicate/publication. Full syntax,
  zero-finding analyzer, strict ABI, Mach-O, exhaustive protocol and sanitizer
  tests pass in `/tmp/ngreen-static.QwNnvP`. A macOS build and controlled load
  remain required before the interrupt source or later engine tail is claimed.

### Restore the scheduler firmware boundary before accelerator enable

- Commit `079d0da` passed GitHub Actions run `36293258176`. Its x86_64 kext
  UUID is `6A412A9A-4175-30B8-BB58-CFEAEAC923B6`, executable SHA-256 is
  `b668dccd2cb0b56afdee34e6bf8343eaf234f503c76568d198de869705852b33`
  and minimal AuxKC SHA-256 is
  `b4ebe4784de8c1fba2573547a6df1731578dbcc16e82bf357d1d3883594d1294`.
- A 45-second watchdog-contained `start-only` load stayed responsive. Tahoe
  allocated and published a real `IOPCIMessagedInterruptController` plus one
  `IOInterruptSpecifiers` entry on the VF. Native start then reached the local
  filter path, called the VF engine wrapper, returned failure `0x214`, and the
  new pre-CTB proof safely quiesced and unwound every accelerator object. No
  new panic was recorded. Evidence and hashes are retained under
  `build/diagnostics/079d0da-runtime1` in the VM workspace.
- Complete disassembly explains failure `0x214`. The pinned native
  `IntelAccelerator::startGraphicsEngine()` runs physical force-wake, mode,
  cache and workaround MMIO first, then calls
  `IGScheduler::initFirmware()`, checks its `IOReturn`, enables the interrupt
  bridge/IOAccelerator and initializes the two embedded completion events.
  The VF replacement correctly omitted the physical prefix but incorrectly
  required `vfNativeGpuWorkReady()` before calling the scheduler boundary that
  creates GuC, translates legacy CTB registration into VF self-config KLVs and
  enables memory IRQ delivery.
- The replacement now resolves and calls the exact scheduler firmware method,
  accepts only `kIOReturnSuccess`, proves the complete GGTT/memory-IRQ/CTB
  transport state and only then admits the interrupt and IOAccel lifecycle.
  The immediately preceding native virtual call was also identified exactly as
  `IGMemoryManager::initCache()`: its body directly writes force-wake, MOCS, L3
  and other physical registers, so it remains deliberately omitted for a VF.
  A detailed post-firmware state log makes any later incomplete boundary
  attributable without speculative hardware access.
- The expanded lifecycle ordering contract and complete syntax/analyzer,
  strict ABI, Mach-O, protocol-model and sanitizer suite pass in
  `/tmp/ngreen-static.KnNcGb`. A fresh macOS build and watchdog-contained load
  remain required before GuC transport admission is claimed.

### Admit VF completion delivery before post-CTB GGTT allocation

- Commit `1b5736d` passed GitHub Actions run `36293818737`; its x86_64 kext
  UUID is `74721B3A-3168-33A2-86ED-1E9D079F8E4F`. A watchdog-contained load
  reached doorbell allocation, CTB backing/layout, scheduler data, memory-IRQ
  setup and successful H2G/G2H registration. The next boot preserved
  `Kernel-2026-09-27-122656.panic`, SHA-256
  `9e15415eb86aa2bf8ce499e173692b4d42bf8a117c7be4c4ffeddf9d2cf64374`.
  The fail-stop was intentional: partial `IGHardwareGuC::initWithOptions()`
  unwind tried to release a GGTT mapping after its synchronous GuC TLB
  invalidation had timed out, so NootedGreen refused to free backing without a
  proven DMA boundary.
- Complete pinned-payload disassembly locates the failure after CTB enable but
  inside the first `createUkContext()` proxy allocation. Tahoe enabled the
  interrupt bridge only after `IGScheduler::initFirmware()` returned, while
  that method creates GGTT-backed proxy state and every such post-CTB mapping
  waits for a `TLB_DONE` G2H message. This was a circular dependency: the
  completion producer was live, but its MSI consumer was not yet admitted.
- Current i915 establishes the same ordering explicitly: prepare VF memory
  interrupts, enable CT, enable GuC interrupts and consume messages crossing
  the enable boundary before later submission setup. The VF lifecycle now
  enables Tahoe's already-contained bridge before scheduler firmware init.
  The bridge retains native event-source ownership, while its force-wake,
  master-IRQ and nested scheduler error-IRQ hardware paths remain routed to
  VF-safe memory interrupts. Firmware failure closes the early bridge before
  reporting failure.
- CTB enable now clears the memory-IRQ source and synchronously drains G2H once
  before publishing legacy registration success. This closes the edge race in
  which an MSI can arrive between firmware enable and `gVfCtbEnabled`
  publication. The normal callback and boundary drain share the same G2H lock.
  Race-free one-shot diagnostics identify each stage of the first proxy
  context without logging every scheduler allocation.
- A narrowly bounded failed-bootstrap rollback preserves recoverability. It is
  available only before scheduler firmware initialization completes, after a
  protocol fault, with valid MMIO and the exact GuC owner. It closes and drains
  the context-operation gate, proves every entry in the complete quota-sized
  direct-context table is empty and unowned, masks and drains IRQ callbacks,
  and requires an independent MMIO `CONTROL_CTB` disable acknowledgement before
  publishing DMA quiescence. It is forbidden once scheduler initialization has
  succeeded, where the normal context retirement, TLB and CTB shutdown proof
  remains mandatory.
- Source contracts enforce the early-MSI / firmware / readiness / IOAccel
  order, firmware-failure bridge closure, complete ownership predicates, CTB
  boundary drain and rollback guards. Full syntax, zero-finding analyzer,
  strict ABI, Mach-O, exhaustive protocol and sanitizer checks pass in
  `/tmp/ngreen-static.MaBknS`. A fresh CI-built macOS artifact and controlled
  runtime are still required; this checkpoint does not claim a completed GuC
  command or Metal acceleration.

### Service late GuC IRQ registration and poll synchronous completions

- Commit `8bf2f49` passed GitHub Actions run `36294970584`; its x86_64 kext
  UUID is `C3443889-1C07-30B9-B177-797666A04FA6`, executable SHA-256 is
  `916707f7e8c48a85c5a2593047608307949071487a75797ae6f6f0a462a9daed`
  and the correctly path-bound candidate AuxKC SHA-256 is
  `227eed7bc9f53a4f927d30b4d26603a6f1e474b32b72872cf207b69c9768fce3`.
  Building an earlier candidate from a temporary kext path caused Lilu to
  return `TooLate` on manual load; rebuilding from its final
  `/Library/Extensions/NootedGreen.kext` path restored early patch admission.
  This is now a deployment invariant, not a driver workaround.
- The watchdog-bounded corrected load reached CTB registration and the first
  proxy-context GGTT mapping, then timed out its sequence-zero `TLB_DONE` wait.
  No memory-IRQ or mapped-stage log preceded the timeout. The guest was
  isolated by its watchdog. On the next boot the preserved panic
  `Kernel-2026-09-27-125343.panic` has SHA-256
  `63cb332f475c63722e9004510007f753890de2bbbc2e3ffa054363b1029eda80`;
  it is a null-vtable call at `IGHardwareGuC::withOptions+0x3b` during the
  partial-init failure path. Evidence is retained under
  `build/diagnostics/8bf2f49-runtime2` in the VM workspace.
- Complete bridge disassembly identifies the lost completion owner.
  `IGInterruptBridge::enable()` tests byte `+0x8a8` at entry and returns
  immediately when already enabled. Its first invocation walks and clears the
  requestor list at `+0x8b8`. Later, `IGHardwareGuC::initInterrupts()` creates
  the software event source and calls `requestEnableCallback()`, but that
  method only retains and appends the GuC requestor to `+0x8b8`; it never
  observes the already-enabled state. The early bridge ordering therefore
  made the GuC hardware callback permanently unreachable.
- A VF-only route now preserves native queuing before bridge enable and invokes
  callbacks synchronously only when `+0x8a8` is already set. The native GuC
  callback then executes its complete `registerForInterruptType()` path:
  provider registration, source enable and service-handler publication are
  retained. The wrapper does not add a list node or retain in the immediate
  case, so no orphan requestor survives teardown. Pinned binary contracts prove
  the native one-shot guard, list append and GuC call edge in both payloads.
- MSI remains the normal asynchronous path. Every synchronous TLB/context
  waiter, H2G backpressure loop and final CTB drain now also invokes the same
  lock-serialized G2H consumer in bounded 256-frame slices before sleeping.
  This matches the explicit current-i915 event-handler boundary and prevents a
  coalesced/lost edge from stranding a synchronous firmware transaction. The
  poll is forbidden on the GuC workloop by the existing wait admission check;
  it neither spins nor bypasses message validation, reply-credit accounting or
  protocol quarantine.
- The panic exposed an independent native Tahoe cleanup defect. Every
  `IGHardwareGuC::initWithOptions()` failure converges on a virtual `free()`;
  XNU `OSObject::free()` deletes the object. `IGHardwareGuC::withOptions()` then
  made a second virtual `release()` through that freed object before returning
  null. A VF-only, UUID- and symbol-bounded nine-byte patch removes exactly the
  second dispatch. Native cleanup and the following null return remain intact;
  the success path is unchanged. Offline tests require the exact factory and
  init failure anchors in both admitted binaries.
- The route inventory is now 63 unique symbols (60 accelerator, three
  framebuffer). Full syntax, zero-finding Clang analyzer, strict ABI, Mach-O,
  exhaustive protocol and sanitizer tests pass in
  `/tmp/ngreen-static.n434NF`. This checkpoint still does not claim a completed
  `TLB_DONE`, GuC context lifecycle, submitted GPU command or Metal result;
  those require the next CI-built watchdog-contained runtime.

### Correct proxy DMA ABI after the first completed VF TLB transaction

- Commit `06d43ac` passed GitHub Actions run `36296255226`. Its x86_64 kext
  UUID is `DE3EFC42-7B45-3F6B-8F7C-F2E91AF57B1C`, executable SHA-256 is
  `878592ae44cff182927d76db07812dc760ebe294fe950b657bb30f92f2bb9e2a`
  and the correctly path-bound candidate AuxKC SHA-256 is
  `b21eca6f63ef3b83cb08353020b9c2fbbaf897213912576422cd6a659ecab56e`.
  Its watchdog-contained runtime retained the immediate late-callback path,
  enabled both CTB channels, configured the memory-IRQ page and consumed the
  first sequence-zero two-dword `TLB_DONE` successfully. This is the first
  direct evidence that a post-CTB GGTT map, H2G enqueue, PF/GuC execution and
  G2H completion all crossed the VF transport. Evidence is retained under
  `build/diagnostics/06d43ac-runtime1` in the VM workspace.
- The next failure was deterministic rather than a lost interrupt. The guest
  panic `Kernel-2026-09-27-131546.panic`, SHA-256
  `8f840b3fca7c54ee4cc794c9302581009ad3522c1665af9d5619da03f7979764`,
  shows `vfCreateUkContext` rejecting the newly mapped process backing and
  releasing it through `IGMemoryManager::releaseFromPageTableForTask`. The old
  rejection first set `gVfProtocolFault`; the void VF unmap then correctly
  refused to release DMA backing without usable TLB transport and panicked.
  Thus the crash exposed both the validation error and a provably reversed
  local-failure rollback order.
- Complete pinned disassembly corrects the validation ABI. `IGMappedBuffer::
  getMemory()` returns the private `IGAccelMemory` owner at map offset `+0x18`;
  it does not return the owned `IOMemoryDescriptor` at the additional `+0xd0`
  dereference. Native `createUkContext` calls virtual slot `+0x158` with exactly
  `(offset, length)`. The concrete symbol is
  `IGAccelSysMemory::getPhysicalSegment(unsigned long long, unsigned long
  long *)`; its body obtains MemoryManager mapper options at `+0x88` and passes
  them to the owned IOMD's three-argument segment method. The former direct
  `IOMemoryDescriptor(..., kIOMemoryMapperNone)` call therefore used the wrong
  object type and bypassed the DMA/IOMapper address contract used by GGTT map.
- The bridge now resolves that exact private symbol under the already-required
  TGL payload UUID, verifies `metaCast("IGAccelSysMemory")`, calls its exact
  two-argument ABI and retains the nonzero/length/39-bit encoder bounds before
  publishing the proxy record. Both payload tests prove the native caller's
  virtual call shape and the concrete method's accelerator, IOMD, mapper-option
  and underlying segment-call offsets.
- Local allocation, shape or ownership rejection during scheduler construction
  now atomically stops new submission without poisoning CT retirement. Native
  `IGHardwareGuC::initWithOptions` synchronously frees unpublished mappings
  before returning its failure; only the outer engine-start wrapper records the
  terminal fault after that unwind. Pre-CTB faults are also permitted to unmap
  because no CT request or GPU translation could yet have consumed them.
  Transport corruption still marks a protocol fault and retains the existing
  fail-stop/quarantine behavior.
- Full syntax, zero-finding Clang analyzer, strict ABI, Mach-O, exhaustive
  protocol and sanitizer tests pass in `/tmp/ngreen-static.V60fJG`. The VM was
  restored to its original AuxKC before this static correction; a fresh CI-built
  artifact and watchdog-contained load are required before the first proxy
  context, workqueue or scheduler completion is claimed.

### Reach first Metal direct submit and isolate the Blit3D scratch mapping fault

- Commit `209be40` passed GitHub Actions run `36297398362`. Its x86_64 kext
  UUID is `ECB4A45C-E787-3BA7-8C85-1B8127CCE299`, executable SHA-256 is
  `54385c0679dcfe9c6adeb316e2ff66d980e4d4d8eb8abf5ff4438e1154506446`
  and its correctly path-bound minimal AuxKC SHA-256 is
  `9747ca779a8ce20f3345c029b62ef5436797c85dfcab0dd4f1c36ac558406b72`.
  The complete procedure and raw evidence are retained under
  `build/diagnostics/209be40-runtime1` in the VM workspace.
- The proxy DMA repair passed its intended boundary. The first proxy context
  acquired and validated its `IGAccelSysMemory` physical segment, allocated its
  workqueue and completed scheduler firmware initialization. IOAccelerator was
  enabled and published. The Metal smoke test created its device, queue,
  resource, command buffer and blit encoder, encoded and committed the fill,
  then entered its first completion wait.
- During that real client path Tahoe registered render, video/compute and blit
  GuC contexts. Direct submission of context `55223`, LRCA `0x419e7319`, class
  `3`, tail `0x58` returned success with fence `75`; the subsequent lifecycle
  message placed it in state `5`. This is runtime evidence of Metal client
  admission, GuC context registration and direct submit, but not of completed
  GPU work.
- The saved panic `Kernel-2026-09-27-133649.panic`, SHA-256
  `340807dd82b222f4d2e72ccf730cc378940a75f30de4139ec058fbf9da23d085`,
  identifies `metal-smoke-209be40` as the panicked task. `_memcpy+0x7`, called
  by `blit3d_initialize_scratch_space()` from
  `IGHardwareBlit3DContext::initialize()+0x4c`, attempted a `0x44`-byte write
  at CPU-map `base+0xd000`. The base in RBX was `0xffffff9020142000`; CR2 was
  exactly `0xffffff902014f000`, proving the final page was absent rather than a
  GuC wait or interrupt timeout.
- Pinned disassembly makes the next compatibility boundary exact.
  `blit3d_scratch_space_size` is `0xd240`; the extended-context factory passes
  it to `IGSharedMappedBuffer::withOptions`, and the native scratch initializer
  writes through offset `0xd20f`. The observed CPU mapping stopped at
  `base+0xd000`. The removed V69/V73 experiment recorded the same address and
  avoided it by skipping the entire native initializer; that partial-context
  fabrication remains prohibited.
- The next repair must compare Tahoe's last supported Intel implementation and
  both admitted payloads at the complete shared-buffer allocation boundary,
  independently validate logical length, backing IOMD length and CPU mapping
  span, then provide a UUID-bounded page-rounded `0xe000` native allocation or
  fail before the first write. It must preserve the real Blit3D initializer and
  ownership rather than restore the old stub. An offline contract must prove
  every scratch write fits before another runtime.
- The host watchdog isolated only the guest. The tested candidate and kext are
  archived in the guest; the active collection was restored to the original
  SHA-256 `041a15e0415a2756a22e53f0c8e7dc348df01afd78cfc76424cb7488094b1fdb`.
  The VM is running and reachable with no Lilu, NootedGreen or TGL image loaded.

### Isolate pre-engine telemetry and OA hardware access on a VF

- A new complete static pass over Tahoe's telemetry graph found another path
  that runs before `startGraphicsEngine()`. `IntelAccelerator::start()` calls
  `telemetryCreateManager(1)` before HWS allocation and engine start. Native
  `IGTelemetryManager::initWithAccelerator()` then performs four force-wake
  transitions, reads raw `MMIO+0x145998`, writes `MMIO+0x2360`, allocates an OA
  buffer and enables OA collection. This is a PF-owned register path and is not
  safe merely because the accelerator later replaces engine start.
- The source `TelemetryDisable` property is insufficient containment. It gates
  trace-stream startup and three context-image patchers, but it does not gate
  manager construction, IOReport global usage, sysctl operations, user-client
  OA init/read/map, dashboard access or per-stamp usage reporting. In
  particular, `IGTelemetryUsage::reportGlobalUsage()` directly reads
  `MMIO+0x145948`.
- Seventeen exact symbols are now routed only for a classified VF. Manager init
  preserves the native object and publishes only its exact software ownership
  fields; manager operations, dashboard, global usage, context patches and OA
  user calls either return unsupported or have no hardware side effect. The 64
  native per-stamp objects are retained for ABI/lifetime compatibility, but
  their GPU/metadata allocation hook leaves the documented allocation-failure
  state and all sampling/reporting entry points are inert. PF execution keeps
  all native methods.
- Teardown is not guessed. Pinned constructor disassembly proves the manager's
  performance-config pointer, OA fields and usage-pointer array begin zeroed.
  Pinned destructor disassembly proves embedded OA finalization first checks
  its two zero reference counters, while usage teardown subtracts zero sizes
  and null-checks both buffers. No fabricated retain, backing or MMIO restore is
  introduced. The complete direct graph from calc/operation/context patch/OA
  APIs to every dangerous helper is fixed for both admitted payloads.
- The injected VF personality separately clones the shallow `Development`
  dictionary and forces `TelemetryDisable=1`; the two source PF personalities
  remain at zero. Accelerator start independently requires the same root
  property so a failed policy publication aborts before native start.
- `tools/vf_telemetry_isolation_contract_test.py` pins the native call order,
  every MMIO/OA anchor, all 17 route mappings, software object offsets,
  zero-state teardown and PF plist policy. The complete route inventory is now
  82 unique symbols (79 accelerator and three framebuffer). This checkpoint is
  static only: the host i915/DMAR hard hold remains in force, the VM was not
  started, and no candidate was installed or loaded.
# Display terminal virtuals and inherited external dispatch follow-up

Offline Tahoe 25G229 paired-KC review: base display-pipe vtable slots
`header+16+0x868` and `header+16+0x8e8` resolve respectively to
`displayModeWillChange` (0x34 bytes) and `framebufferTerminated` (6 bytes).
The former finishes queued transactions and, unless accelerator byte `+0xcec`
has bit 1 set, releases the live transaction; the latter is a base no-op.
Subclass overrides remain unreviewed. Neither notification establishes DMA
quiescence or closes all external producers.

The display user-client `externalMethod` wrapper (0x2e bytes) selects one of
14 24-byte descriptors and delegates to the inherited implementation. Its
SystemKC import at `0x14bcf088` resolves through cache level 0 to BootKC
`__ZTV12IOUserClient` at `0xffffff800027b778`. This is the table HEADER:
the explicit base-call offset `+0x860` resolves to `externalMethod` at
`0xffffff8000b00640`; do not add another 16 bytes as for an object vptr.

Reviewed instructions through the inherited return (0x3be bytes): the
non-null descriptor path validates scalar/structure argument counts, obtains
memory-descriptor lengths when needed, and tail-dispatches its action. It
does not itself acquire the accelerator busy mutex or check the pipe terminal
flag. Null-descriptor legacy helper branches and their jump-table targets
exist; their callees are not newly certified here. Higher-level user-client
entry dispatch, descriptor actions, subclass overrides, registration failure
unwind and concurrent lazy setup still require review. No production hook or
runtime test was introduced. Paired-KC tests pin hashes and resolved targets;
these are provenance checks, not a concurrency or hardware-success proof.
# Selector 8 transaction-end dispatch follow-up

Offline paired-KC evidence pins display descriptor 8 at `0x14be3950+8*24`:
cache-level-1 action `s_transaction_end`, argument counts `(0,280,0,0)`.
The 14-byte action forwards structure input (`arguments+0x30`) to the already
reviewed user-client `transactionEnd`; it does not add synchronization.

Reviewed complete base pipe `transaction_end` span `0x14bb0560..0x14bb0714`
(0x1b4 bytes, including cold-call edges/padding). It searches pending
transactions by ID, validates doubly linked-list relationships, unlinks the
selected transaction, invokes virtual `+0x8d8`, sets transaction arguments and
calls `prepare`. The latter two callee bodies and the virtual override are
not certified by this review. Failures store status in transaction `+0x58`
and preserve return status in `r12d`; both failure and success flow into
`transaction_queue` at `0x14bb06a3`. Successful preparation also iterates
resource/event entries and invokes event-machine virtual `+0x1b8` before
queuing. Queue return is not substituted for the preserved status.

No explicit pipe terminal test appears in this base body; the upstream
user-client checks and lock/wait/recheck sequence remain relevant. Error
transactions being queued may support notification/retirement semantics;
this is not established as a defect or safe to suppress without reviewing
the downstream queue and destruction paths. List-validation cold targets
contain trap instrumentation; they are not normal error-return cleanup.
Paired-KC provenance tests pass. No production patch or runtime operation.
# Error transaction queue and release follow-up

Offline Tahoe 25G229: complete `transaction_queue_gated` (0xac bytes),
transaction `prepare` (0x150), transaction `free` (0x1e2), base pipe
`submitTransaction` (0x14) and `beginTransaction` (6) are reviewed and pinned.
Base vtable slots `header+16+0x8b8`/`+0x8d8` resolve to the latter two methods.

The gated queue calls virtual `+0x8b8` and overwrites transaction status `+0x58`
with its result. Base submit preserves nonzero prior errors, otherwise returns
`0xe0014042`; base begin is a no-op. Actual framebuffer subclass overrides
remain unresolved, so base behavior is not a hardware-success proof. Queue
stores the transaction in one of four embedded event slots, calls event-machine
virtual `+0x1b0`, executes `sfence`, advances producer index `+0x23c`, then
signals source `+0xc0`. No local terminal/capacity check exists here; upstream
capacity/admission and effective gate binding are still required.

Prepare visits resource slots in two plane records. After `checkDirty`, it
calls resource virtual `+0x170`, counts successful calls and rolls back a
partial failure through `+0x178`; success sets transaction flag `+0x140` bit 0
and calls an imported helper on associated objects. Virtual identities and
the imported helper remain pending; do not label them proven DMA pin/unpin.

Transaction free, when its async reference `+0x158` exists, copies transaction
ID to `+0x164`, calls `sendAsyncResult64` with status argument zero and 11
result words, clears the reference and releases retained client `+0x150`.
Thus the outer async status argument is not itself the transaction `+0x58`
error; exact result payload semantics/callee remain pending. It then releases
resource/auxiliary objects from the two plane records and remaining retained
fields, clears/deallocates the backing record and delegates inherited free.
No standalone DMA drain is established by these releases. Notification and
retained-object cleanup explain why indiscriminately dropping an error queue
entry is unsafe; complete queue-to-finished ownership transfer and actual
subclass submission still need verification. Paired-KC tests passed locally;
no production/runtime mutation was made.
# Intel display transaction override follow-up

Both pinned TGL accelerator payload variants expose `IGAccelDisplayPipe`
vtable `header+16+0x8b8`/`+0x8d8` pointing to Intel submit/begin overrides.
Complete begin (0x4e) obtains the event machine through two external
`getEventMachine` relocations, invokes event-machine virtuals `+0x1d8` and
`+0x1d0` using pipe index `+0x390` and the event argument, then sets byte
`+0x133a` to 1. Concrete event-machine virtual targets remain pending.

Complete submit (0x5e), when that byte is nonzero, obtains the event machine,
invokes `+0x1e0` with index, zero argument and stack output, and again stores
1 in the byte. It then calls an imported `IOAccelLegacyDisplayPipe` table
at explicit header offset `+0x8c8`. Relocation `0xc81c0` establishes that
import identity; branch relocations at `0x80dfc`, `0x80e16`, `0x80eb0`
establish the getter identity. Their zero displacements are not local calls.
The paired Tahoe SystemKC legacy table resolves `header+0x8c8` to the
previously reviewed base submit preserving prior errors or returning
`0xe0014042`; there is no distinct legacy submit body at this slot.

This resolves the static delegation, not runtime vtable mutation, event
progress, physical scanout or Metal/encoder acceleration. In particular,
do not replace that base status with fabricated success to obtain a desktop.
VF virtual-display/capture feasibility remains a separate goal requirement.
Both native payload contract tests and paired-KC checks pass locally; no
production route, hardware test or deployment was introduced.
# Display stamp-record construction follow-up

Both pinned Intel payload event-machine vtables import Fast2 methods at
object slots `+0x1d0` setEventStamp, `+0x1d8` incrementStamp and `+0x1e0`
writeStampCommand; relocation entries `0xcebd8/0xcebe0/0xcebe8` establish
identities rather than treating on-disk zero pointers as runtime null.
The paired SystemKC bodies/virtual identities were already pinned and were
re-read for this display caller: increment changes requested stamp and an
accelerator counter, setEventStamp merges requested dependencies (with mapped
stamp reads and an overflow helper), and writeStampCommand ignores its queue
argument and tail-dispatches event-machine virtual `+0x2a0` with record output.
The dependency overflow helper remains separately pending.

Intel `+0x2a0` resolves to complete `writeStamp` (0x22 bytes), which invokes
virtual `+0x138` getStampOffset (0xc bytes, `index << 6`) and writes the two
32-bit fields `{offset, requestedStamp}` into the supplied CPU record. These
complete bodies, local vtable slots and record stores are now pinned in both
native-payload checks. They contain no GPU submission or completion wait.
This caller writes to stack output in Intel display submit; downstream record
consumers and actual display/engine retirement still need review. CPU record
construction and requested-stamp increments are not hardware execution proof,
and may not be substituted for a genuine completion observed from GPU backing.
Targeted native checks pass; no driver deployment or runtime experiment.
# Display completion, notification and prepared-resource follow-up

Re-reading the complete Intel submit override confirms its stack stamp record
is not subsequently loaded or passed onward in that body: after record
construction it stores the state byte and delegates legacy submit. No hidden
record consumer in this override is established. Other writeStamp callers
and their GPU-command consumers remain a separate review task.

Complete SystemKC pipe `completeTransaction` (0x152 bytes) cancels the timeout
timer, updates last-transaction metadata, conditionally sends notification,
appends the previous live transaction to the finished list, clears active
`+0x248`, replaces live `+0x250`, invokes signalTransactionComplete virtual
`+0x8e0`, wakes the command-gate waiters and delegates an accelerator wakeup.
The imported object helper, signal override and wakeup callee are not newly
certified. This method's name does not independently establish GPU completion.

Transaction `complete` (0x68) and `finish` (0x64) both require prepare-success
flag `+0x140` bit 0. Complete invokes resource virtual `+0x178` on the two
plane resource pairs; finish invokes an imported helper on associated objects.
Neither clears that flag in its reviewed body. Correct one-shot ownership and
call order therefore remain obligations of callers; repeated release safety
cannot be inferred from these names or the flag alone. Resource virtual/import
identities and native backing lifetime still require review.

Transaction `sendNotification` (0x64) sends 11 result words with outer status
zero, records send's result, clears async reference `+0x158`, releases/clears
client `+0x150` and returns send's result. Its wrapper (0xa) tail-calls an
import stub; the imported send implementation is still pending. Base pipe
`isTransactionComplete` (0x34) compares the queried ID against active/live
transaction IDs using signed comparisons, with no mapped hardware-stamp read.
It is a software queue-state predicate, not a GPU/DMA completion oracle.
These six complete bodies and explicit ownership/flag anchors are pinned;
paired-KC checks pass locally. No runtime/deployment/production changes.
# Resource preparation count and Intel completion override follow-up

Paired SystemKC resource vtable `header+16+0x170/+0x178` resolves to complete
`IOAccelResource2::prepare` (0x518 bytes) and `complete` (0x74), now pinned.
Prepare increments existing nonzero count `+0x28`; its first-prepare path
obtains backing `+0x40`, invokes backing/manager virtuals and accelerator
mapping helpers, increments the count after successful mapping and marks
flag `+0xe` bit 4. A flag-only path sets count to 1. Failure branches include
backing allocation, manager and accelerator operations, with an alternate
resource recovery/retry path. These callees/virtual identities remain pending;
this full body review does not certify their DMA operations or rollback.
No explicit counter-overflow guard or local lifetime mutex is present.

Base complete unconditionally decrements count, returning unless it becomes
zero. For resource type 1, zero cleanup releases `+0xf8`, calls backing helper
at `0x14bb79c2`, releases/clears `+0x40`, clears `+0x70/+0x20` and the prepared
flag. Type 3 instead invokes object `+0xe0` virtual `+0x1e8` and clears the
flag. The helper/virtual effects are not newly proven. Starting from zero
wraps this 32-bit count to `0xffffffff`; a repeated completion is therefore
not locally idempotent. This is a caller pairing obligation, not yet an
observed VF defect or justification to silently suppress cleanup.

Both Intel resource vtables import base prepare at relocation `0xd9550` but
override complete at object slot `+0x178` with `IGAccelResource::complete`
(0x6c bytes). If Intel flag `+0x240` is set, that override optionally calls
event-machine `+0x1b0` when count is nonzero, invokes object `+0x238` virtual
`+0x140`, clears the flag and tails to imported base-resource table header
`+0x188`. Import `0xc8138` establishes base-table identity; the object-vptr
and explicit table-header offsets must not be confused. Intel auxiliary
virtuals and backing cleanup remain pending. Native payload and paired-KC
checks pass; no production guard, hardware access or deployment was added.
# Finished-list pairing and backing inventory follow-up

Reviewed/pinned complete accelerator `accel_transaction_finished` (0x7e):
it validates/unlinks each transferred finished-list transaction, invokes
transaction `finish` then `complete`, then virtual release `+0x28`, looping
from the new head. Reviewed display finishTransactionQueue/releaseLiveTransaction
both call this helper after their gated finished-list extraction. This proves
the local unlink/finish/complete/release order, not absence of every other
caller or full concurrency safety. List corruption branches reach a trap.
The complete extraction wrapper (0x32) runs the already reviewed gated list
transfer through the command gate and does nothing when that gate is absent.

Complete `set_current_plane_resources` (0x27a) returns immediately for an
unchanged resource pair. Replacement merges old resource/backing events,
calls resource complete then release, clears old plane references, and
prepares each non-null new resource independently; only successful prepares
are stored/retained. Separate branches handle auxiliary plane objects.
No local terminal admission, plane-index bounds check or failed-prepare error
return appears here; callers, native virtual identities and partial replacement
semantics remain obligations. The complete body is pinned, not a proof that
the referenced event has actually completed on the GPU before cleanup.

Backing `IOAccelMemoryMap::remove_resource` (0x5e), called by resource complete,
searches the CPU pointer array `+0xa0`, decrements count `+0xb0` only when
found, and compacts subsequent entries. It has no hardware access, wait or
object release; missing entries leave the count unchanged. Consequently that
helper alone is not DMA unmap/drain. The subsequent backing release/destructor
and hardware lifetime need review. Paired-KC checks passed locally; no
production mutation, runtime test or deployment.
# Memory-map final-free delegation follow-up

Complete Intel `IGAccelMemoryMap::free` (0x12 bytes) only tails to imported
base memory-map vtable `+0xa0`. Both native payloads pin its effective free
slot, body and table import relocation `0xc8130`. Complete SystemKC base
`IOAccelMemoryMap::free` (0xa6) is the resolved base free vtable target.
It removes itself from parent memory `+0x18`, releases/clears that parent,
frees a non-null/non-embedded resource pointer array `+0xa0` using bounded
capacity-to-byte arithmetic, then delegates inherited free. The allocation
deallocator and inherited object-free callees remain separately pending.

Complete parent `IOAccelMemory::remove_mapping` (0x50) searches pointer array
`+0x50` with signed 16-bit count `+0x5a`, decrements count only for a match,
and compacts subsequent entries; missing matches do not change count. It is
CPU inventory maintenance, not hardware unmap or a completion wait. Neither
this helper nor the Intel final-free body establishes DMA quiescence. Parent
memory final free, earlier mapping complete/unmap operations, backing virtual
identity and concurrent ownership still require review. Native payload and
paired-KC targeted checks pass locally; no runtime or production mutation.
# Parent memory and inherited SysMemory free follow-up

Complete base `IOAccelMemory::free` (0x80) rejects non-null field `+0x20`
through a cold trap edge, frees mapping-array storage using signed capacity
`+0x58`, releases/clears object `+0x60`, and delegates inherited free.
Complete `IOAccelMemory::complete` (0xa) only decrements 32-bit count `+0x10`;
the already pinned map complete similarly decrements `+0xc`. Neither is
itself a hardware wait, and neither checks zero before decrement.

Complete `IOAccelSysMemory::free` (0x1d6) logs/releases retained fields
`+0xe8/+0x188`, conditionally calls virtual `+0x1b8` when byte `+0xc` bit 1
is set, handles another flagged allocation helper, releases retained objects
and descriptor-array entries, updates accelerator accounting and removes
collection membership before explicit base-memory free delegation. Helper,
descriptor and collection callees are not newly certified by the body hash.

Both native Intel SysMemory vtables import inherited SysMemory free via
external relocation `0xcd778`; effective object slot `+0x1b8` resolves to
`IGAccelSysMemory::unwire`. That body is the next concrete review boundary,
not yet a verified DMA/TLB barrier. This resolves a conditional cleanup edge
without claiming release or counter decrement proves quiescence. Paired-KC
and both native-payload targeted checks passed; no runtime/production writes.
# Intel/base unwire and release-PTE follow-up

Complete Intel SysMemory unwire (0x10e) first delegates imported base SysMemory
table header `+0x1c8`, then performs conditional tracing/accounting reads.
Native table import `0xc8140`, body and explicit delegation are pinned in
both payloads. External debug-call relocations are not local fallthrough
calls. Complete paired SystemKC base unwire (0x1f8) is now reviewed/pinned:
for DMA-command `+0x148` it calls virtual `+0x148` and `+0x130`, logs nonzero
statuses but continues, returns the command and clears the field. Those DMA
virtual identities/effects remain pending; logging is not fail-closed drain.

It temporarily adjusts parent prepare count while iterating the signed
mapping count, calls release_pte for mappings whose flag `+0x10` bit 2 is set,
then calls descriptor `+0xd0` virtual `+0x1f8` (status not tested in this
body), performs flag-selected virtuals/recovery checks, clears wired bit,
updates sysmem accounting and possibly invokes another purge-state virtual.
Descriptor and purge virtual effects/return contracts remain pending.

Complete release_pte (0x80) clears a resource state bit for every resource
listed in the mapping, conditionally invokes mapping virtual `+0x178` based
on mapping flags, clears its PTE-present flag, invokes parent virtual `+0x1d8`
and increments mapping generation `+0x110`. It is not independently a DMA
barrier until those virtual targets and backing lifetime are established.
Complete returnDMACommand (0xa6) conditionally pools/releases commands under
imported lock wrappers; complete sysmem_unwired (0x78) updates collections and
byte accounting through helpers. These helper callees/locks are not newly
certified. All four base bodies and caller edges are pinned. Targeted native
and paired-KC tests pass; no production or runtime operations were added.
# Inherited release-PTE status loss and native release-chain consolidation

Verification checkpoint: full `bash tools/check-static.sh` exited 0 in
`/tmp/ngreen-static.fxiqa6`; only the two existing SDK macro-redefinition
warnings were emitted. Proprietary paired-KC checks ran separately locally.

Re-reviewed complete Intel mapping `releaseFromGPUPageTable` (0x140) and
manager `releaseFromPageTableForTask` (0x112); both are now pinned in native
tests, including mapping virtual slot `+0x178` and direct manager call edge.
The mapping skips flagged special mappings as success; otherwise it forwards
to its accelerator memory manager and preserves that result. The manager
builds the address range through native getters, traverses task page-table
list `+0x268`, ANDs all releaseRange results, and returns success for an empty
list. Concrete list ownership and getter/page-table identities remain pending.

This consolidates the earlier “Synchronous GGTT unmap invalidation” review,
not a new discovery of releaseRange's deferred-flush behavior. Newly paired
with inherited SystemKC release_pte: after virtual `+0x178` returns, the caller
reloads flags without checking the bool result, clears PTE-present and proceeds
to parent notification/generation update. Therefore a false mapping/manager
return cannot protect backing through this caller. The production VF void
unmap already requires synchronous GuC invalidation or refuses unsafe cleanup;
its comment now records this additional inherited status-loss evidence.
No executable driver behavior changed. Guest panic is still not a DMA barrier
or substitute for the host containment plan. Targeted native/paired-KC tests
pass; no VM/PCI/deployment operations.
# BootKC DMA-command cleanup identity follow-up

Base BootKC IODMACommand object slots `+0x148/+0x130` resolve to
`complete(bool,bool)` and `clearMemoryDescriptor(bool)` respectively. The
reviewed SystemKC SysMemory unwire calls them with `(false,false)` and `false`.
Complete base bodies (0x230/0x90 including padding) and canonical Boot vtable
identities are now pinned. This does not prove runtime receivers cannot be
subclasses. General-memory-descriptor `+0x1f8` resolves to its complete method;
abstract IOMemoryDescriptor's corresponding slot is pure virtual, so concrete
descriptor identity must be established before certifying its effects.

DMA-command complete returns `0xe00002d8` for zero prepare count; otherwise
decrements `+0x68`, returns early for remaining references and on the final
reference handles synchronization flags, a mapping helper at `0xad4050`,
descriptor DMA-operation virtuals and mapping-record cleanup. The helper,
descriptor DMA-operation implementation and subordinate virtuals remain
pending. There is no explicit GPU retirement wait in these reviewed wrapper
instructions, though unreviewed callees may synchronize DMA mappings.

clearMemoryDescriptor(false) rejects a nonzero prepare count with the same
status; it does not force-drain references in this mode. True mode instead
repeatedly calls complete(true,true) until that count is zero. Once admitted,
it optionally sends a descriptor operation, releases the descriptor and
clears `+0x48`. The SysMemory unwire caller logs cleanup failures and continues
to return/clear its command field, as previously recorded. Do not infer
device-idle or DMA-safe backing release merely from command cleanup success.
Paired-KC checks pass after correcting a new fixture anchor offset from
`+0x4c` to the observed pointer-clear instruction `+0x7c`; no payload changed.
No production/runtime mutation.
# General descriptor completion and DMA walkAll backing-release follow-up

Reviewed/pinned complete BootKC `IOGeneralMemoryDescriptor::complete` (0x3a0)
and `IODMACommand::walkAll` (0x380), including padding/cold edges. The former
locks an optional descriptor mutex, handles type/flag/prepare-count branches,
decrements preparation count when backing exists and on final/flag-selected
cleanup invokes descriptor mapping helpers, UPL commit/abort and deallocation,
then clears backing state. Active-DMA count `+0x34` has a cold panic edge in
the final-release path, but its producer pairing and cross-device relevance
are not yet certified. Other types/flags can bypass or perform alternate
cleanup. Do not assume every concrete descriptor follows this base body.

DMA walkAll handles several flags for preparation, copy/synchronization and
release. The reviewed DMA complete invokes it with release bit `0x40` plus
its second bool. Its release branch calls vm_page_free_list for a saved list,
clears page count/list fields, releases the retained staging descriptor and
clears staging state. Mapping walk helper `0xad39c0`, callbacks, allocation
virtuals and descriptor read/write methods remain pending.

Canonical Boot symbols verify direct calls to `_upl_commit_range`,
`_upl_abort_range`, `_upl_deallocate` and `_vm_page_free_list`, now asserted
as explicit edges alongside body hashes. Their VM implementations are not
newly certified by caller review. These establish real backing-release paths,
not a GPU-completion oracle: safety still requires native GPU retirement and
translation invalidation before entering final release. Paired-KC targeted
tests pass; no runtime/production mutation or deployment.
# Descriptor DMA-operation count producer follow-up

Reviewed complete BootKC GeneralMemoryDescriptor dmaCommandOperation span
(0x8a0 including six-entry relative jump table). Canonical object slot +0x130,
body/table hashes, category targets and atomic count instructions are pinned.
Dispatch uses `(operation-0x01000000)>>24`; categories 1..6 have separate
size checks, segment queries, mapping allocation/reuse, limits and release.
Mapping helpers/VM callees are not certified by this body review.

Category 3 reaches `+0x155`: nonzero low 24 bits atomically adds one to the
16-bit descriptor count `+0x34`, clearing field `+0x38` on the 0-to-1 edge;
zero low bits checks nonzero then atomically decrements, otherwise reaches
a cold trap. The increment has no explicit saturation check. Both then flow
to the category-1 data-size/mapping query branch, so count side effects can
precede a later argument-size error. The reviewed DMA-command clear path sends
`0x03000000` with dataSize zero and ignores the descriptor operation result;
do not assume a returned error means no count transition occurred.

This explains the counter tested by descriptor complete's final-release trap:
it is maintained by descriptor DMA-operation protocol, not a directly observed
GPU execution counter. SetMemoryDescriptor producer, registration-success
flag pairing, concrete receiver identity and failure unwind remain pending.
The SDK header separately describes dmaCommandOperation as dedicated
communication for IODMACommand; it does not supply private operation semantics.
Paired-KC checks pass locally; no production/runtime mutation or deployment.
# DMA descriptor registration and forced-clear pairing follow-up

Complete BootKC IODMACommand setMemoryDescriptor span (0x1d0 including padding)
and base object slot +0x128 are reviewed/pinned. Replacing an existing different
descriptor rejects outstanding command prepare count `+0x68`; otherwise it
calls clear(true) before installing a new one. Query-operation failure returns
before retaining/installing the incoming descriptor. A same-descriptor call
with autoPrepare false repeatedly completes existing prepare references;
this branch is not a descriptor-registration replacement.

After a successful query, set retains/installs the descriptor at +0x48 and
sets registration flag in private state +0x7e according to command field
+0x40 being null. That mode sends `0x03000001` with dataSize zero and ignores
the operation return, pairing the already reviewed clear path's flagged
`0x03000000` decrement. General descriptor category-3 side effects precede
the size error, so this caller behavior is consistent with side-effect-based
registration; do not invent rollback solely on that ignored return status.

If autoPrepare is requested, virtual +0x140 is called; failure preserves its
error, calls clear(true) and returns the original failure. Clear's own return
is ignored here. Forced-clear mode drains prepare references, decrements the
descriptor registration when flagged, releases and clears the descriptor.
Local base pairing is now established; concrete runtime subclasses, prepare
callee failure behavior, count-overflow/concurrent mutation and Intel creator
options remain pending. This registration count is not GPU execution/retirement.
Paired-KC targeted checks pass; no production or runtime mutation.
# Base accelerator DMA-command factory/pool follow-up

Reviewed/pinned complete base createIODMACommand (0x40) and getDMACommand
(0xe0). Creation requires accelerator flag +0xcec bit 6. Its imported
withSpecification overload uses 64-bit addressing, mapping-options value 0,
zero max-transfer parameter, alignment 1, null mapper/refcon, and separately
loaded output callback/max-segment global values. The SDK defines mapped
mode/default system mapper; loaded globals and runtime effective options
still require provenance, so do not infer a VF bypass or a fixed max segment.

System stub 0x10072 and chained cell 0x24098 resolve to BootKC's exact
withSpecification overload. Its complete 0x90 body allocates through a
metaclass virtual, invokes initWithSpecification +0x120 with forwarded
arguments, releases on failed initialization and returns null. The metaclass
allocator/init body is not newly certified here. The other SegmentOptions
overload was inspected but is not this imported path or newly test-pinned.

getDMACommand requires the same feature flag, takes an imported lock wrapper,
removes/validates a pooled command and updates count, or obtains one from
command pool +0xa10 virtual +0x118 when the list is empty. Missing pool yields
null. Concrete Intel accelerator factory override, pool construction,
allocation virtual and command receiver identity remain pending; these base
bodies alone do not establish which receiver is used at runtime. Targeted
paired-KC checks pass; no runtime/production mutation.
# Intel inherited DMA factory and template-clone pool follow-up

Both Intel accelerator vtables import base createIODMACommand at relocation
`0xd1b20`, effective object slot +0xab0. Complete base createDMACommandPool
(0x1d4) calls that slot after allocating lock +0xa18, stores the returned
command at +0xa10, retains/inserts it into a validated circular list, sets
count +0xda0 to 1, then clones further commands up to configured count +0xd1c.
Clone failure skips that insertion rather than failing the whole creation.
Thus +0xa10 is a template IODMACommand, not a separate IOCommandPool object;
Boot base slot +0x118 resolves to cloneCommand(void*). Earlier generic “pool”
wording denotes the overall accelerator pool, not this concrete receiver.
Clone body and factory metaclass allocation remain pending.

Complete releaseDMACommandPool (0xf0) validates/unlinks/releases list entries,
zeros pooled count, directly dereferences/releases template +0xa10 and clears
it, then frees/clears lock +0xa18. It has no template-null check in that branch
and no local acquisition of the pool lock. Create failure paths can leave
template null/lock allocated, and concurrency with get/return is not excluded
by this body alone. Outer failed-start cleanup selection and shutdown caller
serialization must be established before alleging an observed defect or
adding a guard/lock. Locking after normal get/return admission remains open
would not by itself close lifetime races. Hashes/clone identity/native factory
import and explicit failure-cleanup dereference anchors are pinned; targeted
native/paired-KC checks pass. No production/runtime changes.
# DMA-pool failed-start reaches inherited stop follow-up

Direct byte candidates were checked against decoded selected instruction
windows (not an exhaustive indirect-caller scan): accelerator start calls
createDMACommandPool at 0x14b9ff30 under feature bit +0xcec bit 6; false takes
0x14ba07b5, records failure and joins 0x14ba07fd. After notification/unlock,
the false branch goes to 0x14b9fbc3 and directly calls base accelerator stop
at 0x14b9fbed. The previously reviewed complete stop calls releaseDMACommandPool
at 0x14ba1e36 under the same feature bit, with no template-presence test at
that callsite. Selected complete windows and these explicit edges are pinned;
the entire accelerator start span is NOT newly reviewed.

Combined with reviewed pool creation order: lock/factory failure can return
false before circular-list initialization and leave template +0xa10 null.
The failed-start route nevertheless selects feature-gated pool cleanup, which
assumes initialized list/template. This is a concrete static failure-unwind
hazard under such allocation failures, not a dynamic reproduction or proof
that it caused the historical VF panic/Host i915 hang. Earlier stop callees
may fail first and remain relevant; no claim of first crash instruction is
made. Successful shutdown also needs external get/return admission closure.

A production repair must establish per-object VF ownership and partial-init
state or patch the full paired cleanup contract, not blanket-hook shared
IOAccel methods using only global VF identity. The current shared-IOAccel
routes do not provide that owner registry. Do not add a lock around this
cleanup without checking gate/mutex order and late producer admission.
Targeted paired-KC checks pass; no executable patch/deployment/runtime test.

# DMA template clone allocation follow-up

Reviewed the complete Boot IODMACommand::cloneCommand(void*) body at
0xffffff8000ad24b0 (0xe0 bytes including trailing alignment), now hash-pinned.
It constructs a zero-initialized 0x28-byte SegmentOptions record from the
template's address width, maximum sizes and three stored alignment-minus-one
fields (incremented when copied). It forwards the stored output callback,
mapping options and mapper, but uses the caller's new refcon. This body does
not copy the template's installed memory descriptor or prepared reference count.

Allocation uses a metaclass virtual +0x88; null allocation returns null.
The newly allocated object's virtual +0x180 initializer receives the options.
False initialization releases that new object through +0x28 and returns null;
success returns it. Initializer/metaclass implementation identity and their
internal partial-failure cleanup remain pending; this selected body alone
does not certify them or prove mapper/descriptor runtime ownership.

Together with pool creation's reviewed clone-null skip, clone allocation
failure itself does not select the template-null failed-start edge. Original
template factory/lock failure remains the distinct unsafe partial-pool unwind.
Per-object ownership still must be established before a shared IOAccel hook:
current Intel start/stop routes classify through global VF identity, not a
registry of System-kext accelerator owners. No production patch or VM test.

# DMA clone initializer and mapper ownership follow-up

Resolved base object slots +0x178/+0x180 to initWithRefCon (0x50) and the
SegmentOptions initWithSpecification (0x60); both complete bodies and delegated
setSpecification (0x290) are now pinned. The wrapper calls initWithRefCon,
then converts setSpecification's zero return to true. initWithRefCon initializes
the circular link and refcon, allocates private state +0x70 if absent and
returns true without a local allocation-null test. The allocator is
kalloc_type_impl with flags including literal 4, not an ordinary nullable
IOMalloc conclusion: allocator flags/failure policy must be reviewed before
alleging a recoverable null-allocation bug.

setSpecification rejects null callback/options and checks address width with
special cases for the built-in 32-bit output callbacks. Zero maximum sizes
become all-ones limits; alignments are stored as minus-one with zero defaults.
An explicitly supplied mapper is checked via its metaclass ancestry. Mapped
mode may resolve/wait for the default system mapper; alternate mapping modes
select different mapper handling. Changed effective mapper +0x40 is retained,
and the previous mapper released; the separately stored mapper provenance
at private +0xb8 is not itself retained by this body. Optional mutex creation
stores its result at private +0xc0 without a local null-result test.

These bodies establish local clone initialization/mapper reference operations,
not mapper runtime validity, allocator failure semantics, full free/unwind or
GPU retirement. Mapper wait, allocator and mutex implementations remain
pending. No speculative null-allocation fix, executable patch or dynamic test.

# DMA private allocation and free contract follow-up

Reviewed/pinned complete Boot kalloc_type_impl/external alias body (0x90).
It masks supplied flags with 7 before forwarding to zone/heap allocators;
thus the conditional 0x8000 from initWithRefCon does NOT survive this external
KPI wrapper. Literal 4 means Z_ZERO, not Z_NOFAIL. Local XNU reference
12377.121.6 (git ac9718fb1af618d5ce8678d0dc6e8a58f252216f) defines WAITOK=0,
ZERO=4 and NOFAIL=0x8000, and explains WAITOK's nonfailure guarantee only for
non-exhaustible zones. The actual allocation-view zone/exhaustibility and
downstream heap/zone failure policy remain unresolved, so null failure is not
yet proven possible on this private-state allocation. Do not add a speculative
null-check patch solely from a source-version analogy.

Reviewed/pinned complete base IODMACommand::free (0xd0) and its effective
vtable slot +0x90. It tests private state, conditionally handles/frees the
optional mutex, frees/clears private +0x70, releases/clears mapper +0x40,
then simply zeros descriptor +0x48. That last operation is NOT a call to
clearMemoryDescriptor, nor registration decrement, descriptor release or
prepare-reference drain. The matching local XNU source documents an intentional
descriptor-detach leak workaround for callers missing clearMemoryDescriptor.
Hence release of a command, including pool cleanup, cannot replace the required
complete/clear protocol established earlier. The optional mutex branch calls
CompleteDMA only if active; its call and allocator/free callees still require
their own review. No claim that command destruction proves DMA quiescence.

This closes the local no-mutex failed-clone release ordering: initialized
private state and any retained mapper are cleaned by the base free path.
Concrete runtime subclasses, descriptor-bearing pool return/release discipline,
allocator semantics and Dext-lock behavior remain pending. No runtime mutation.

# DMA command return admission and cleanup-status follow-up

Re-reviewed complete returnDMACommand (0xa6, already body-pinned) in context
of command free/clear. A null command or disabled feature returns immediately;
otherwise it takes pool lock +0xa18, then either inserts the command into the
circular list/increments count, or releases it under the lock when capacity
is reached. It performs no descriptor/prepare-state validation, complete,
clear, stopping-state check or lock-null check. Release occurs before unlock,
so adding a destructor/drain wrapper here also requires checking lock order.

The direct-byte scan of this embedded accelerator address range found one
candidate to this body, decoded as sys-memory unwire's call at 0x14bba30d.
This is not an exhaustive indirect/imported caller inventory. That caller
invokes complete(false,false) once, then clear(false); each nonzero status is
logged but still joins the unconditional return-to-pool call. It clears its
command field afterwards. Additional selected argument/error-join and pool
lock/capacity/release anchors are pinned.

For a base command entering unwire with prepare count greater than one,
complete only decrements it, and clear(false) rejects the remaining count.
This conditional case therefore leaves descriptor/prepared state intact but
still returns/releases the command. Actual reachability of that initial count
through wire/prepare callers is not yet established; do not label it the
observed VF crash or repair it by silently draining without GPU-retirement
proof. Likewise normal successful clear does not establish shutdown admission
closure: a late return can still select a freed pool lock. The repair must
address owner lifetime/admission and status propagation together, not just
add a list lock. No production/runtime changes.

# Sys-memory wire and failed prepare reference follow-up

Reviewed/pinned complete base sys-memory wire (0x34c) and Boot base
IODMACommand prepare (0x650). Wire prepares its descriptor, gets/stores a
command at +0x148, binds with autoPrepare=false, then calls prepare(0,0,false,
false) once. Ordinary clean-command success therefore acquires one prepare
reference, paired with unwire's single complete; this local normal path does
not itself prove a count greater than one. Wire has no local wired-bit check
before getting/overwriting the command; caller wire-count/admission remains
pending. A null command is accepted by the local success branch.

The more immediate failure issue is prepare's ordering: after initial
specification/max-length checks, it increments +0x68 BEFORE fallible alignment,
walk or mapper work, with no decrement in this complete body on those error
returns. Nested mismatched-range requests also increment before returning an
error. Wire's prepare-error branch calls clear(false), ignores its status,
then calls descriptor complete(0) and returns false; it does NOT first complete
the command or return/clear its stored command field. The wired bit is only
set on success. Thus for a post-increment prepare error, clear(false) rejects
the retained reference and leaves descriptor registration installed; immediate
descriptor completion is not a proven valid unwind. The base/general descriptor
active-registration panic checked earlier may then apply, depending on actual
descriptor class/count. This is a conditional static failure protocol, not a
reproduction or attribution of the historical panic/Host hang.

New command binding failure similarly completes the descriptor without locally
returning/clearing the stored command. Outer failed-wire disposal and effective
Intel wire override still require review. Do not repair failed prepare by
forced descriptor release: first establish command cleanup/status semantics,
concrete owner, and whether any mapping/backing can still be referenced by GPU.
Selected arguments, failure-clear, descriptor-complete and success-bit anchors
are pinned; no production patch, deployment or VM test.

# Intel wire override and outer prepare-count follow-up

Reviewed/pinned complete Intel IGAccelSysMemory::wire (0x10a) in both archived
payloads. Its effective object slot +0x1b0 selects that override; it loads the
already relocation-pinned base sys-memory vtable import at 0xc8140 and calls
header-relative +0x1c0. The paired System table resolves that slot to base
sys-memory wire, now explicitly pinned. Intel saves the result, optionally
emits tracing, and returns the saved result; it supplies no failed-wire cleanup.
External tracing call relocation placeholders are not evidence of self-calls.

Reviewed/pinned complete base IOAccelMemory::prepare (0x3c): if wired bit +0xc
bit 1 is absent, call virtual +0x1b0; false returns false without incrementing
memory prepare count +0x10. Success/already-wired increments that count and
stores accelerator generation. Consequently the count gate suppresses normal
repeated wire calls for that entrypoint, but does not clean a failed wire's
stored command +0x148. Complete's previously reviewed count decrement is not
a command complete call. Actual higher-level failed-prepare disposal remains
pending; this is one concrete entrypoint, not all virtual slot users.

Also reviewed/pinned wire-count helpers: increment only increments +0x14;
decrement first decrements it and, on zero with a parent-count helper returning
zero, dispatches virtual +0x1b8 unwire. There is no local count-underflow guard
or failed-wire cleanup branch. Parent-count helper and callers still require
review. Wire count, memory prepare count and command prepare count are distinct
fields/protocols; none is a substitute for GPU retirement. No production/runtime
mutation or broad per-owner-hook safety claim.

# Sys-memory factory distinguishes prewired pool ownership

Reviewed/pinned complete legacy withOptions wrapper (0x14) and bool-overload
implementation (0x4ce). The wrapper forwards to that implementation. The latter
builds/obtains a descriptor through pool, segmented-large-allocation or ordinary
allocator paths, creates sys-memory via accelerator virtual +0x8b8, and stores
descriptor +0xd0 and resource/task/size metadata. Allocation failure releases
already-created segmented descriptors and their array; failed sys-memory
creation releases the descriptor. Allocator/pool helper callees are not newly
certified by these bodies.

The successful pool-backed path performs an event-machine virtual +0x1b0 on
the event-machine receiver (NOT sys-memory wire), stores pool provenance,
sets flags including wired bit 1 and pool-mode 0x2000, zeros wire count +0x14,
and calls sysmem_wired/parent accounting without calling ordinary wire or
locally constructing a command. These flag/count anchors are now pinned.
Hence a null command +0x148 with wired=true is not by itself proof of a broken
wire. Actual pool preparation/retirement requires its helper protocol; reject
any proposed blanket command-presence check until this branch is accounted for.

This factory is not the missing cleanup after ordinary wire returns false;
it does not call the ordinary wire path. Higher-level failed-prepare disposal
remains pending. Byte patterns for virtual +0x1b0 alone cannot identify wire
callers: receiver provenance and vtable type must be verified. No production
patch or runtime/Host GPU mutation.

# Mapping parent prepare failure has no local command cleanup

Reviewed/pinned complete IOAccelMemoryMap::prepare (0x7c) and its outlined
.cold.1 helper (0x62); this helper is normal fallible control flow, not a panic
merely because its symbol contains "cold". Intel memory-map effective slot
+0x138 imports base prepare at relocation 0xcd040 (both payloads now checked).
The paired base table and sys-memory slots +0x148/+0x150 resolve to previously
reviewed parent memory prepare and newly reviewed sys-memory complete override,
not command prepare/complete. The first fixture attempt incorrectly expected
base memory complete at the sys-memory slot; actual-table inspection resolved
the override and corrected that expectation before passing the checks.

For a map with zero local count and no installed-PTE flag, the helper calls
parent +0x148. False immediately selects its false-output return, with no
parent complete, command complete/clear or command-field disposal. If parent
prepare succeeded, it attempts PTE initialization; either success or failure
balances parent memory prepare through +0x150, but only successful PTE setup
increments mapping prepare count. PTE initializer is not newly reviewed here.
Thus failed wire propagated via parent prepare is not repaired at this layer.
Resource prepare's reviewed mapping +0x138 false branch can enter the separate
accelerator recovery helper; that helper/caller disposal still must be traced.

Also reviewed/pinned complete parent getPrepareCount (0x48) and mapping
getPrepareCount (0x32). Mapping aggregates its own count with one unit per
resource having nonzero prepare count; parent aggregates its own count with
one unit per mapping having nonzero aggregate count. These are admission/
accounting summaries, not command prepare references or GPU completion.
They add without local overflow checks. Existing wire-count decrement therefore
tests this aggregate, not command +0x68. No executable/runtime changes.

The complete sys-memory complete override (0x42) explicitly calls base memory
complete via its table-header +0x160, then tests wire count +0x14 and aggregate
getPrepareCount. If both are zero, it dispatches virtual +0x1b8 unwire. Thus
successful parent-prepare balancing can indirectly reach command cleanup; it
is not merely a bare memory decrement. The failed parent-prepare branch above
does not invoke this override, so that local failure conclusion remains valid.
The full override, paired slot, explicit base table and dispatch are pinned.

# VF post-write GGTT completion must not return into native cleanup

Reviewed/pinned complete Intel manager commitIntoPageTableForTask (0x11a).
It iterates the task list +0x268, calls address-space commitRange and ANDs
results, continuing after false. An empty list returns true. It supplies no
rollback of earlier successful entries. commitRange and cross-entry rollback
remain pending; this manager result is not an all-or-nothing mapping guarantee.

Combined with reviewed base commit_pte/prepare unwind: false commit does not
publish installed-PTE flag, yet parent completion can reach unwire. The existing
VF mapRange/dummy/rotated finalization wrote direct PTEs then returned the result
of TLB invalidation; false could therefore enter native cleanup that skips PTE
release. Rotated second-pass rollback also ignored failed invalidation before
releasing its retained descriptor. These are concrete production control-flow
gaps, not evidence of the historical crash's exact cause.

Production repair: a shared VF-only post-write completion helper panics rather
than returning if invalidation is unconfirmed. Normal/dummy mappings use it
before true; rotated rollback and successful mapping use it BEFORE releasing
the retained descriptor. Pre-write validation may still return false because
no PTE stores occurred. Four source mutations check guard inversion, bypass,
rollback release-before-barrier and successful-map release-before-barrier.
Native full-body and source contracts do not simulate actual DMA.

This prevents the specific post-write synchronization failure from returning
into premature backing cleanup; it does NOT quiesce Host DMA, solve earlier
successful segment/address-space commits followed by later preflight failure,
prove callback admission closure, or repair sys-memory failed prepare. Guest
panic is explicitly not a Host containment barrier. No deployment or VM test;
runtime hold remains in force until all independent hazards/gates are resolved.

# Native segment commit preserves a successful prefix on later failure

Reviewed/pinned complete IGHardwarePageTable::commitRange (0x41c). Manager's
direct 0xf639 call reaches this body. Global-page-table declared virtual slots
+0x118/+0x120/+0x138 resolve to the routed ordinary/rotated/dummy mapping
methods, now checked in both payloads. This fixes the native receiver/dispatch
graph without asserting all task-list members are global page tables.

Ordinary commit retains descriptor iterators, rounds lengths and maps segments
sequentially. A low-level map false branches out with result false; there is
no unmap of the earlier successfully written prefix. After releasing iterator
references, it attempts dummy mapping only for the suffix beginning at the
current GPU cursor. That suffix call's result is ignored before deferred flush
notification and return of the earlier result. Native arithmetic/descriptor
iteration lacks a whole-operation preflight in this body. The rotated branch
delegates to the routed rotated mapper and releases its iterator separately.

The concrete conditional failed-commit chain is therefore: one segment
succeeds, a later segment fails, only suffix cleanup is attempted, manager
returns false, base commit_pte does not set installed flag, and parent complete
can enter unwire whose release_pte loop tests that missing flag. A successful
prefix is not automatically rolled back. No dynamic failure injection or
historical crash attribution is claimed. The prior post-write invalidation
repair covers failure within a single low-level operation, NOT later preflight
failure or another address-space commit after an earlier successful operation.

Repair must provide a per-owner full-operation mapping transaction, preserve
descriptor/backing across every failure, and confirm rollback invalidation
before native cleanup can proceed. Global counters or blanket shared hooks
would not establish ownership/concurrency; silent false or fabricated success
would preserve the hazard. No new production patch/runtime test this checkpoint;
Host containment hold remains active.

# Task address-space list can contain both PPGTT and GGTT

Reviewed/pinned complete initManagedPageTableList (0xfc) and
newPageTableForTask (0xa6) in both native payloads. An exploratory disassembly
through 0x7cdc included the separate following initStampAndScratchPages body;
actual next-symbol boundary 0x7bea was checked before pinning the list function.
No combined-span claim is made.

When accelerator feature +0x1191 bit 0 (PPGTT) is enabled, list construction
allocates a node for task per-process page table +0x260. It additionally adds
manager's global table +0x98 when task context/classification conditions permit;
kernel/bootstrap task conditions can therefore yield BOTH per-process and
global nodes. Later non-kernel task conditions suppress the global node when
PPGTT is enabled. With PPGTT disabled, the examined list path adds global only.
Nodes link through tail +0x270; allocation failure frees the first node before
returning false. The list stores raw table pointers without local retain.
Constructor ordering/table pointer validity and ownership release still need
their own proof.

newPageTableForTask selects 32-bit or 64-bit per-process factories by hardware
context address mode, checks factory null, then synchronizes kernel-task table
from global or another task from accelerator kernel-task per-process table.
Those synchronization return paths are void at these callsites; internal
allocation/mapping/failure semantics remain pending. Factory/synchronization
direct identities are pinned, not their full callees.

This supplies concrete evidence against a GGTT-only rollback being a complete
manager-commit repair. The VF bootstrap classification wrapper changes which
source is used for the first task; it does NOT replace its per-process object
with the global object. Do not disable PPGTT or fabricate all-task kernel
classification to avoid the multi-address-space lifetime problem. Full manager
transaction, per-process invalidation and descriptor retention remain required.
No production/runtime/Host GPU mutation this checkpoint.

# Page-table synchronization failure visibility and borrowed list release

Reviewed/pinned six complete native bodies: global synchronize wrapper (0xa),
per-process synchronize wrapper (0x42), synchronizeEachEntry (0xd0),
synchronizePageDescriptor (0x5c), task managed-list release (0x6e) and global
read (0x44). Wrapper/helper edges and global read virtual are pinned in both
payloads. Global synchronization delegates to per-entry reads/maps; per-process
synchronization chooses the descriptor path only when both table +0x28 bit 0
flags are set, otherwise delegates to the same per-entry path.

Per-entry synchronization reads source virtual +0x140, skips absent entries,
then maps/remaps destination (+0x118/+0x128 selected by the boolean). False
mapping exits the loop without rolling back previous entries or returning an
error status. It only issues the previously reviewed deferred-flush notification
before returning void. The constructor selector consequently returns its
nonnull page table even if a synchronization helper stopped early; an actual
failure has not been dynamically reproduced.

The descriptor path replaces the requested range with [0, 0x40000000), reads
source descriptor virtual +0x168, then invokes map/remap descriptor virtual
+0x158/+0x160. Destination status is not consumed. Actual table feature flags,
descriptor ownership and compatibility of that fixed range with VF allocations
must be established before reuse is certified or rewritten. It is not evidence
that the user's current VF address range is necessarily outside that window.

Global read assembles two 32-bit PTE loads, exposes low 12 attribute bits and
masks physical addresses to 0x7ffffff000 (legacy 39-bit range); it does not
locally lock or bounds-check. Current VF physical-range checks already limit
native-address compatibility, but whole sync receiver bounds/concurrency and
Gen12 attribute interpretation remain pending. Do not widen addresses without
fixing every consumer of this getter.

Managed-list release validates/unlinks/frees each 0x18-byte node. It does not
release referenced table objects, revoke mappings or drain GPU users. That
matches raw pointer storage locally, but table ownership/free ordering and
callback admission must be proved elsewhere. No executable patch/runtime test.

# Descriptor synchronization option has a concrete 64-bit constructor source

## Base ownership and 64-bit descriptor operations

### PagePool reuse and expansion/shrink bodies

#### Reallocation is not gated by prune age

##### Queue publication failure is a real local allocator branch

###### Pool initialization/free and hash removal

####### Standard pool owner path is non-threaded

######## Offline production repair: bounded initialized-prefix unwind

######### Factory failed-init release cannot blindly use pool free

Allocator scope checkpoint: target 0xffffff80003d4230 has no defined function
symbol in the selected kernel symbol table. The next named-symbol span is
0x4200 and mixes several routines; exploratory disassembly output was truncated.
It is NOT a complete function review and no full-span safety pin is added.
Reference kalloc.c kalloc_heap_init invokes kalloc_zone_init with ZC_NONE, which
passes creation flags to zone_create_ext; source zalloc_ext also checks execution
context and NOFAIL compatibility. Actual selected zone/initialization/policy
remain unresolved; source defaults are not current runtime configuration proof.

Priority correction from established path evidence: standard manager pools use
options 0 and the pinned base OSObject init returns true. They do not reach the
threaded lock-allocation false branch. Thus factory failed-init release repair
would address conditional threaded/other admission, not a demonstrated normal
manager failure. Allocation-null faults can precede that return path and remain
open. Keep the tested empty-state predicate staged, not a live-pool destructor.
Do not delay the critical PPGTT/GGTT transaction/retirement audit behind an
unbounded kernel allocator review. Next work returns to complete mapping/unmap
callees and cross-owner rollback; Host containment hold remains unchanged.
No production/hardware mutation, no claim the allocator is safe or complete.

Reviewed Boot kalloc_ext root plus separate unnamed large-allocation helper
region (named-symbol span 0x370) and pinned region hash, OSObject new's exact
direct allocator edge and root null-propagation branch. The ordinary size-class
path calls allocator at 0xffffff80003d4230; if RAX is zero it branches to XOR EAX
and returns zero, which OSObject new propagates unchanged. Region identity is
not mislabeled as one complete function; lower zone/VM/diagnostic callees remain
pending.

Local XNU zalloc.h decodes 0x41004 as tag bits, backtrace tagging and ZERO with
WAITOK; it does not include Z_NOFAIL (0x8000). Its documented WAITOK guarantee
depends on non-exhaustible zone policy; exhaustible zones may fail at their
limit. Therefore root null propagation plus absent NOFAIL is NOT proof that this
specific pool's allocation can return null. The selected size-class zone,
initialization/exhaustion flags and actual allocator policy must be resolved.
No memory pressure/OOM test was run. Paired KC/diff-check pass; aa0439c
CI37165903219 success. Fixture/docs-only, no full-suite rerun or runtime mutation.

Complete Boot OSObject operator new (0x30) reviewed/pinned with next-symbol
boundary: passes size and flags 0x41004 to allocator 0xffffff8000369360, updates
global ivar-size accounting and returns the allocator result without a local
null check/assert/panic. Native pool metaclass allocator therefore depends on
that underlying allocator's failure semantics before dereferencing the result.
Reference XNU OSObject.cpp contains assert(mem), but it is not present in this
pinned binary; source assertion is not a runtime nonnull guarantee. Allocation
flags and actual callee must be reviewed before alleging recoverable null return
or assuming allocation failure cannot happen. No failure was induced.
Paired KC contract and diff-check pass; 9839ed1 CI37165811210 remains in progress.
Fixture/docs-only; no full-suite rerun, hook, deployment or Host GPU operation.

Complete native pool metaclass alloc (0x40)/explicit-meta constructor (0x20),
and Boot OSObjectC2 (0x20)/instanceConstructed (0x30) reviewed/pinned. Native
alloc requests 0x78 through OSObject new, calls base constructor, installs pool
vtable, increments class instance bookkeeping and returns that object. Base
constructor installs OSObject vtable and initializes reference count to 1;
instanceConstructed atomically increments class count and superclass only on
the first instance, pairing the reviewed destruction bookkeeping. No pool queue,
callback or GPU backing is published by those constructor bodies.

Native metaclass alloc does NOT locally check OSObject new's returned pointer
before constructing/installing vtable. Its allocator's failure policy remains
pending; the downstream withOptions null check alone does not establish safe
allocation failure handling. Declared metaclass alloc/effective runtime vptr
initialization and all indirect factory admission must be established separately.
Future failed-init cleanup must verify reference count 1, matching borrowed
owner and fully empty resource predicate at factory-exclusive entry/return,
not infer exclusivity from constructor history or emptiness alone. No hook is
installed by this audit checkpoint. Both native/paired KC targeted checks and
diff-check pass; 53a2dc2 CI37165604548 success. Fixture/docs-only; last full
suite remains /tmp/ngreen-static.gUoMsZ. No runtime/Host GPU changes.

Implemented `NGVfPagePoolPatch::failedFactoryStateIsEmpty` as an explicit
resource-state predicate for future failed-construction cleanup. It requires
nonzero object/borrowed accelerator, zero available pages/page-ID count, null
queue/free-list head/interrupt/timer/lock, cleared scheduled flag, and the empty
free-list tail pointing to object+0x30 with checked pointer arithmetic. It does
not mutate fields or release anything, and is NOT integrated into a hook yet.
It intentionally does not claim factory-exclusive ownership, reference-count
validity, absence of escapes or GPU quiescence; those must be established at
the actual entrypoint before base deletion can be authorized.

ASan/UBSan tests for both payloads cover the valid empty state, all 31 nonempty
combinations of five owned pointer fields and seven invalid scalar/list/owner/
overflow states (39 cases), alongside existing prefix-unwind mutation tests and
4097 index cases. No new failed-init release behavior is enabled by this helper.

Full tools/check-static.sh exit0, diagnostics /tmp/ngreen-static.gUoMsZ (two
existing TargetConditionals macro warnings), including the new cases for both
payloads. Diff-check passes; 981abe4 CI37165426168 success. No deployment,
VM start, PCI/Host GPU operation or hardware acceleration proof.

Complete Boot KC OSObjectD2 (0x10) and OSMetaClass::instanceDestructed (0x90)
reviewed/pinned with next-symbol boundaries. D2 overwrites vptr with -1 and
returns; no derived pool cleanup or second free dispatch. instanceDestructed
atomically decrements class instance count, recursively updates superclass only
on zero, and emits an error path if count is negative. It is class bookkeeping,
not pool ownership, descriptor or GPU retirement accounting. Base free's
getMetaClass virtual and this direct call must preserve valid class construction
counts before deletion; they do not justify deleting an escaped live object.

Exploratory nearest-symbol span for OSObject free was 0x280, but its reviewed
wrapper is only the existing 0x30 window ending at an unnamed subsequent body.
Do NOT claim the entire 0x280 as one free method. Sized delete's reviewed 0x40
wrapper calls kfree_ext at 0xffffff8000369a30 with object pointer/size; its nearest
0x340 span also includes unnamed internal helpers/multiple return paths. Only
exploratory decoding was performed there, with truncated output; allocator
callee coverage remains incomplete and no complete-body pin is added for it.
This correct boundary accounting prevents a synthetic all-callee safety claim.
No production patch or runtime action in this checkpoint.

Complete pool D2/D1/D0 bodies (0xa/0xa/0x22) reviewed/pinned with external
destructor/delete relocations and effective vtable targets. D1/D2 tail-call
OSObjectD2 through relocation placeholders; they do NOT call the next local
function despite the zero displacement seen before linking. D0 calls OSObjectD2
then sized OSObject delete with 0x78. Pool virtual free is separately at +0x90.
The deleting destructor contains no queue/lock/backing cleanup.

This narrows a potential failed-init leak repair: after an init failure, bypassing
pool free via already resolved base OSObject free could delete the instance
without the null-lock path ONLY when every owned queue/source/lock/backing field
is demonstrably cleared and no object escaped. Such a repair needs explicit
field-state preflight and the base destructor/delete callees' full review.
It is not safe for a normal/live pool and has not been implemented here. The
native factory's sole decoded init caller does not prove all indirect admission.
Both native payload contracts/imports/vtable checks and diff-check pass;
9b101b4 CI37165122377 success. Fixture/docs-only, no full-suite rerun and no
production/runtime/Host GPU mutation in this checkpoint.

Reviewed/pinned complete Tahoe Boot KC IOSimpleLockFree (0x50), lck_spin_free
(0x50), lck_spin_destroy (0x30) and OSObject init (0x10), including exact
next-symbol boundaries and effective OSObject init virtual. All three lock
cleanup bodies dereference the lock before any null guard. OSObject init simply
returns true here; the native pool's base-init failure branch is not a currently
reachable allocator-failure substitute in this pinned base implementation.

Local XNU ac9718fb1af618d5ce8678d0dc6e8a58f252216f independently agrees:
iokit/Kernel/IOLocks.cpp IOSimpleLockFree delegates lck_spin_free;
osfmk/i386/locks_i386.c spin destroy reads/writes the lock; OSObject::init
returns true. Actual Boot KC code, not source version resemblance, is the
authoritative runtime ABI evidence.

Threaded pool lock allocation failure clears queue and leaves +0x68 null while
+0x64 remains true. Adding a factory release would enter pool free's unconditional
IOSimpleLockFree(+0x68), invalid for this failed-init state. Thus failed-init
leak repair must first make cleanup partial-state-safe or use narrowly validated
base-object destruction after all owned state is cleared. Do not simply insert
virtual release on every init false. Standard manager options=0 pools avoid
this threaded branch; their failed-init factory can still have other obligations
in a redesigned allocator. No production mutation in this checkpoint.
Paired local KC contract checks and diff-check pass. 32cd0db CI37164951303
success; full suite last passed for that production repair, not rerun for these
KC-only fixture/document additions. No VM, kext deployment or Host GPU changes.

Added UUID-pinned VF native patch for initPagePool's partial factory failure.
Its symbol bounds must be exactly 0xcc and the complete 49-byte unwind window
must match uniquely at +0x6b before mutation. Patch changes only five bytes:
initial TEST/JE becomes DEC/JS, final INC/JNE becomes DEC/JNS. RBX starts as
the failed index N, so the first access is N-1 and traversal includes zero;
N=0 exits before any access. Native pool release, slot clearing, array free/
pointer clearing and false return are preserved. Normal successful initialization
is unchanged. The patch lives under existing VF classification, not PF behavior.

The pools are freshly initialized without page allocations or registered sources
on this path, so this is failed construction cleanup, not retirement of mapped
GPU backing. It does not fix factory failed-init leaks, unchecked queue/map
allocations, rehash atomicity, postwrite mapping transactions or ordinary teardown.
Do not reuse this reverse-prefix logic for live GPU resources without retirement.

Offline tests verify both payload anchors, exact branch destinations, unchanged
inner release/clear body and all bytes outside the five changed positions,
49 single-byte anchor mutations, duplicate/missing/changed-length/null/already
patched rejection, and 4097 decoded index-control cases (prefix lengths 0..4096).
Those cases model index control only, not callbacks/DMA. Full check-static exit0
at /tmp/ngreen-static.rD6AqK (two existing TargetConditionals warnings); after
adding extra duplicate/outside-byte checks the ASan/UBSan native patch test was
rebuilt and passed again for both payloads. Offline disassembly confirms JS
0xee3a and JNS 0xee0e. Diff-check passes; 401009d CI37164723083 success.
No kext build/deployment, VM start, PCI mutation or hardware safety claim.

Further complete manager init (0x232), releaseDeviceMemory (0x46) and
releasePagePool (0x92) reviewed/pinned. releaseDeviceMemory only clears +0x18
and releases/clears +0x68/+0x70 objects; it does NOT touch pool array +0x110.
releasePagePool separately releases each slot across real_ncpus, clears it,
frees the array and clears +0x110. That helper assumes every slot is either
initialized or null and the CPU count still equals the allocation count.

Manager init clears +0x110 before constructing device resources, dummy pages,
global page table, stolen/fence objects and pools. Its failure branch releases
device/global/stolen/fence/dummy resources then releasePagePool. But initPagePool
already contains the erroneous partial-failure loop, so the outer helper cannot
be relied on to recover from that loop. The pool array allocation is not zeroed
locally; a partially populated array cannot safely be passed to the full-count
release helper without first initializing all slots or passing prefix length.

Native __text decoded direct edges (external placeholders excluded) show only
initPagePool calling pool withOptions (options 0); registerEvents is called by
IntelAccelerator start at 0x24145; releasePagePool's direct caller is manager
init's failure branch at 0xe7a2. Reviewed manager free does not call it, directly
or via releaseDeviceMemory. This exposes an ownership gap requiring effective
vtable/indirect/stop caller review; it is not yet a whole-path leak proof.
Any new release must follow true table/GPU-user retirement, not merely a free
method name. No production patch, runtime or Host GPU mutation.

Complete manager initPagePool/free/registerEvents and pool withOptions reviewed/
pinned (0xcc/0xb4/0x50/0x4e). Native relocation at 0xc8240 resolves to
_real_ncpus, not a literal zero pointer or constant pool count. An initial
exploratory raw-pointer lookup returned zero because this is an unresolved
import cell; relocation resolves the provenance, not current guest CPU count.

initPagePool creates one pool per real_ncpus and passes options 0. Therefore
standard manager-owned pools have threaded flag +0x64 false: registerEvents
returns success without installing sources and schedulePrune runs synchronously.
The previously reviewed timer rearm hazard applies conditionally to threaded
pool users, NOT proof of a standard manager callback race. Other factory callers
and runtime option mutation must still be inventoried.

Partial creation failure with index >0 starts cleanup at the failed (null)
entry, then INCREMENTS the index and loops until wrap-to-zero. It does not
bound the index to real_ncpus or release the created prefix. This is a concrete
conditional out-of-bounds cleanup path, not a dynamically induced failure.
withOptions itself returns null after init false without releasing the newly
allocated pool, a separate local failed-init ownership gap. A correct unwind
must release only initialized prefix entries in reverse order, retain valid
ownership during cleanup and free/clear the array exactly once. Merely changing
INC to DEC skips index zero and is not a correct fix.

Manager registerEvents ANDs statuses across pools while continuing after false;
its own caller failure handling is pending. Manager free releases dummy pages,
stolen/fence objects and global page table, then calls releaseDeviceMemory.
That callee and all owner admission/retirement must be reviewed before claiming
safe pool release. No production patch or hardware execution in this checkpoint.

Complete pool registerEvents/pruneEvent/pruneTimer/inPruneList/allocation-report
bodies (0xc0/0x46/0x4e/0x24/0x6) reviewed/pinned with both event factory imports.
Threaded registration creates a software interrupt source (provider/index zero)
and a default timer, passing the pool as owner. It adds/enables both on
accelerator virtual +0x688 workloop WITHOUT testing either addEventSource result.
Timer factory failure releases/clears the interrupt source; interrupt failure
returns false. Actual callers must handle registration failure and avoid
duplicate registration; local body has neither an existing-source guard nor a
terminal admission flag.

pruneTimer clears scheduled flag +0x50 under optional lock then dispatches the
interrupt source virtual +0x1d8. pruneEvent calls prune(mode 1), then acquires
optional lock and calls schedulePrune, potentially rearming the timer. Neither
callback locally rejects teardown. inPruneList traverses raw free-list nodes
without local locking; describeDriverAllocations is a no-op, not backing-lifetime
or allocation accounting evidence.

Paired Tahoe Boot KC's already reviewed base timer vtable +0x218 resolves to
cancelTimeout. This matches the default factory used here: the pool free call
is cancellation, not a demonstrated callback drain. Default owner storage,
passive timer semantics and cancel rearm counterexamples are covered elsewhere
in this audit; concrete pool workloop/callback admission still require review.
The interrupt handoff plus pruneEvent rearm makes cancellation after destroying
queue state insufficient locally. No race was induced and no runtime was run.

Complete pool init (0x174), pool free (0x114), hash destructor (0x120) and
hash remove (0xc3) reviewed/pinned. Init stores a borrowed accelerator +0x18,
zeros state, initializes the free-list, then calls OSObject base init. It
allocates the 0x48 queue object and writes it before a null check; initial
vector grow(capacity 4) status is ignored. It similarly constructs the 0x18
bucket container before checking allocation and tolerates zero bucket capacity.
It publishes the queue and, in threaded mode, allocates a lock; lock failure
destroys/frees/clears the queue and returns false. Earlier allocator failures
are not safely represented by that checked lock-failure branch.

Pool free prunes(mode 2) and destroys/frees the queue BEFORE timer/source
cancellation, disabling and workloop removal. The queue pointer is not locally
cleared after free. Timer virtual +0x218 semantics and effective source classes
must be resolved, and callback admission/drain/outer locking proved: cancellation
after data destruction alone does not establish safe lifetime. Do not confuse
this order with proof a callback is actively racing in the current machine.

Hash remove unlinks/frees a matching node, updates bucket/hash counts, then
tail-calls shrinkIfNeeded. Missing key exits without change. Thus removal can
trigger the reviewed non-atomic rehash, including during prune; the failure
scope is wider than grow alone. Hash destructor frees hash nodes/bucket storage,
not the PoolElements or their DMA backing. It zeroes occupancy before calling
shrinkIfNeeded, making that call exit immediately. Hardware retirement remains
an outer owner responsibility, not a consequence of deleting hash metadata.
Native targeted checks pass; no production patch or runtime operation.

Additional complete shrinkIfNeeded (0x52) and percolateDown (0x15a) reviewed/
pinned. Shrink uses hash occupancy percentages, then tail-calls the same
non-failure-atomic resizeAndRehash; it has no success result to its caller.
percolateDown updates heap entries and immediately writes through operator[]
without contains guards. Its safety requires complete hash coverage for all heap
elements, which failed rehash copies can invalidate. Adding contains only at
eval's entrance does not protect this inner heap-index update.

Exploratory instruction-decoded direct CALL/JMP inventory of the native __text
section, excluding external relocation placeholders, found resizeAndRehash
from add at 0xc024 and shrinkIfNeeded at 0xbe3a; vector grow from pool init
0xa951 and pool grow 0xb3c2; hash add from pool grow 0xb3e8 and rehash 0xbed6.
operator[] direct sites are prune (0xac3d/0xac63/0xac83), grow
(0xb47e/0xb5df), eval (0xb6ba/0xb7a7/0xb7ff) and percolateDown
(0xc288/0xc2c1). Eval is invoked by allocatePage/releasePage at 0xb0cf/0xb8d3.
These are decoded direct edges, NOT a complete indirect-call or owner inventory;
cold/code-data decoding, addresses taken and lifecycle callers remain pending.
Pool init and hash removal/shrink invocation must be traced next. No production
patch, allocation failure injection or hardware execution in this checkpoint.

Further complete resizeAndRehash (0x170) and bucket-vector constructor (0xa2)
reviewed/pinned. Resize allocates a 0x18 container and calls the constructor
before checking the allocated pointer; constructor immediately writes three
qwords without a null check. Its separate bucket allocation failure leaves
size/capacity/backing zero. Resize nevertheless publishes that container,
resets hash counters and copies old entries through add while IGNORING each
insertion result. It then destroys old nodes/buckets/container and returns true.
Thus this resize is not a failure-atomic transaction preserving old entries.
Actual allocator failure was not induced; these are conditional native branches.

If a copy insertion fails, the old entry can be lost when old nodes are freed;
subsequent queue index lookup may encounter the missing-key 0x8 path. If bucket
allocation fails, later zero-capacity indexing is not locally guarded. Safe
replacement must allocate all destination storage/nodes before publishing,
preserve original entries on failure, and admit no new queue element until its
hash membership is established. All source-template callers and pool ownership
must be checked before implementing a native hook; the current audit contract
does not change or certify these algorithms.

Complete vector grow (0x78), hash add (0x120), contains (0x56) and operator[]
(0x56) reviewed/pinned. Vector grow returns false if requested capacity is not
larger or backing IOMalloc fails; allocation failure leaves the existing vector
intact. Hash add returns false for a missing bucket container or failed 0x20
node allocation. A resize attempt's status is ignored, but insertion continues
using the current bucket container. Resize callee semantics remain pending.

Thus PagePool grow's false-publication branches are not an assumed interpretation
of an opaque return value: vector/hash allocation failures can take them. Grow
still enqueues/counts the uninserted block and returns true. allocatePage retries
the allocation queue, which cannot discover that new block. Under repeated
publication failures, this can repeat growth until another allocation fails;
it is not locally bounded to one grow attempt. Prune can later process such a
free-list block without a hash entry, but does not make it allocatable. This is
static conditional failure evidence, not a hardware/OOM reproduction.

contains keys compare PoolElement's first qword (the monotonic block identifier).
operator[] returns node+8; its missing-key path zeroes RAX then still adds 8,
so the returned address is 0x8, not null or an inserted default entry. Reviewed
eval/prune guard lookups with contains, and successful add precedes grow's index
updates; whole caller inventory and mutation/locking still need review. A null
check on operator[] would not provide a valid missing-key guard. Do not patch
the shared template globally without validating all callers and owners.

Complete grow (0x52e) additionally reviewed/pinned with its descriptor factory,
CPU-map factory and atomic index imports. Correction: allocator target 0xb15e
is named grow, not allocatePoolElement. It allocates a 0xcc0 software block,
creates IOBufferMemoryDescriptor with options 0x23 and length 64 << page shift,
and checks descriptor prepare status. A failed prepare triggers prune(mode 0)
and one retry, then release/free on failure.

Successful prepare calls createMappingInTask, then immediately dereferences its
return to call mapping virtual +0x118 WITHOUT a local null check. A zero virtual
address completes/releases the descriptor and frees the software block, but
does not locally release the mapping object. Those failure paths need upstream
factory guarantees; they are not a reproduced panic/leak. Successful setup
stores map/block base, builds 64 page records with physical-segment virtual
+0x138 (no local zero/result validation), backing CPU address and zero refs.
Address truncation/segment-contiguity and default-map options remain open.

Queue publication takes the optional lock. Resize failure and hash insertion
failure converge on +64 availability, free-list enqueue and a true return;
failure to insert is not propagated to allocator retry. Consequently grow=true
alone does not prove a block is discoverable in the allocation priority queue.
Subordinate hash/resize contracts and recoverability must be established before
fixing publication; blanket false could leak or discard an already published
block. No production patch or actual allocation failure was induced.

Complete allocatePage (0x1f2), prune (0x436) and priority-queue eval (0x18a)
reviewed/pinned. Allocation takes the optional pool lock, selects the queue
head's nonzero free bitmap, removes an entirely free block from the prune list,
chooses a free page by BSF, decrements availability, records uptime, clears its
bitmap bit and reorders the queue. After unlocking it increments the returned
descriptor reference count through imported OSAddAtomic64. No age threshold,
GPU-completion test or quarantine intervenes between a released free bit and
this allocation. Empty/no-free queue unlocks and calls grow;
false returns null, true retries. That allocator and concurrent pool lifetimes
remain pending.

eval reorders using free-page popcount classes (zero, 1..48, 49..64) and uptime,
with hash-index helpers maintaining locations. It is an allocation preference,
not retirement accounting. Its subordinate hash/heap helper contracts are still
pending; these three reviewed bodies are not a complete pool concurrency audit.

Prune mode 1 uses elapsed uptime versus pool +0x58; other modes skip that age
filter. It removes eligible blocks from queue/hash/prune list, subtracts 64
available pages and moves them to a temporary list under the optional lock.
After unlocking it releases block +0x10 object, calls descriptor +0x08 complete
virtual +0x1f8 with direction 0 (status ignored), releases that descriptor and
frees the 0xcc0 software block. Object types, mapping revocation and DMA lifetime
must be traced through grow and descriptor factories before treating those operations as
safe. The prune delay is not a GPU completion proof and does not delay ordinary
page reuse. No production/runtime change or inferred historical crash cause.

Nine additional complete bodies reviewed/pinned: releasePage (0x15e),
schedulePrune (0x4c), both expandLevel variants (0x9e/0xde), shrinkRange
(0x2d0), and four shrinkLevel variants (0x86/0x86/0x86/0x4e).
Correction: mapDescriptor failure target 0xd3fa is shrinkRange, not releaseRange;
hierarchy pruning is not transactional removal of an installed mapping prefix.

releasePage clears CPU page backing with imported memset BEFORE the optional
pool lock. It sets the free bitmap bit, calls availability helper 0xb68c,
increments available-page count and, for a completely free block, records
uptime, enqueues it and schedules prune. No local GPU wait/invalidation precedes
clearing/free-bit publication. schedulePrune uses timer virtual +0x1e8 in
threaded mode or calls prune 0xab36 synchronously otherwise. Allocation/prune,
helper semantics and external GPU quiescence remain pending; this does not prove
a reachable stale-GPU use or historical crash cause.

Expansion allocates a PagePool page, fills 512 hardware entries with dummy
physical address masked to 39 bits plus 3, then allocates/zeros 16 KiB software
records. Software allocation failure releases the page and clears its pointer.
Child expansion publishes its parent hardware entry only after software
allocation succeeds. Page allocation invariants are pending; no local barrier.

shrinkRange walks the hierarchy and ignores shrink-helper statuses. Non-root
helpers act only when signed low-word count <=0: replace parent entry with dummy,
decrement parent count, release/clear descriptor, and free software records only
if the upper count word is zero. Root variant releases similarly. Shared marker
0x10200 inhibits ordinary low-word pruning; count/alias lifecycle needs all
callers, not a blind unmap shortcut. No local GPU invalidation separates entry
replacement from releasePage. Outer serialization, GPU completion and deferred
release remain required before cross-address-space rollback or dynamic tests.

Complete native bodies reviewed/pinned: hardware base init (0x3e), task address
mode (0x16), 64-bit descriptor map/remap/read (0xb6/0x8a/0x32), expand2 (0x84),
and PageDescriptor retain/release (0x14/0x38). Base init calls OSObject base init
through imported vtable, stores accelerator +0x10 and type +0x18 without a local
accelerator retain. Address mode reads task byte +0x2d9: zero returns 3 (64-bit
factory), nonzero returns 1 (32-bit factory). Writers of this byte and outer
accelerator/table lifetime are still pending; this does not establish the mode
of any current task.

Descriptor operations use only range start, not its length: root index selects
bits 39..47 and the subordinate descriptor index bits 30..38. Thus sharing is
at 1 GiB granularity, not an arbitrary byte-range copy. Map expands the root/
second-level structures, stores/retains the incoming descriptor, sets software
record flags 0x10200, and publishes its physical address masked to 39 bits plus
3 into the hardware entry. Expansion failure invokes shrinkRange then returns
false; that cleanup's ownership and failure semantics remain pending.

Remap releases the old descriptor BEFORE replacing/retaining the new one and
updating the hardware entry. No local invalidation/drain appears. Read blindly
dereferences the root/subordinate records, writes the descriptor pointer and
returns true; it supplies no null/bounds validation locally. Caller invariants
must be established before treating either operation as safe or erroneous.
Retain/release use imported OSAddAtomic64 on descriptor +0x28; release seeing
old count 1 tail-calls PagePool's release helper. Its actual reuse/free ordering,
aliasing assumptions, expansion callees and cross-owner invalidation remain
unreviewed. Do not infer a GPU lifetime barrier from atomic reference counting.
No production patch or runtime operation in this checkpoint.

Validation: both native payload contracts and four source guard/order mutation
checks pass, as does diff whitespace validation. The first new atomic import
check incorrectly expected a CALL for retain; disassembly/relocation identifies
its tail JMP, and the contract now checks that exact opcode (release uses CALL).
No binary or production change was made to resolve the fixture error. Full
static suite last passed at 825e45d; its CI 37163430723 is now successful.

Reviewed/pinned complete common per-process init (0x3a), 32-bit init (0xbe)
and 64-bit init (0x40). Common init delegates to hardware-page-table base init,
then stores task +0x20 and supplied options +0x28. The 32-bit initializer passes
options 0; 64-bit passes 1. Thus normal 64-bit source/destination pairs meet
the descriptor-sync branch condition reviewed above, not a speculative flag.
Base initializer and actual task address-mode selection still need review.

32-bit init clears four descriptor records and invokes virtual dummy mapping
+0x130 for manager ranges +0xa0/+0xb0/+0xc0 without testing their return values,
then reports success. Those dummy-mapping callee bodies remain pending.
64-bit init clears its root descriptor record and delegates to common init;
that local body supplies no range-by-range failure propagation.

Historical evidence only: archived ce166c8-runtime1/pre-reboot-live.log records
the VF GGTT [0x5104000,+0xf9c06000] and proxy backing at 0x419aa000. The former
crosses the fixed descriptor-share [0,1 GiB) window, and the latter is above
it. This makes the fixed window a concrete compatibility review requirement,
but does NOT establish current VF provisioning, that proxy backing must be
copied to every user PPGTT, or a dynamically observed missing user mapping.
Shared mapped-buffer clone/admission and descriptor granularity must be traced
before rewriting the range. Do not simply disable PPGTT/64-bit contexts.
No production/runtime/Host GPU mutation; native targeted checks pass.

Checkpoint validation: full `tools/check-static.sh` completed with exit 0,
diagnostics `/tmp/ngreen-static.NK1Ual`; the two existing TargetConditionals
macro-redefinition warnings remain. Paired local Tahoe KC contract checks and
`git diff --check` also pass. These are offline checks, not GPU/DMA safety proof.
Remote branch still matched 573ef8a before this checkpoint was committed.

# Mapping recovery frees other allocations and retries preparation

Reviewed/pinned complete freeToPrepareMapping (0x260) and base
freeWaitToPrepareSysMap (0x1bc). Both direct resource-prepare calls select the
first helper. Base accelerator virtual +0x968 resolves to the sys-map helper;
+0x940 resolves to freeWaitToPrepareVidMap (identity only, body pending).
Concrete Intel accelerator overrides of these slots remain to be resolved.

The ordinary system-map branch invokes recovery with false then true mode.
The base sys helper iterates accelerator collection +0xa08 twice, skips entries
with nonzero aggregate prepare count, optionally performs memory/event waits,
calls their virtual +0x1b8 unwire, then retries the TARGET mapping virtual
+0x138. It does not directly complete/clear the target command. The final
true-mode fallback sets target parent flag 0x20000 and retries once more;
earlier wire selects descriptor prepare arguments from that flag. Collection
helpers and memory/event wait callees are not newly certified here.

This is a resource-pressure retry protocol, not blanket failed-wire cleanup.
If the target remains unwired with stored command +0x148 from a prior failed
wire, another wire attempt has no local existing-command check before storing
a newly obtained command. Target inclusion in the collection, effective native
overrides and outer disposal must be established before claiming a reachable
leak or implementing a cleanup/force-release hook. Vid-map branch additionally
uses orphan-pool/virtual reclamation and timed escalation; those callee bodies
remain pending. No observed-panic attribution or GPU-retirement proof.
Targeted contracts cover complete selected bodies/base identities/direct edges,
not all indirect callers or runtime concurrency. No production/runtime changes.

Native follow-up: IntelAccelerator vtable slots +0x940/+0x968 are external
base vid/sys recovery imports at relocations 0xd19b0/0xd19d8, not distinct
Intel implementations. Both native payloads now check their exact imported
identity and slot position. Dynamic table mutation/other runtime accelerator
classes are not established by this fixed declared-table evidence.

# Recovery resident-set publication and borrowed iterator lifetime

Reviewed/pinned complete sysmem_wired (0x78), resident-set add (0x64), remove
(0x6c), iterator construction (0x1e), getNextMemory (0x70), sort (0x1c0),
reallocation (0x7a) and parent getLRUSeed (0x56). Direct-byte candidates for
sysmem_wired in the embedded accelerator range are the two previously decoded
factory prewired/successful-wire calls; addMemory has the sysmem_wired caller.
These selected decoded edges are pinned, not an exhaustive indirect inventory.

sysmem_wired publishes the memory into resident set +0xa08 and adds its size
to +0x350; sysmem_unwired removes it and subtracts its size. The examined fresh
ordinary wire failure happens before this publication. Under these known
paths, the recovery set therefore cannot be assumed to contain that failed
target and unwind its stored command. Pre-existing/alternate membership and
dynamic callers still require provenance.

Resident add stores a raw memory pointer, index +0xa4 and membership +0xa0,
without retain; remove trusts the index, compacts by moving the last pointer
and updates its index. Iterator returns a borrowed pointer, restarts on sort
generation changes and filters by signed LRU seed comparisons. Remove itself
does not update that generation; iteration after compaction may skip a moved
entry until the later sorted pass. Sort refreshes seeds, reorders entries,
rebuilds indexes and increments generation. No local locks or count/index
overflow/underflow guards are present in these selected bodies; outer locking
and memory lifetime must be established before a concurrency repair.

Reallocation doubles capacity, copies/frees the old buffer and installs the
new one; null allocation branches to an outlined failure helper (not newly
reviewed). Parent seed aggregates mapping seeds using signed comparisons;
mapping-seed callee and wraparound policy remain pending. This review does not
certify resident pointers across unlock/wait, mapper retirement or Host safety.
No executable patch or runtime mutation.

# Native 64-bit PPGTT map/unmap failure and retirement boundary

## Mapping last-release admission and deferred raw-list transfer

### Update fanout must participate in the same owner transaction

#### Plane cache replacement is owner transfer, not geometry validation

New complete KC review/pin: set_current_plane_ioSurfaceDeviceCache
`0x14bb31ee/0x21a`, SHA-256
`8beabc3abe7e423ff49dacf19daa96567c69e70b727205ae1db92b8b4ed10ce5`.
The previously reviewed argument setter reaches it at `0x14bb08ca` and
`0x14bb0d1a`; both edges now checked. An identical nonnull incoming cache
returns early. Otherwise the helper computes the plane/subplane record,
merges old resource and primary mapping event storage into the transaction
event through event-machine `+0x1b8`, completes the old resource via `+0x178`,
releases it, and releases/clears associated cache and related-object fields.
The alternate no-cache/old-resource branch also completes/releases the old
resource before replacement. No synchronous hardware completion wait occurs.

For a nonnull incoming cache it calls its resource virtual `+0x170` first.
False exits without publishing new cache/resource references; old owners may
already have been removed. True stores three related pointers in the live
plane record and retains each owner. The selected selector-8 route supplies
the previously established accelerator outer lock, but this complete helper
has no local lock and other callers remain separate obligations. Event merge,
resource complete and OSObject release are not interchangeable with GuC ACK.
Its complete body does not set private Intel rotation width/height or finally
release private rotation map `+0x238` directly. Consequently it is an owner
replacement entry, not the missing geometry setter or complete retirement
proof. Tests pin both resource complete calls, incoming prepare and the
failed-prepare branch, alongside the full body hash. Paired KC passes and
whitespace validation passes. No production/runtime change. Next trace the
geometry setter separately while including this replacement path in resource
retirement ownership and partial-prepare failure handling.

#### Selector-8 outer lock reaches concrete Intel rotation validation

New complete KC review/pin: transaction set_transaction_args
`0x14bb0714/0xab0`, SHA-256
`830382a2686dc7615224b4b12d072e1484eef55f80854c6f017ffae6b9d1ceb2`.
Re-read already-reviewed complete user-client transactionEnd
`0x14bb622e/0x232` and pipe transaction_end `0x14bb0560/0x1b4`.
The former takes accelerator mutex `+0x88` and busy lock before state checks;
its queue-full wait retains the pipe, unlocks, waits, then reacquires and
rechecks. The final transaction_end call `0x14bb6456` occurs while those
locks are held, before their common unlock path. Thus this selected selector-8
route supplies outer accelerator exclusion to rotation validation; it is not
a claim that every display/mapping entry shares that exclusion.

Pipe transaction_end calls set_transaction_args at `0x14bb0602`. That complete
body processes two plane records and dirty-bit-controlled state, performs
resource lookup/object-factory dispatch, stores and retains selected related
objects in transaction records, reuses/retains live-pipe record objects in
unchanged branches, updates plane rectangles and other transaction state,
and invokes concrete pipe validation through virtual `+0x8a0` at
`0x14bb1134`. The Intel table entry is validateTransaction `0x80bd0`.
Selected factory/helper semantics, namespace lookup ownership, earlier-field
validation and all virtual callees are not automatically certified by reading
this enclosing body. In particular, calls to plane-resource update helpers
still need review for rotation geometry and ownership side effects.

Nonzero validation status returns to pipe transaction_end, which preserves
the error at transaction `+0x58`, skips ordinary prepare, and nevertheless
queues the transaction through the existing cleanup/notification route.
There is no local PTE-prefix rollback in this error return. Previously reviewed
transaction free releases its retained plane-related objects, but that fact
does not prove the resource-private rotation mapping `+0x238` reference is
balanced or that GPU backing is safe to reuse. Do not bypass error queuing as
a shortcut. New paired-KC checks pin caller/argument-setter/validation/error
edges; native checks bind the concrete Intel virtual. Both pass. No production
or runtime change. Next: complete plane-update helper and rotation geometry
setter/owner-transfer semantics under this now-established selected outer lock.

#### Display transaction entry into rotation preparation

New complete native reviews/pins: DisplayPipe validateTransaction
`0x80bd0/0x21e`, SHA-256
`5b269acb9228757b84f056db88633774aff390b3016b48776bf76e7258b93a05`,
and DecodeTransaction `0x8091e/0x2b2`, SHA-256
`d23abcf017c44eb884e855914dd15d019d517c6bec1d71b231a6ca05ab182722`.
Selected import/call edges now pinned in both archived payloads. Decoded
direct-edge discovery located validateTransaction calling rotation creation
at `0x80d0f`; it is not an all-indirect-caller inventory.

Validation gets the framebuffer and dirty bits, decodes one dirty group,
dispatches framebuffer virtual `+0x6b8`, and under selected accelerator/pipe
feature guards iterates the decoded plane count. For each returned plane
resource, an existing rotation map `+0x238` is prepared through `+0x138` and
its bool stored in resource `+0x240`; an absent map calls rotation creation.
Failures set an error but do not locally release/clear the failed mapping or
undo its PTE prefix. The loop can continue. The function resets its saved
status before decoding a new dirty group; whether later groups can mask an
earlier error requires the effective transaction/decoder sequence and is not
claimed as a reproduced fault. There is no local accelerator-mutex operation
in this complete body; external caller exclusion remains unproved.

DecodeTransaction consumes dirty bits, classifies low-bit groups, inspects
selected transaction plane resources and an optional transformation matrix,
sets plane count for relevant groups and clears processed dirty bits. It does
not write resource rotation width/height or validate their nonzero values.
External transaction accessors and framebuffer command semantics are not
newly certified by these body reviews. This narrows rotation preparation to
the selected display transaction route, not arbitrary mappings or proof that
the present headless VF executes it. Next: effective outer display-transaction
serialization, upstream geometry setter and transaction cleanup after failure.
Both native payload contracts pass. No production/runtime change.

#### Rotation acquisition: borrowed task and conditional mapping-reference transfer

New complete paired-KC reviews/pins: `IOAccelResource2::getGPUTask`
`0x14b8c99c/0x22` (SHA-256
`7dc3a618b56c2611b1d27a58ce28260cd354c9d58275fb75b084e23e31fe0b3a`)
and `IOAccelMemory::createMappingInTask` `0x14b672ee/0x16` (SHA-256
`47b5beedeaa9f97ff450a1c3e79647872b26fbd32afe277c8638f9016202bac1`).
Native calls `0x752c3/0x75324` resolve to getGPUTask, not an accelerator
accessor: it reads resource Shared owner `+0x68`, returning Shared task
`+0x48` when present, otherwise accelerator `+0x150`. There is no local
retain or lock, so any transaction must stabilize the supplying owner before
using this borrowed task.

For the concrete Intel system-memory class, object slots `+0x138/+0x140`
import createMappingInTask and createMappingInTaskAtAddressLength at
`0xcd820/0xcd828`; native external relocation checks now pin both. The wrapper
zeros address/length arguments and tail-dispatches `+0x140`. Re-read the
complete already-pinned factory `0x14b67304/0x272`, without new whole-body
credit: a normal matching mapping gets a retain at `0x14b674a1`, whereas a
deferred match preserves/transfers its existing deferred reference through
list movement and flag clearing. New mapping allocation/failure uses the
previously reviewed accelerator factory and VA-recovery paths. This proves
the selected system-memory route, not every possible class of resource
backing `+0x88/+0x80`, nor mapping-array synchronization or GPU completion.

Rotation creation stores the returned reference in resource `+0x238` before
preparation; assigning that raw field does not itself add another retain.
Owner transfer must be distinguished from deferred-list reuse when designing
failure cleanup. Factory geometry arguments are zero here, while rotation
width/height are copied separately afterward, so this wrapper cannot be cited
as validation of rotation geometry. Both native payload contracts and paired
KC contracts pass. No production/runtime change. Next: trace the caller that
sets geometry and balances the resource's returned mapping reference, with
outer task/parent/mapping serialization proved before retirement integration.

#### Rotation owner cleanup boundary: complete is not final release

New complete native reviews/pins: resource init `0x6e840/0x8c`, SHA-256
`77551d181809aac756b0e8ae80a91f3df84d9cae6ee36b881c5ee5c90e9fd70b`,
and resource free `0x6f59e/0x26c`, SHA-256
`8bf6604632ce62b2bccd54c2d2ca22233633738b3de60ebbd4f00aa01a67a767`.
Re-read already-pinned complete `0x75380/0x6c`; no duplicate whole-body
credit. Init saves the inherited init bool, clears private owner fields
including rotation mapping `+0x238`, prepared flag `+0x240`, rotation mode
`+0x244` and width/height `+0x248`, then returns the saved bool. Initial
zeros do not establish what later geometry-setting APIs validate.

Complete checks `+0x240` first. When true it may merge mapping event storage
through accelerator's event machine, invokes rotation map virtual `+0x140`
and clears the prepared flag. When false it skips that entire path and
delegates inherited resource complete. It does not release or clear the
rotation pointer. Thus a rotation prepare-false result from the preceding
review is not followed by a local rotation-map completion in this method.
Completion and dropping the OSObject reference are distinct operations.

The complete native free body walks resource-info entries and returns selected
CCS-related ranges, frees resource-info/auxiliary arrays, releases owner fields
`+0x228` and `+0x220` (completing the latter first), then delegates inherited
free. It does not directly load/release/clear rotation field `+0x238` or
dispatch complete on that mapping. The previously reviewed inherited resource
free's treatment of primary mapping `+0x40` is not automatically treatment
of this private field. This narrows the missing ownership graph; it is not
proof of a leak because other effective callers, raw aliases, aggregate owner
storage or earlier cleanup could handle the mapping. Adding a blind final
release would risk double-release or premature backing reuse without that
graph and GPU-completion proof.

Both archived native payload contracts pass with the two new complete-body
fixtures. Decoded resource-method field discovery located init, creation and
complete accesses; negative field search does not exclude indirect addressing
or inherited/external cleanup. No production repair or runtime operation.
Next: establish rotation mapping acquisition/owner transfer and cleanup across
all preparation failures, and connect it to retained page/pool retirement
before native backing zero/reuse.

#### Effective rotation preparation and VA-allocation dispatch

Resolved the concrete Intel memory-map vtable: object slot `+0x138` at
`0xcd040` imports inherited `IOAccelMemoryMap::prepare`; slot `+0x150`
is native `IGAccelMemoryMap::allocGPUVirtualAddress` at `0x10e6e`.
The already-existing external relocation fixture pins the former import;
new checks bind it to the concrete table and pin the latter method and
rotation caller dispatches `0x75317/0x75342`. An exploratory relocation
script initially used LC_DYSYMTAB field indices relative to `+8` instead
of the complete command, producing invalid import output. Corrected that
read-only probe and verified against the existing contract; no payload or
production changes resulted.

Re-read the complete already-pinned inherited prepare `0x14bb7754/0x7c`,
commit_pte `0x14bb77d0/0x64` and prepare.cold.1 `0x14bbca32/0x62`.
First preparation of a mapping without installed-PTE bit 4 dispatches the
cold helper. It prepares the parent, invokes commit_pte, and completes the
parent in both commit-success and commit-failure cases. Only commit success
increments the prepare count. commit_pte dispatches map virtual `+0x170`
unless its existing special flags bypass that call; false prevents setting
installed bit 4. These software flags/counts are not a partial-PTE rollback
or GPU-completion proof. Neither complete inherited body validates Intel
rotation width/height. Resource rotation creation stores map prepare's bool
in resource `+0x240`; it does not locally release/clear the mapping on that
prepare-false path. The distinct preceding recovery-failure path does
release and clear resource `+0x238`. Later owner cleanup still needs review.

New complete native review/pin: allocGPUVirtualAddress `0x10e6e/0x212`,
SHA-256 `dbeabbc4bcff5ada45dd5be9ff14bb4ae00a66d544b720359040f504cf01058f`.
It ordinarily delegates to inherited allocation through the imported base
vtable. A selected flag/physical-address lookup path may set flag `0x20`,
call that same base allocator, and then store the selected physical address
at mapping `+0x98` even when the saved allocator bool is false. The body
returns that saved bool; this local publication is not independently proof
of active-list publication, safe backing ownership or a reachable failure.
Physical lookup semantics, range invariants and upstream caller cleanup are
not inferred from symbol names. No rotation-geometry check appears locally.

Both native payload contracts and paired Tahoe KC contracts pass. KC methods
are revisited, not newly credited. No production hook, deployment or Host GPU
operation. Next: effective resource geometry-setting/cleanup entry and complete
failure unwind, together with the independent pre-zero page/pool lease gate.

#### Rotated mapping geometry reaches private PPGTT through commitRange

New complete native bodies reviewed/pinned in both payloads:
`IGAccelResource::createAndPrepareRotationMapping` `0x7528c/0xf4`, SHA-256
`3ecec4f2acb2a437f81f304a571663c062632f47270ba1a17993e9a254029e3b`,
and `IGAccelMemoryMap::init` `0x10d5e/0x68`, SHA-256
`970afaf15c9854e913ad4aa67af32f4ff61ca1267ba3f4f06ad6dd1f15fea892`.
Re-read the already-pinned complete commitRange `0x14090/0x41c`; no new
whole-body credit for that method.

Mapping init clears rotation flag `+0x11c` and the two geometry dwords at
`+0x120/+0x124`. Resource rotation creation obtains a mapping, stores it at
resource `+0x238`, copies resource `+0x244..0x24b` into mapping
`+0x11c..0x123`, copies resource `+0x24c` into mapping `+0x124`, then sets
the low flag byte at `+0x11c` to one. Thus the width consumed by the private
mapper is resource `+0x248`, height is resource `+0x24c`. The complete
rotation-creation body contains no local zero-width/height rejection before
these writes and subsequent mapping preparation dispatches. Its virtual
callees and earlier geometry setters still require review.

commitRange tests mapping flag `+0x11c`, loads width/height at
`0x1411d/0x14123`, constructs the iterator, decrements height without a
local nonzero check, computes/clamps the initial cursor against range end,
and calls table virtual `+0x120` at `0x141d2`. The concrete 64-bit vtable
entry is private mapRangeRotated `0xd6ca`. It returns that method's status
after releasing its local iterator memory reference and setting deferred
hardware-flush flags. This closes the selected resource-to-private-table
geometry connection; it does not prove an invalid user input can traverse
all earlier layers. Upstream validation and the effective virtual preparation
callees remain necessary before a production guard/rollback replacement.

The new contracts pin the complete bodies, geometry-copy bytes, commit
loads/dispatch and concrete vtable entry. Both payload checks pass. An
exploratory decoded MOV-store search found resource initialization but not
the nonzero geometry setter; derived pointers, aggregate/vector copies,
inherited code and shared external storage remain possible. Negative search
is not a proof that geometry is immutable or always zero. No production
change, runtime mode observation, VM start or VF/PF manipulation.

#### Additional pruning entry: private rotated PPGTT mapping

Function-bounded decoded direct CALL/JMP discovery identified five selected
entries to native 64-bit shrinkRange `0xd3fa`: ordinary map `0xd19e`, rotated
map `0xd7e2`, unmap tail jump `0xdb18`, dummy map `0xdbec`, and descriptor map
`0xdd58`. Regression checks now pin all five edges in both archived payloads.
This does not exclude indirect, inlined, alias-derived or inherited entries.
It establishes that wrapping only ordinary unmap would leave allocation
rollback pruning outside the proposed retirement domain.

Complete native private `mapRangeRotated` `0xd6ca/0x2cc` newly reviewed and
hash-pinned, SHA-256
`3b6131194a023eb7513ccbb26a210c5fa2288c4fc74d66723188a46887a1b11b`.
This is distinct from the existing custom VF global/GGTT rotated mapper;
that repair does not cover this private PPGTT method. It expands first;
expansion failure shrinks at `0xd7e2` and returns false. On success it retains
the input memory descriptor for two local iterator copies, walks physical
segments and finds software leaf records through pageWalk3 `0xd874`.
It writes the PTE and increments the leaf count at `0xd8ae..0xd8b5` before
the unsigned division by iterator width at `0xd8de`. No local width-zero
check precedes that division. Rotation arithmetic and clamping update the
iterator cursor after the PTE store; caller geometry/preconditions remain
unproved, so this is a conditional hazard, not a demonstrated live fault.

If pageWalk3 returns false, `0xd915` sets BL to zero but flows onward through
the segment-iterator advancement path, not an immediate cleanup return.
A subsequent successfully completed physical segment can set BL back to one
at `0xd911`; neither path explicitly undoes already-written PTEs. Reachability
requires proving the relevant descriptor/segment and concurrent hierarchy
conditions. The final result is BL masked to one bit. The body releases its
local memory-descriptor references on normal completion, but contains no
GPU invalidation acknowledgement before pruning or return. A CPU descriptor
retain does not certify the lifetime of hardware page-table pages.

Both native payload contracts pass with the new complete-body fixture and
selected store/divisor/result anchors. These preserve evidence of unrepaired
behavior; they are not correctness tests for that behavior. No production
hook or runtime operation. Next: include rollback pruning in the transaction
owner graph, prove rotated caller geometry, and build prefix-rollback tests
before replacing this method; retain the independent page/pool lifetime gate.

#### Native PageTableMode provenance follow-up

Re-read complete already-pinned task init `0x788c/0x1aa`, manager
initDeviceMemory `0xe7b2/0x410`, and task address-mode getter
`0x82c6/0x16`; no new whole-body credit. Task init loads accelerator's manager
at `+0x1260`, copies manager byte `+0x101` into task `+0x2d9` at
`0x791b..0x792d`, before base init and private-table factory publication.
Manager policy uses the property string `PageTableMode` at `0x9198d`,
defaults its local integer to 64 at `0xea25`, and stores the comparison
against 32 into `+0x101` at `0xeb88..0xeb93`. Its property lookup paths
include accelerator properties, Development dictionary fallback and the
IODeviceTree options node. External cast/parse imports and live property
values are not newly established by this selected data-flow check.

The getter maps zero to mode 3 and nonzero to mode 1. The already-pinned
manager factory compares exactly those encodings at `0xf8ee..0xf8f7` and
calls the 64-bit/32-bit factories respectively. Thus absent overrides, the
native policy selects 64-bit; a successfully parsed value 32 selects the
known defective 32-bit constructor. This is not evidence that a present
running task took either branch. Function-bounded Capstone discovery found
the selected `+0x2d9` write in init and read in the getter; that inventory is
not an absence proof for aliases, indirect addressing or inherited code.

The native regression contract now pins the manager default/comparison,
task copy, getter, factory compare and property name in both archived
payloads, alongside their existing whole-body hashes and direct-call checks.
Both payload checks pass. The initial string fixture erroneously assumed
padding and then used the wrong fixed span; corrected it to the literal's
computed length, without changing the payload or production driver.
Full static suite last passed immediately before this fixture addition at
`1205c20` (`/tmp/ngreen-static.OEgEdI`); no full rerun claimed here.
Next prioritize the default 64-bit retirement transaction and its page/pool
leases, while preserving the supported 32-bit constructor repair requirement.
No runtime deployment, mode override or acceleration proof in this checkpoint.

#### TGL primary-specification check: 32-bit PPGTT is not obsolete by definition

The Intel-authored [TGL Volume 2d, Command Reference: Structures](https://cdrdv2-public.intel.com/703050/intel-gfx-prm-osrc-tgl-vol-02-d-command-reference-structures.pdf),
Doc Ref `IHD-OS-TGL-Vol 2d-12.21`, printed pages 277-278 (PDF pages
293-294), was downloaded and both complete relevant pages were rendered and
visually inspected. The downloaded file SHA-256 is
`bc4ff0780e992f1e5c51b5d3ef8c5850620444d7b484110f419c6d7cb837d5a7`.
The context-descriptor bits 4:3 table specifies `01b` for legacy 32-bit
PPGTT with the PDP descriptors describing the 4-GiB address space, and `11b`
for legacy 48-bit-canonical PPGTT with PDP0 identifying the PML4 and the
other PDP descriptors ignored. These are supported TGL modes, not merely
an inference from a pre-Gen11 document. The same table requires privilege
access bit 8 and describes the legacy fault-and-hang behavior; a page fault
must not be treated as an automatic context switch or successful quiescence.

Cross-check against local i915 source at
`c613c76e2e7023b1617bed346b9d35bf871fb958`:
`gt/intel_lrc.c:init_ppgtt_regs` selects the four PDP roots versus PML4;
`lrc_descriptor` selects the corresponding legacy addressing encoding;
`gt/intel_gtt.h:i915_vm_is_4lvl` derives that choice from VM size.
`i915_pci.c:GEN8_FEATURES` sets 48 address bits, inherited through
GEN9/GEN11/GEN12, with explicit platform overrides such as EHL/JSL 36 and
DG1 47. Linux's configured address-space size is not proof that hardware
rejects the 32-bit mode.

Consequently, the previously pinned native 32-bit constructor's nonempty
segment unmap with zero software-root arrays remains a real conditional
constructor defect. It cannot be dismissed or globally bypassed as an
unsupported TGL feature. This specification check does **not** establish
which mode the present ADL VF actually selected, validate every Gen11+
platform, prove GuC/VF acceptance of a context, or attribute the host crash.
Before any constructor replacement, prove the mode-selection caller,
initial root/page ownership, allocation-failure unwind, and context
publication ordering. Independently, 64-bit PPGTT retirement still needs
ownership retained before the descriptor release path zeroes a page and
completion proved before reuse. No runtime deployment is authorized by
this documentation-only cross-check.

Validation for this follow-up: the complete `tools/check-static.sh` suite
passed (exit 0, diagnostics `/tmp/ngreen-static.OEgEdI`; two existing SDK
macro-redefinition warnings), and the paired Tahoe SystemKC/BootKC contract
test passed. Previous HEAD `a688c8e` CI run `37177118629` also completed
successfully. These results check the current static contracts, not corrected
constructor behavior, whole-project audit completion, or GPU acceleration.

Constructor range provenance and conditional 32-bit defect (2026-10-04):
complete manager initDeviceMemory 0xe7b2/0x410 reviewed/pinned; it initializes
device memory and task address-mode policy but does not populate table root
records. Revisited already-reviewed manager init, initSegments and base table
initializers. initSegments 0xebc2/0xb4 assigns +0xa0/+0xb0 ranges and a fixed
+0xc0 range start 0x40000000, length 0xbe000000 at 0xec45..0xec5c. This third
range is unequivocally nonempty after successful segment setup, independent
of BAR/stolen-memory input. Its complete body is now in consolidated fixtures.

The 32-bit constructor zeroes all four inline directory records first, calls
the base initializer (which only sets owner/type/task/options, not roots),
then dispatches concrete unmapRange on manager +0xc0. That range starts in
directory 1. Selected unmapRange immediately reads that directory's zero
software-entry pointer and dereferences it for the first page; it neither
allocates nor checks null. Thus if this exact 32-bit constructor is selected
after normal manager segment setup, its precondition is violated, not merely
an unknown possibility of zero-length input. Factory/mode reachability for
the current device has not been demonstrated; this is not a reproduction or
an explanation of the earlier Host i915 failure.

Do not fix this by suppressing nonempty unmaps globally or pretending roots
exist. A repair must establish correct empty/dummy root initialization before
any range operation/root getter and preserve supported mode semantics, or
explicitly establish hardware support/deprecation using the authoritative
Gen11+ interface. Next: reconcile the 32-bit mode with current hardware/PRM
and implement the proven constructor correction with failure unwind tests;
the independent 64-bit retirement blocker remains. No runtime change.

32-bit constructor unmap preconditions (2026-10-04): complete concrete
unmapRange 0x12282/0x8a newly reviewed/pinned, paired to table object +0x130
and existing init calls 0x11d6e/0x11d8b/0x11da8. Zero range length returns
immediately. Nonempty ranges directly index the inline directory's software
entry array, dereference the selected leaf descriptor CPU address +0x20 and
write a dummy physical PTE from manager +0xf0. There is no allocation, missing-
directory/leaf check, local retain/mutex or invalidation acknowledgement in
this body. It therefore does not construct absent root descriptors.

Combined with the reviewed init's initial zero records, those constructor
calls require either zero effective ranges or a separately established
directory/leaf precondition. Current constructor/manager range provenance
and downstream context mapping have not certified that precondition; do not
claim a reachable null fault without proving actual nonzero input. Similarly
successful factory completion is not alone proof all four root descriptors
are ready for the unchecked getter. This narrows the root-publication inquiry:
trace mapping/commit establishment of roots and effective constructor ranges,
then hardware registration/scheduling; do not invent a getter fallback or
silently disable 32-bit mode to bypass missing evidence. No runtime change.

Root snapshot versus hardware publication (2026-10-04): complete 32-bit
getter 0x11e44/0x20 reviewed/pinned. It walks four inline parent records
+0x40/+0x60/+0x80/+0xa0, dereferences each descriptor +0x18 and returns four
physical addresses without null tests or retains. Complete 64-bit getter
0xd0ae/0x20 returns root +0x40's descriptor physical address and zeroes the
other three output slots. Both concrete object vtable +0x148 bindings checked.

Inside the previously reviewed context init, the instruction-aligned selected
edge 0x7c29b obtains context's task +0x58, task private table +0x260, and calls
that getter at 0x7c2ad into a four-qword stack array. The following loop copies
the snapshot's upper/lower halves in reversed root order into context-image
fields. This is CPU-image construction, not by itself the GuC publication
boundary. No new complete context-init review credit is claimed.

Revisited existing 32-bit table init: it zeroes four records and invokes three
range-unmap virtuals before returning true; the body does not directly certify
all root descriptors are nonnull. Therefore do not assume every successful
table factory already satisfies the getter's four-nonnull requirement or that
expand's parent-allocation rollback is construction-only without tracing the
effective mappings/root consumers. Existing 32-bit runtime is not tested, and
this is not a reproduced null fault. Next: effective context-image registration/
scheduling and root update order versus table mutation/failure; collector must
retain roots/backing until that real hardware boundary is safe. No deployment
or VM start.

Pre-zero hook caller scope (2026-10-04): function-bounded decoded direct-call
inventory identifies descriptor release from 64-bit shrink levels, remap,
expand failure paths, 32-bit free and 32-bit expand; releasePage's selected
direct caller is descriptor final release. This inventory excludes external
relocation placeholders, but is not complete indirect/inlined reachability.
It reinforces that a generic release interception must distinguish normal
retirement from construction/rollback, not require live transport for every
new unpublished page cleanup.

Complete 32-bit PPGTT expand 0x1242e/0x23e newly reviewed/pinned. It can allocate
a parent descriptor, store it in the inline directory record and fill its 512
PDEs with a dummy physical address. It then requests a 0x4000-byte software
directory-entry array; on null, 0x12646 releases that parent descriptor and
0x1264f clears the record before returning false. Leaf creation similarly
stores/fills a descriptor and later writes its physical address into the
parent PDE at 0x12560. Selected IOMalloc/memset imports, rollback release and
clear are now checked. There is no local invalidation wait or owner lock.

The new-parent failure branch does not itself write a leaf PDE, but that alone
is not proof the parent physical address is invisible to hardware: root-
directory consumers and outer construction/publication need verification.
Do not mark this branch safe-unpublished solely because it is a rollback.
A collector policy must establish publication/generation/ownership before
choosing bypass versus retained pre-zero retirement, including bootstrap when
CTB is not yet usable. No production hook installed; next trace 32-bit root
directory address publication and its relation to this expand failure path.

Concrete table/pool owner reference pairing (2026-10-04): verified twenty
external vtable imports across IGPagePool, 32/64-bit per-process tables and
global table: object slots +0x20/+0x28/+0x48/+0x50/+0x58 bind OSObject retain,
release, taggedRetain and both taggedRelease overloads. Complete paired Boot
release 0x10, taggedRelease 0x20 and threshold overload 0xa0 reviewed/pinned.
Ordinary release passes null tag; taggedRelease passes threshold 1 to the
virtual overload, which uses locked compare/exchange and dispatches object
+0x90 free only at its terminal-count transition. Special terminal/tag-accounting
diagnostics are preserved. This identifies the actual reference API for the
selected owners, not a raw descriptor counter masquerading as pool ownership.

An acquired live owner reference can defer ordinary OSObject final-release
destruction, but does not prevent explicit free calls, arbitrary threshold
release callers, table mutation or descriptor final return. Pool/table reference
acquisition still needs outer serialization before dereferencing borrowed
addresses. Keep separate descriptor and pool leases through matching GuC
invalidation and release descriptor first/pool last; a retained table alone
is not proof of an owned pool relationship. Current runtime collector remains
unimplemented. Next: concrete acquisition/collection hook boundaries before
descriptor zeroing, covering all table fan-out and destruction paths.

Concrete task reference acquisition (2026-10-04): native IGAccelTask vtable
object slots +0x18/+0x20/+0x48 import OSObject getRetainCount/retain/taggedRetain
at 0xca608/0xca610/0xca638, now verified in both payloads. Complete paired
Boot bodies are reviewed/pinned: getRetainCount reads low 16 bits at object
+8; retain dispatches object +0x48 with null tag; taggedRetain performs a
retrying locked compare/exchange on the 32-bit count at +8 and has special
terminal-count handling (return or diagnostic panic), not a pointer-validity
or resurrection API. The existing native task release's count-one test and
owned-context cleanup were revisited only; no additional whole-body review
credit is claimed for that already-pinned function.

A prospective task lease can use the concrete inherited retain API only
while a real owner/outer mutex proves the pointer is live. Reading a raw list
pointer, dropping exclusion, then retaining it permits destruction between
lookup and count access. Likewise one task retain does not itself retain a
shared manager's page pool or independently stabilize every raw managed-table
node. The collector must acquire all relevant owners inside proven admission,
hold them through matching invalidation completion and release in proven
dependency order. These are integration obligations, not an installed runtime
collector or DMA proof. Next: table/pool owner retention and coverage of final
release/destruction entry points. VM containment remains unchanged.

Task-list teardown and selected observer (2026-10-04): complete inherited
IOAccelTask::free 0x14b9df3e/0x144 reviewed/pinned. It walks existing mapping
collections, performs mapping virtual cleanup, releases supplied allocator
references, then calls TaskList::removeTask at 0x14b9e05f before base object
free. The list is accelerator +0xc48 or +0xc58 according to task +0x60 bit 0.
Complete removeTask 0x14b82032/0x9a searches raw head/next pointers, rewrites
head or predecessor, decrements count and clears task +0x18 only on a match;
empty/absent cases log and return. No local lock, retain or hardware wait is
present. Combining the already-pinned native task free with this newly reviewed
base body proves native private page-table release precedes accelerator list
unlink. Raw list membership therefore cannot alone certify table lifetime.

Complete accelerator freeAllGPUMappings 0x14ba55fc/0xba reviewed/pinned: walks
active +0xc48 tasks and invokes task +0x128, then orphan +0xc58 tasks, invokes
event-machine cleanup and task release +0x28. It locally acquires neither
mutex nor task retain. A function-bounded direct-call inventory identifies
additional TaskList iterator consumers in dynamic-VA, vidmem, sysmem, orphan
cleanup, sleep/wake and allocation reporting. Those edges identify further
review targets, not all inlined/indirect observers or evidence that callers
are unlocked. Selected caller outer locks may provide necessary exclusion.
Do not asynchronously carry raw iterator tasks/tables past that exclusion
without independent retained owners and admission. No runtime change; next
verify effective collector caller lock and native task final-release domain.

Shared construction outer-mutex proof (2026-10-04): complete inherited
SharedUserClient2::start 0x14b900c6/0xea reviewed/pinned. It acquires accelerator
+0x88 mutex at 0x14b90129, adjusts the +0x90 lock-entry counter, calls
lock_busy/notification and invokes client object slot +0x990 at 0x14b9015c.
It records returned AL in +0x108, balances notification/busy state, unlocks at
0x14b90192 and returns the saved success. The concrete Intel client inherits
this start through external relocation 0xdb7e0. Its +0x990 override is native
sharedStart 0x783f4/0x40, now reviewed/pinned; that calls base header +0x9a0
(object +0x990) using external vtable pointer 0xc81c8, then initializes its
Intel-specific channel fields only after base success.

Complete base sharedStart 0x14b9275a/0x62 calls accelerator +0x8b0 to obtain
Shared, stores it in client +0x100 and publishes the client list link on
success. Complete createShared 0x14ba3ab8/0x50 invokes newShared +0xa08, then
Shared init +0x118, releasing the object and returning null on failed init.
Intel accelerator inherits both createShared and newShared, verified by
relocations 0xd1920/0xd1a78; base Shared init slot is also paired. This connects
the real Intel entry to the prior task-construction graph under one outer
mutex, including factory failure cleanup. It explains how the selected start
entry can exclude observers that obey the same mutex during early raw task/
Shared publication. It does not establish that every observer/factory entry
uses that mutex, nor provide retained task/table/pool leases for an async
collector. Do not repair this selected construction ordering speculatively.
Next: remaining list observers and lifetime-admission boundaries before
implementing pre-zero PPGTT retirement. No runtime change or VM start.

Task factory ownership follow-up (2026-10-04): complete native withOptions
0x7844/0x48 reviewed/pinned: metaclass allocation, initWithOptions at 0x7870,
failed-init virtual release +0x28 and null return. Complete kernel factory
0x2d204/0xa tail-calls it. Complete user factory 0x2d20e/0x3c checks accelerator
+0x1191 bit 1: clear means retain kernel task +0x150 via virtual +0x20 and
return that same task; set means tail-call withOptions. Neither factory has a
local mutex. Native accelerator vtable object slots +0x998/+0x9d0 are verified
as user/kernel factories, respectively. A per-Shared transaction cannot assume
exclusive task/table ownership: multiple Shared objects may own the same
retained kernel task in this supported native mode.

Complete inherited Shared2::init 0x14b8e516/0x208 reviewed/pinned: stores
accelerator +0x78, references the process task, invokes accelerator factory
+0x998 at 0x14b8e547 and stores its owning result at Shared +0x88; null rejects.
It then publishes Shared in accelerator's list before constructing namespace
and remaining bookkeeping. This selected body has no local accelerator mutex;
outer Shared allocation/init caller serialization must still be traced. A
function-bounded instruction scan found other +0x998/+0x9d0 calls in different
classes, whose receiver types differ: those are not automatically factory
callers. Construction publication is not a demonstrated race without the
outer caller/observer contract. No runtime change or VM start. Next: effective
Shared factory/creation caller lock covering task and Shared publication.

Task construction/publication admission (2026-10-04): complete native task
initWithOptions 0x788c/0x1aa reviewed/pinned. It clears private table +0x260,
initializes managed-list head +0x268 to zero and tail +0x270 to the head's
address, then calls base init via header slot +0x128 at 0x797e. External
relocation 0xc8118 resolves the header pointer to IOAccelTask's vtable (this
is header-relative, object slot +0x118). Base init completes before native
newPageTableForTask 0x79a4, private-table store 0x79a9 and managed-list init
0x79b8. Subsequent stamp/scratch initialization and sampler construction
still precede the native success return; selected failures release/reset the
private table or return false for factory cleanup. Failure is not proof of
hardware quiescence or that no list observer can see the partial task.

The complete inherited IOAccelTask::init 0x14b9de06/0x138 was reviewed/pinned:
it initializes owner/event/allocator state, retains supplied range allocators,
and calls addTask at 0x14b9df26 with accelerator +0xc48. Complete addTask
0x14b8201e/0x14 simply stores previous head into task +0x18, publishes task as
new head and increments count; it has no retain, mutex or initialized-state
filter. Thus base init genuinely publishes a task before native PPGTT/list
construction is complete. This is construction order, not a reproduced race:
the effective factory's outer lock may exclude all observers. A lifetime
transaction must establish that outer serialization/publication boundary
rather than assume every task reachable through the accelerator list is
fully constructed. Next: effective withOptions/factory callers and task-list
observer lock contract. No runtime change or VM start.

Selected completion lock regression contract (2026-10-04): current synchronous
TLB waiter retains gVfGucLock across direct poll, so its completion cannot
require that lock. Re-read the existing native software wrapper 1f9a0/a4
(already reviewed/pinned, not newly credited): it invokes the routed parser
and interprets its normalized short header without a producer/accelerator
lock. Current direct poll/drain/parser plus readiness, mapping identity,
credit/fault and IRQ-gate helpers use the scoped G2H +0x20 mutex; selected
context completions additionally acquire the context spin lock briefly.
The selected bodies contain no inverse GuC/H2G/accelerator acquisition.

New structural checks cover these twelve selected source bodies and the
waiter's lock/poll/unlock ordering. Four injected dependencies (GuC lock in
credit return, accelerator lock in drain, H2G lock in pending check and a
nested invalidation wait in fault publication) must reject. This is a literal
source regression check, not a compiler call-graph or whole-driver deadlock
proof; external primitives, indirect route installation, context-lock users,
all destruction callers and retained-owner admission still require their
own proof. In particular it does not authorize waiting while an arbitrary
native caller holds an unknown lock, nor remove the pre-zero PPGTT hold.
No production behavior or runtime artifact changed.

Concrete resource-delete admission (2026-10-04): complete SharedUserClient2
delete_resource 0x14b91470/0x116 reviewed and pinned. It temporarily increments
accelerator +0x90 around acquiring IOLock at accelerator +0x88 (call
0x14b914b4), then calls lock_busy and notification virtual +0x850. Namespace
lookup 0x14b914f0 is borrowed but occurs inside that mutex. Successful lookup
for a resource whose type byte is not 0x0a invokes resource virtual +0x160 at
0x14b91513; type 0x0a skips that invocation and still records success. Missing
lookup returns e00002c2. Both paths converge on notification +0x858,
unlock_busy and IOLockUnlock at 0x14b9156f. Native IGAccelResource's object
slot +0x160 is external relocation 0xd9540 to base Resource2::sharedRelease,
now contract-checked, connecting this selected concrete class to the existing
reviewed release graph rather than assuming base-vtable inheritance.

This supplies real outer serialization for one resource-deletion path, in
addition to already-reviewed color/depth resolve callers. It does not prove
all GC/task/map/PPGTT destruction is serialized, nor that a GuC wait while
holding accelerator +0x88 cannot deadlock an effective completion/submission
path. The temporary +0x90 counter is not a resource retain. A retirement hook
must respect the existing mutex and sharedRelease's nested release graph;
do not add an unconditional reacquisition or equate deletion success with
GPU translation acknowledgement. No runtime changes; containment remains.

Event-pair destruction follow-up (2026-10-04): complete Resource2::free
0x14b86478/0x41c is now reviewed and fixture-pinned. For nonnull mapping +0x40
it calls remove_resource at 0x14b86588 and mapping release at 0x14b86594,
clearing +0x40 before reaching the event-pair branch. At 0x14b86753 it reads
+0x90, skips null, clears that field before typed free through 0x101f2;
paired import 0x24298 resolves to Boot `_IOFreeTypeImpl`. The body has no
local lock or direct event-completion wait, but its virtual/type-specific
callees and outer destruction serialization have not been certified. This
is not a reproduced UAF. A deferred event snapshot cannot establish safety
by retaining an event address alone: resource/mapping owners and admission
must be stabilized before destruction can reach these releases. No runtime
hook has been installed and the independent pre-zero PPGTT blocker remains.

The complete mapping remove_resource callee 0x14bb79c2/0x5e was also reviewed
and pinned. It searches mapping +0xa0's raw resource-pointer array using count
+0xb0, decrements count only on a match, then compacts following entries; an
absent resource leaves count unchanged. It neither retains/releases resources
nor acquires a lock or waits for an event. Therefore removal from this array
is not itself an event lease or a completion barrier. Caller/outer lifetime
serialization remains required; the subsequent mapping release is separate.

Allocator-policy clarification: local XNU kalloc_type_impl_external explicitly
applies Z_KPI_MASK in both zone and heap branches, consistent with the reviewed
Boot mask instruction. That mask excludes Z_NOFAIL. zalloc.h documents that
Z_WAITOK never fails for a non-exhaustible zone, but an exhaustible zone may
fail at its limit. Thus neither the high-level typed-wrapper NOFAIL request
nor its loss at the KPI establishes selected-callsite null reachability.
Runtime typed-view fixups, selected zone/heap and its exhaustion policy remain
to be proven; do not add a speculative initializer repair on this evidence.

Resource event-pair producer follow-up (2026-10-04): the complete SystemKC
Resource2::initialize at 0x14b88c08 (0x33e, fixture-pinned) sets flag +0xf bit
0x10 for NewResourceArgs bit 12, stores the allocation at +0x90, and initializes
the two event records without a local null check. Its 0x1020a import resolves
through chained slot 0x242b8 to Boot `_IOMallocTypeImpl`, not plain IOMalloc.
The complete 0x20 wrapper at 0xffffff8000a85dc0 selects flags using typed-view
+0x28 bit 0x10 and tail-calls `_kalloc_type_impl`; that external wrapper masks
input flags and adds its own internal flags. Local XNU IOLib.cpp provides
context (typed zone allocations request Z_NOFAIL), not an exact-build proof of
downstream zone policy. Consequently the lack of a null branch is not yet a
proven reachable failure and does not justify a speculative initializer patch.
This does not resolve nullable event-vector growth omissions, pointer leases,
or the pre-zero PPGTT retirement transaction. VM containment remains in force.

Null-entry admission follow-up: re-read the previously whole-pinned channel
setEventStamp wrapper and Fast2 setEventStamp (0x9e). Channel forwards the
supplied event pointer to event-machine +0x1d0; Fast2 begins reading
[rdx+rax*8] at 0x14b96242 without an event null guard. Both reviewed collector
copies can append resource event-pair +0x90 and a second pointer conditionally
set to zero when that base is null. Thus a zero vector ENTRY cannot be treated
as an empty vector or completed event; valid producer construction must ensure
required pair pointers are non-null before those paths reach this consumer.
The producer-side invariant remains unproven, so this is a conditional graph
hazard, not a demonstrated reachable malformed resource or panic attribution.

Added the consumer read anchor and extended the test-only snapshot predicate:
exact sequence matching also rejects expected/observed null entries, while a
genuinely selected empty pair of vectors remains admissible. Non-null alone
does NOT establish mapped accessibility, canonical address, event owner retain
or backing lifetime. Runtime enumeration/owner acquisition is still absent;
no driver behavior, VM or Host GPU state changed this checkpoint.

Offline expected-event admission specification: added 2,720 omission states
for the two reviewed collector variants, Boolean wait skip, pair-vs-indexed
resource events, optional mapping event and two potentially aliased resources.
Expected wait and update sequences remain distinct; legal aliases retain
multiplicity. All incomplete subsets reject exact expected-sequence matching;
same-count substitution demonstrates that a length-only check is insufficient.
Intentional wait omission still requires update events, and a selected genuine
no-op is distinguished from dropping a required wait set.

This is a test-only symbolic snapshot/specification, not executable native
instruction emulation, pointer validation, full protocol model or a deployed
admission gate. Arbitrary omission sets are a conservative superset, not a claim
that every subset is realizable by the native allocator. Resource values here
remain assumed stable; actual enumeration must acquire owner protection before
reading pointers and preserve it across any wait/cleanup. Ordering equivalence,
all unreviewed collector copies, null event-pair semantics and direct GL uses
remain pending before adopting this specification as a runtime predicate.
No driver behavior or VM/Host GPU state changed this checkpoint.

Duplicate collection ABI distinction: reviewed complete direct user-client
grow copy at 0x79a2e (0x78) and private AddDstResourceEvents at 0x79aa6
(0x176), pinned selected name/address/boundary/hash and allocator imports.
Grow bytes match the previously reviewed CCS copy, but collector bytes and
semantics differ: when resource +0xf bit0x10 is clear, user-client helper
tests DL at 0x79ac1 and nonzero skips the initial wait-vector append, while
continuing update-event collection. The CCS copy at 0x7582a has no corresponding
DL branch. Native depth client passes zero to this helper at the reviewed
resource/storage callsites, but that does not justify treating all other uses
of the symbol as equivalent. Both copies still skip failed growth with no
aggregate error return.

Completeness design must enumerate expected wait/update entries from the actual
resource flags and each selected helper's option, not demand an unconditional
fixed count or route duplicate symbols by name alone. Retain legal empty/no-op
semantics while distinguishing allocation omission from intentional omission.
This checks two selected copies, not the entire duplicate-symbol population.
No new production/runtime change or firmware ownership proof claimed.

Depth caller status/lock follow-up: reviewed complete SharedUserClient
depth_resolve at 0x78434 (0x52a) and Metal depthStencilResolve at 0x4f150
(0xa0); full bodies, imported lock/busy/lookup and selected dispatch/status
anchors pinned in both payloads. User-client allocates two vectors through a
different duplicate grow copy before taking accelerator +0x88 at 0x7857f;
initial growth results are not checked. Under that mutex it looks up/binds
resource, collects resource/storage events through another private copy,
builds depth params and calls native publisher at 0x788e6. Post-submit cleanup
removes resource from channel/completes preparation, then clears return status
r15d at 0x78924 before busy/mutex unlock at 0x786a6/0x786b2. There is no local
native submission-result check on this path. Pre-submit validation/bind failure
uses error cleanup with 0xe00002c2. Effective lock is concrete for this caller,
not a universal proof of Metal/GL serialization or firmware completion.

Metal depthStencilResolve tests selected entry resolve bits, optionally invokes
resource depth resolve at 0x4f1c5, maps AL false to integer status 10 and true
to zero; no-op selection also returns zero. Do not conflate that status with
the user-client IOReturn. Its native body has no local lock/retain. Larger
Metal render and GL token callers are identified by function-bounded direct
scan but NOT whole-reviewed here. Consequently pre-publication admission must
cover distinct status contracts, selected duplicate collection helpers and
prepare/channel pairing rather than changing one shared void publisher and
assuming every caller observes failure. No production/runtime change here.

Native depth publication boundary: reviewed complete accelerator
submitDepthResolve at 0x2c656 (0x1f6), pinned both payloads, aggregate barrier,
chunk assembler edge, imported submitBuffer and software-phase loop anchor.
It obtains task contexts, cleans accelerator aggregate +0x11e8, merges the
wait-vector entries from params +0xb8/+0xc8 through event virtual +0x1c8,
and invokes barrierForWaitEvents. An empty vector uses AL=1, bypassing an
outstanding aggregate barrier; that is not evidence every required source
event was collected. It then prepares FIFO updates from +0xd0/+0xe0.

The depth chunk assembler at 0x85a8c is resolve_hiz_g7 (0x5738); only its
identity/edge is reviewed here, NOT its whole body or return-status semantics.
Its AL controls a diagnostic block, while both branches join submitBuffer
at 0x2c82e. After each publication, software phase [rbp-0x2c] is compared
with 0xe and can repeat FIFO preparation, assembly and publication. Therefore
AL must not be treated as an assembly error without reviewing the assembler,
and phase 0xe is not a GPU/GuC ACK. No independent complete-collection status
is tested in this wrapper. No local mutex/owner retain is present; effective
callers remain separate review work.

Reliable admission must reject an incomplete event set before the first
publication, not after one chunk or only on phase termination. Partial-chunk
progress/owner lifetime and native-ring failure propagation remain open.
No new production/runtime changes, acceleration or DMA-safety claims.

Depth resolve shared admission: reviewed complete resource submitDepthResolve
at 0x7415e (0x3f0), pinned both payloads, exact selected duplicate helper
addresses, native submit edge and storage/free imports. It initializes two
event vectors and requests capacity 4 at 0x741eb/0x7420b without testing AL.
Depending on resource flags it collects resource/storage events through the
already reviewed private helper at 0x7430e/0x74331. No aggregate collection
failure status gates subsequent native depth submit at 0x744d7. Selected
resolve bits and +0x64 can then be cleared, depending on resolve-type flag.
Temporary vectors are freed through IOFree at 0x74518/0x74534. No local
accelerator mutex/OSObject owner retain is present; effective caller locks
remain separate required review. The submission callee is identified, not
newly reviewed as a whole body or certified to complete GPU work.

This extends the actual omission graph beyond CCS: a CCS-only collection
patch would leave depth resolve unchanged. A shared repair must preserve
event completeness, binding/prepare cleanup, selected per-plane progress and
outer compression-state semantics across both entrypoints. Enlarging capacity
alone cannot handle initial allocation null, and waiting the admitted aggregate
cannot recover events never appended. No new production/runtime change here;
the implemented rectangle-null repair remains distinct from this open problem.

Bind/preparation pairing: reviewed complete native bindResource (0x48), KC
checkDirty (0x144) and addToChannel (0x22a); pinned bodies, native checkDirty
import, prepare-result branch and channel dispatch, plus declared base channel
virtual. bindResource ignores dirty-query return, invokes resource prepare
+0x170 and returns false immediately on AL false. Only prepare success calls
addToChannel +0x190 with access 1 and cached client channel +0x170, then returns
true. color_resolve's false bind path goes to mutex/busy cleanup, while its
post-bind paths use removeFromChannel and concrete complete. Do not insert
post-bind cleanup on a path that never acquired those states.

checkDirty can inspect mapping installed/ownership flags, update generation/
dirty metadata and call subordinate storage/surface helpers; its result is
dirty-state classification, not an admission result consumed by bindResource.
Subordinate helper bodies remain unreviewed here. addToChannel increments
resource +0xa0 before optional storage delegation, initializes/ORs access
+0x98 and records channel id +0x9c when first admitted. These counters are
separate from resource prepare +0x28 and are not OSObject retains or GPU ACKs.
Both add/remove paths have storage recursion; full effective lifetime and
subordinate event callback closure remain pending. No new production/runtime
change, and no claim that the existing prepare body was newly reviewed.

Borrowed lookup and cleanup identities: reviewed full Shared2::lookupResource
(0xe), Namespace::lookupId (0x2a), and Resource2::removeFromChannel (0x14c)
in paired archived KC. Shared wrapper tail-calls namespace lookup. Lookup
checks id < capacity, reads table pointer, rejects null, writes output and
prefetches before returning true; it does NOT retain, increment preparation,
lock or clear output on failure. color_resolve initialized its output to zero
and holds accelerator mutex while accessing the resulting borrowed resource.
No asynchronous lease can be inferred from lookup's Boolean success.

Declared native IGAccelResource +0x198 imports removeFromChannel, while +0x178
targets the already reviewed concrete IGAccelResource::complete; base KC
resource +0x198/+0x178 identities are also pinned. removeFromChannel updates
channel events through channel +0x140 for mapping/resource/storage conditions,
may recurse through another resource's +0x198 or call a subordinate helper,
then decrements resource +0xa0 and resets packed +0x98 when zero. It is not
a generic OSObject release, GuC ACK or simply dropping a retained lookup ref.
Subordinate callbacks and preparation pairing remain their own obligations.

This rules out unlocking during a new wait on the assumption that lookup
owns its returned resource. A valid owner lease must be acquired while the
effective admission lock still protects the pointer and include backing/task/
table/pool requirements; OSObject retain alone does not prove those domains.
No new production/runtime changes or full indirect lifecycle closure claimed.

SharedUserClient effective lock: reviewed complete color_resolve at 0x7899c
(0x5d0), pinned both payloads, lock/busy/lookup imports, CCS call edge and
error-mapping anchors. After size 0x30, feature and nonzero-resource checks,
the body loads client +0xf8 accelerator, locks its +0x88 at 0x78a0f and marks
busy at 0x78a1f. It does resource lookup/validation, preparation and selected
CCS resolve at 0x78f23 inside this scope. AL false maps to 0xe00002c2 and
true to zero at 0x78f28..0x78f32. Resource virtual cleanup +0x198/+0x178
then joins notifications, unlock_busy at 0x78e30 and mutex unlock at 0x78e3c.
Early request rejection outside that scope goes directly to the return path.

This establishes one concrete lock enclosing mapping/cache/event mutation and
submission, rather than inferring unlock from nested bodies. It does NOT prove
resource lookup retains backing, cleanup virtual semantics, every concurrent
owner destruction, Metal/render/depth locking or firmware quiescence. Inherited
stop cleanup previously reviewed also uses accelerator +0x88, but that alone
does not close all ownership paths. A new synchronous retirement wait must
account for this held mutex and downstream event waits; indiscriminately
dropping it could invalidate borrowed resource/task pointers. Effective callback
lock ordering and all cleanup variants remain required. No production/runtime
change; native targeted contracts pass both payloads.

Outer resolve state follow-up: reviewed complete plane-selecting resource
submitCCSResolve at 0x74078 (0xa6), and enableRenderCompressionWithAccelTask
at 0x73774 (0x2ac), pinned both payloads and selected edges/state anchors.
The plane wrapper iterates six groups of selected plane bits, calls the
previously reviewed per-plane resource function at 0x740d8, ANDs AL into r13b
at 0x740dd, reloads resolve bits and continues after false. It returns aggregate
Boolean failure without undoing already successful planes. Independent-plane
continuation is not itself classified as a hardware bug; callers must interpret
partial progress and preserve unresolved state correctly.

Compression enable can map two auxiliary ranges through task +0x278; it tests
both mapping returns, and second-map failure can release the first range. After
successful setup it sets ResourceInfoEntry +0x91 flags 0x1/0x2 (0x739a4/0x739af)
BEFORE the conditional selected resolve at 0x739d6. That call's AL is not locally
tested, and the epilogue loads the stack canary into RAX; do not assume this
enable function returns the resolve Boolean. A new event-admission failure
return alone cannot establish outer compression/resolve-state consistency.
No local mutex/owner retain appears in these two complete bodies; effective
caller serialization remains unproven. Auxiliary map/release callees were
identified only, not newly whole-reviewed or declared quiescent.

Direct bounded inventory also found selected resolve invoked from Metal render
and SharedUserClient color_resolve, while the private event append helper is
used by depth resolve as well as CCS. Those larger callers and effective
locking/failure propagation remain required work. No new production/runtime
change; the rectangle-null repair stays separate from event admission and DMA.

Event-result semantics follow-up: re-read the already pinned complete Tahoe
Fast2::mergeEventExcluding (0x1d4). Its final call at 0x14b96798 is virtual
+0x190 testEvent on the DESTINATION aggregate, after the source's eligible
stamps were merged. Return false therefore means that aggregate is not yet
complete, NOT that merging failed. Added an epilogue anchor to retain this
distinction. The last merge completion query includes earlier aggregate
members; do not introduce a Boolean failure accumulator from those AL values.
This does not recover source events silently omitted before merging.

Reviewed/pinned barrierForWaitEvents (0x52) and the selected finishEvent
instrumentation wrapper (0x70), plus imported merge/barrier/base-vtable/reporter
identities in both payloads. When aggregate completion is false, the barrier
helper invokes event-machine +0x1f0 to emit an event barrier; false emission
tail-calls finishEvent on aggregate accelerator +0x11e8. Target 0x15e90 is
finishEvent, NOT finishAllStamps. Its third argument 0x2b is a statistics
bucket: wrapper invokes base Fast2 header-table +0x198 (object slot +0x188),
then IOSimpleReporter::incrementValue and bucket accounting. It is NOT a
channel mask or independent whole-device quiescence. This effective graph
corrects the exploratory target assumption before any production change.

Only events admitted to the aggregate are covered; allocation-skipped events
are not magically recovered by this fallback. The existing Fast2 completion
termination bypass and outer task/table lifetime obligations also remain open.
Targeted native and paired-KC contracts pass; no production/runtime change.

Implemented bounded VF CCS rectangle-null repair (not deployed): the exact
34-byte result-setup block at 0x73c1a now uses a near JE to 0x73f13, retains
the allocation pointer and r12 on success, and stores byte 1 into count/capacity
whose complete qwords were previously zeroed. This preserves original full
qword values without widening the body. Removed RCX=1 is dead: native code
first overwrites RCX at 0x73cd9 before reading it or calling another function.
The remaining instructions and all cleanup/import sites stay at their original
addresses. Existing UUID/VF gate plus solved start/end bound 0x554, unique
allocation/zero patterns and selected false-cleanup anchor fail closed before
apply. The archived binary remains unchanged; its bad-branch fixtures preserve
original evidence rather than claiming the archived code itself was repaired.

Full offline suite passed in /tmp/ngreen-static.wgZbXe (two existing SDK macro
warnings). Tests cover both payloads, 68 single-byte preflight mutations,
duplicate/reapply rejection, untouched neighbours, null destination/empty-state
and selected success-setup equivalence. Separate actual x86 Unicorn execution
verified five non-null pointers including high-bit/maximal values, identical
vector/r12/flags, null branch to cleanup and Capstone RCX liveness. These are
selected CPU setup checks, not whole-kernel or firmware/DMA proof. Event append
failure propagation and cross-owner PPGTT transaction are STILL unimplemented.
No native macOS build/artifact promotion or runtime deployment/VM start claimed.

CCS cleanup verification: relocation identifies the 16-byte rectangle request
as IOMalloc (0x73c16); normal rectangle cleanup is IOFree (0x73ed3).
Both failure-path vector frees are IOFree (0x73f2b/0x73f47), sized by vector
capacity * 8. False cleanup at 0x73f13 releases the two preceding event
vectors and reaches AL=0 at 0x73f4b; it does not submit or clear resolve state.
At rectangle-allocation null, its pointer/count/capacity are still zero and
need no additional free. This establishes a candidate null-failure destination,
not an implemented patch. The 2-byte JE cannot be widened by overwriting its
following instructions without rebuilding/verifying that bounded setup region.

Reviewed complete event-pointer vector grow at 0x757b2 (0x78) and
AddDstResourceEvents at 0x7582a (0x172); full bodies/imports pinned in both
payloads. Grow returns false without modifying its vector on allocation null,
and also returns false when requested capacity is already sufficient. AddDst
checks each growth result, skips a failed append, and continues other event
append attempts; it returns no aggregate admission status. Initial event-vector
growth results in the CCS caller are also not checked. Therefore repairing
the rectangle null write alone does not establish complete dependency-event
coverage or safe submission under allocation failure. This separate propagation
requirement must remain open; do not mistake CPU fault removal for DMA safety.

Resource CCS caller follow-up: reviewed complete resource submitCCSResolve
at 0x73a20 (0x554) and pinned its downstream edge 0x73e99. After that call,
the body clears selected resolve bits in ResourceInfoEntry +0x4c and sets
+0x64 to zero at 0x73eb6, frees temporary vectors, then returns AL=1 at
0x73f0f. There is no local downstream submission-result test on this path;
other preparation/error paths return false. No local owner retain/mutex is
visible. This does not certify effective callers or asynchronous completion.

UNREPAIRED native allocation failure: r12 is zeroed at 0x73bf8, the one-element
rectangle vector starts empty, and allocation requests 16 bytes at 0x73c10.
The null test at 0x73c1a jumps from 0x73c1d to 0x73c3c, skipping the only
r12=allocation store at 0x73c39. Following arithmetic reaches [r12] store
0x73d08 without another r12 null test; null allocation can therefore cause
a CPU null write instead of a clean preparation failure. Whole-body and bad
branch/store anchors are evidence of the archived defect, NOT a correctness
gate. No failure injection or historical panic attribution is claimed.
Before patching, imported allocation/free identities and alternate false-return
cleanup must be checked to avoid leaking the preceding resource vectors.

CCS submission follow-up: reviewed complete submitCCSResolve at 0x2c84c
(0x2b0), pinned its full body in both payloads, resource cache update edge
0x2c94b, resolve_ccs assembly edge 0x2cad0 and external submitBuffer relocation
0x2cad9 (instruction 0x2cad8). The resource cache request is type 2, conditional
on non-null resource. It is followed by debug/bookkeeping branches, FIFO
virtual operations, optional event-resource append, resolve assembly, then
command-buffer submit and clearing the bookkeeping pointer +0x1860 -> +0x48.
No local result test or rollback connects cache update to downstream submit.
External call placeholder 0x2cadd is NOT a self-call; relocation identifies
IOAccelCommandBufferPool2::submitBuffer. No local IOLock acquisition is in
this complete body; effective outer caller locks remain unproven. Previously
reviewed barrierForWaitEvents is not a newly reviewed full body here.

Function-bounded scan of defined native text owners found 87 direct edges to
the resource cache updater or video wrapper across 29 owners. Routes include
submitBlit, submitCCSResolve, blit rectlist, Metal render/posh/blit/compute,
GL binding/indirect state and media token processing. This scan excludes
external branch relocations and does not establish all indirect routes or
whole review of those owners. Consequently a video-only admission hook cannot
cover the observed mutation graph. Repair requires shared owner admission,
cache-state rollback and pre-submit failure propagation across pipelines.
Runtime safety, DMA quiescence and Metal/media acceleration remain unproven.

Resource follow-up: complete updateMappingCacheType at 0x751a6 (0x30)
sets resource +0x108 bits 25..26 from requested type & 3 before borrowing
mapping +0x40. Non-null mapping tail-calls updateCacheType at 0x751cf;
null mapping returns with resource flags changed. Complete video-context
updateResourceMappingCacheType at 0x780e6 (0x24) tests accelerator +0x1184
bit 0 through context +0x5a8, then tail-calls resource update with type 1 at
0x78105. Neither body locally retains/locks owners or handles a failed update.
Both full bodies and tail edges are pinned; effective outer admission remains
unproven. Resource flags must also participate in eventual rollback consistency.

Exploratory whole-text Capstone decoding stopped at invalid bytes, so its empty
cache-caller result was discarded, not used as absence evidence. Repeated
function-bounded scanning found the resource tail edge and additional blit/video
callers. Large caller disassembly output was truncated; those bodies are NOT
claimed as wholly reviewed. Same-offset indirect calls on unrelated classes
are not classified as mapping dispatch without receiver identity proof.
No production changes or dynamic GPU test this checkpoint.

Follow-up: reviewed complete IGAccelMemoryMap::updateGPUPageTable at 0x11444
(0x140) and updateCacheType at 0x1159c (0x24); pinned both payloads, declared
mapping virtual +0x180, manager direct edge 0x114f5 and cache-store/tail-dispatch
anchors. updateGPUPageTable returns success immediately for flag 0x20; otherwise
it passes borrowed mapping task +0x90 and accelerator +0x88 -> manager +0x1260
to updatePageTableForTask and returns its result. No local owner retain or mutex
appears. Direct-call inventory found this manager caller in native text after
excluding external relocation placeholders; indirect callers are not exhausted.

updateCacheType returns if +0x114 already matches; otherwise it stores the new
type BEFORE testing installed flag 4 and tail-dispatching virtual +0x180. This
body has no result test, retry or restoration of the prior cache type. Thus a
false manager result can leave software cache type changed and successful
earlier segments/tables already remapped. This conditional path is not proof of
a reproduced failure, nor proof that effective outer callers lack serialization.
The outer cache-type request domain, overrides and rollback semantics still
need review; do not repair by returning fabricated success or by fencing only
unmap. No production/runtime/Host GPU mutation this follow-up.

Reviewed complete updatePageTableForTask at 0xf7bc (0x11a) and updateRange
at 0x14580 (0x3c4); added whole-body fixtures for both native payloads.
Manager obtains mapping range through virtual +0x128/+0x168, borrows task
+0x268 list, calls updateRange at 0xf865 and ANDs each AL into r12b at
0xf86a. It continues to the next raw node even when AL is false; empty-list
result is true. There is no local task/table retain, lock, rollback or ACK.
This is a local-body claim, not proof that every effective caller is unlocked.

updateRange constructs descriptor/segment iterators, uses descriptor virtual
+0x20 references and releases through +0x28 before its flush notification.
Those references are not a demonstrated retain of the task, table or page pool.
Its per-segment remap virtual +0x128 at 0x1483d is tested at 0x14843;
false exits through 0x14892 without locally undoing previous successful
segments. Cleanup calls the previously reviewed deferred-flush helper 0x2d1d8
at 0x148d6 on both success and failure. Notification is not a GuC completion.
External relocation placeholders in tracing/memory helper calls are not treated
as actual recursive targets.

Consequently the proposed retirement domain must also exclude/admit updates,
preserve all affected owners before mutation, and account for partial segment
and cross-table updates. A release-only fence or holding iterator descriptors
does not establish this. No production patch, hardware failure injection or
historical Host crash attribution is claimed; containment hold remains active.

Concrete mapping VA-free follow-up: existing full commit/release/free bodies
and +0x170/178 slots were already reviewed/pinned; do not count them as new.
The previously unpinned Intel freeGPUVirtualAddress 11080/10a is now fully
reviewed. Its declared mapping virtual +0x160 resolves to this override. It
does optional diagnostics around explicit base-vtable +0x170 delegation
(header-inclusive base +0x160) at 11116. The already pinned c8130 import is
the base IOAccelMemoryMap vtable. Thus the real declared Intel path reaches
the reviewed base list removal/address return/identity clear operation; it
does not add a GuC wait or hardware-retirement boundary before VA reuse.

Combined with the established releaseFromGPUPageTable -> manager fan-out ->
releaseRange -> unmap/shrink -> page descriptor/pool recycle graph, this
rules out fixing retirement solely in the outer VA-free wrapper: physical
page zero/reuse may have occurred earlier. Likewise merely delaying page-pool
free-list publication does not preserve the already-zeroed page contents or
the subsequently returned VA reservation. A correct integration must acquire
explicit owner references before hierarchy unlink/zero, serialize affected
mapping admission, exclude conflicting submission, and retire all affected
table/VA/backing owners only after real firmware completion. Exact transaction
and destruction coverage remains unimplemented, not proven by this graph.

One new whole body and concrete VA-free slot/base-dispatch anchor pass in both
payloads. Review priority remains cross-owner retirement, not generic memory
allocator investigation. No production hook, deployment or Host operation.

Stolen purge disposition: complete setPurgeable fda2/2a, pool allocate
c876/a8, deallocate c91e/8e and descriptor withSubRange fc90/88 reviewed.
Allocate obtains a range, creates the typed stolen descriptor, updates counts
and links it into the pool chain; descriptor factory success stores the pool
pointer and initial state 2. Failed factory creation returns the range.
The declared descriptor +0x120 slot resolves to setPurgeable. It accepts
states 2..4 by storing descriptor +0x78, permits state-query operation 1,
optionally returns the previous state, and rejects other values. It does not
release backing, modify GPU PTEs, call the pool allocator or issue/wait for
GuC invalidation. Thus the reviewed sleep purge's operation 4 is metadata,
not a backing retirement or DMA-completion barrier.

Actual pool deallocate is distinct: it returns address/length to the allocator,
decreases byte usage, checks intrusive-link consistency and unlinks the
descriptor. Its outlined invariant-failure callees and final descriptor
destruction are not newly reviewed here; this body contains no local GuC
acknowledgement. No malformed descriptor, allocator failure or runtime race
was reproduced. The raw chain and borrowed pool pointer still require outer
ownership provenance; fixed declared class identity is not a dynamic inventory.

Four complete-body fixtures, the declared purgeable slot and typed factory
edge pass for both payloads. This resolves the selected purge question without
pretending it solves cross-owner PPGTT retirement. Return review priority to
mapping admission/transaction owners and actual invalidation-before-reuse
integration; no executable source hook or hardware action was changed.

Power subordinate follow-up/correction: reviewed complete stolen-memory
pool purge c9ac/32, base scheduler sleep 56844/52 and wake 56896/6, and
Scheduler4 forwarding bodies 1db5c/12 and 1db6e/12. Bridge sleep/wake
4ab14/2c and 4ab40/a were previously checked by selected lifecycle edges;
their complete-body hashes are now added as well. Seven whole-body fixtures
are new, not seven newly discovered runtime routes.

Corrected the preceding Intel sleep description: c9ac is stolen-memory purge,
not bridge shutdown. The actual order is scheduler sleep, stolen-pool purge,
base accelerator cleanup, then interrupt-bridge sleep. Purge walks a raw chain
and dispatches virtual +0x120 with operation 4; that callee's disposition and
outer lifetime still need provenance. Base scheduler sleep locks +0x440,
cancels/disables timer +0x448, clears +0x450 and unlocks; Scheduler4 forwards
to those base vtable slots, and base wake is a no-op. These selected bodies do
not themselves send a GuC disable or wait for GPU DMA retirement.

Bridge sleep invokes finishAllStamps with mask 0x2b before tail-dispatching
its established disable method. Wake tail-dispatches enable. Stamp completion
and IRQ disable do not independently prove TLB invalidation or all native
page-table users retired, especially under the already documented termination
stamp bypass. No hardware/panic attribution is made. Two explicit Intel
purge/bridge call-edge checks now prevent repeating the ordering confusion;
both payloads and negative source contracts pass. Next trace purge disposition
and finishAllStamps dependencies against native mapping retirement. No runtime
or production hook changed, and Host containment hold remains.

Effective sleep caller follow-up: complete system_will_sleep
(14ba6144/25c) and IntelAccelerator systemWillSleep (28916/64),
systemDidWake (2897a/62) reviewed/pinned. In the true-mode system_will_sleep
branch, accelerator +0x88 IOLock is acquired before virtual +0x9d8 dispatch
and released afterward. Dispatch occurs only when feature c78 bit 0 is clear;
the routine publishes sleep-related flags/timer cancellation afterward.
The false-mode path first calls display methods outside that mutex, then locks
for orphan-pool cleanup. These different scopes must not be collapsed into
a universal power-event lock assertion.

The declared Intel +0x9d8/+0x9e0 slots resolve to the reviewed Intel overrides,
not directly to base methods. Intel sleep dispatches scheduler +0x118 and
calls stolen-memory pool purge before explicitly invoking the imported base
accelerator vtable +0x9e8 (header-inclusive +0x9d8). It then invokes another
interrupt-bridge sleep. Intel wake resets selected bookkeeping, invokes
engine/scheduler subordinates and delegates through base +0x9f0. The import
at c81a8 is verified as the base accelerator vtable, and the corresponding
KC base sleep slot resolves to systemWillSleep. This connects the recorded
iterator mismatch to the fixed Intel/base power graph, not to an observed
guest suspend or Host fault. Scheduler/bridge/engine quiescence at these
boundaries is not established merely by their method names.

Contracts add the three complete bodies, declared Intel power slots, base
vtable import/slot, four mutex edges and in-scope sleep dispatch. Paired KC,
both Intel payload and negative source contracts pass. Next review the
effective power subordinates and remaining mapping admission before changing
sleep cleanup or introducing acknowledged page-table retirement. No runtime
or production modification; suspend containment remains mandatory.

Sleep/wake follow-up: complete systemWillSleep (14ba5eb2/190),
system_did_wake (14ba63a0/20c), TaskList iterator constructor
(14b820d8/c) and getNextTask (14b820e4/16) reviewed/pinned. The true-mode
wake branch acquires accelerator +0x88 IOLock before optional orphan task
release/active-task pruning and unlocks afterward. Its false-mode display
branch does not acquire that mutex or perform the same mapping cleanup.
Sleep has no local accelerator lock; effective caller locking remains open.
These paths are not interchangeable quiescence predicates.

A concrete archived native sleep defect is now recorded, not repaired:
the active-task iterator at rbp-0x30 is consumed to null. The following
orphan-task iterator is initialized at rbp-0x48 (5f7f), but the next fetch
uses rbp-0x30 (5f9a), and its loop alias also uses rbp-0x30 (5fab).
The reviewed constructor stores only the selected list head at [rdi];
getNextTask reads that slot and advances it to task +0x18. Thus this second
loop reads the exhausted first iterator instead of the new orphan iterator,
skipping its cleanup in the pinned native reference. It is not evidence that
any actual guest suspend or Host hang traversed this path.

The cleanup success checks pin those wrong stack displacements explicitly as
an UNREPAIRED defect. A green fixture does not certify sleep correctness.
Both iterator displacements would need consistent repair, but adding a KC
patch before effective power lifecycle, lock admission and DMA quiescence
are reviewed could activate previously skipped unsafe cleanup. No runtime
patch is added here; do not relax Host suspend containment based on this
finding. Four new whole bodies, five cleanup/lock edges and three defect
anchors pass paired-KC checks. Next resolve effective sleep callers and
retained owner admission before implementing safe retirement integration.

Shared release caller follow-up: complete SharedUserClient2 clientClose
(14b907b2/54), free (14b90238/6a), stop (14b902ec/f4) and sharedStop
(14b927bc/46) reviewed/pinned. In the active +0x108 stop branch, it captures
accelerator +0xf8, acquires that accelerator's +0x88 IOLock, balances busy
state/notifications, retains the accelerator, then invokes virtual +0x998.
The declared user-client vtable resolves +0x998 to sharedStop. That method
clears its owned shared object's +0x110 association, invokes object release,
clears client +0x100 and removes client list membership. Stop subsequently
detaches/clears accelerator storage, releases the client, unlocks the captured
accelerator mutex, then releases its explicit accelerator retain. Thus this
selected shared-object release occurs inside an established mutex lifetime.

clientClose instead marks closure and samples outstanding +0x160 operations
under the client +0x158 IOLock. It unlocks before optional virtual termination
and returns success even when outstanding operations defer termination. That
client mutex is not the accelerator mutex and close success is not teardown
completion. Client free disposes its client lock and other owned objects but
does not locally acquire the accelerator mutex or explicitly free Shared.

These bodies and declared slot prove one ordinary stop route's lock scope,
not every actual shared-object class, last reference or user-client override.
Other release paths and external admission must remain covered by the future
retirement transaction. Four whole bodies, one declared stop identity, four
mutex edges and two stop scope anchors pass offline. Next connect sleep/wake
and remaining mapping admission callers; no production/runtime change.

Shared teardown follow-up: complete Shared2::free (14b8e71e/2be),
OrphanedMemoryPool::sharedRelease (14bb8404/9a) and Resource2::sharedRelease
(14b868cc/cc) reviewed/pinned. The declared resource virtual +0x160 resolves
to the reviewed sharedRelease body. Shared free first walks its resource list
and invokes that operation, then notifies two accelerator orphan pools. It
releases other owned objects/arrays before pruning mappings on its task +0x88,
invoking task virtual release, and clearing that field. Thus its task pointer
cannot be borrowed by asynchronous retirement after Shared destruction.

Resource sharedRelease first validates its stored table entry's identity,
removes that entry and Shared list membership, then releases selected owned
objects and finally itself. Orphan-pool sharedRelease traverses two raw node
lists with next-node snapshots, matches memory +0x30 against the dying Shared,
and either clears that association or removes/updates/re-adds memory according
to helper policy. Subordinate memory disposition helpers remain uncertified;
these list operations do not prove firmware ownership ended.

None of the three bodies locally acquires accelerator +0x88 IOLock. This does
not prove an unlocked effective caller: Shared free could run under a caller's
release lock, and its distinct +0x80 field must not be confused with accelerator
mutex storage. Collector locking already established for other paths is not
automatically inherited here. The required retirement owner must independently
retain task/mapping/allocator/page-pool resources and cover Shared destruction,
not merely store the Shared task address until a later worker executes.

Contracts add three whole bodies, one declared resource vtable identity,
three direct Shared cleanup edges and task release-before-clear bytes. Targeted
paired KC checks pass; no production change or hardware validation. Next
resolve effective Shared release callers and sleep/wake lock scope before
choosing retirement interception points.

G2H completion lock dependency follow-up and offline production repair:
rechecked vfInvalidateTLBSync, vfSendCtbFastAction, vfCanWaitForGuc,
vfDrainGuCToHost and complete vfCtbGucToHostAction. The synchronous TLB
wait holds gVfGucLock, submits under native H2G +0x18, then directly polls
the independent consumer under G2H +0x20. Its event application uses atomic
credits/TLB sequence and the context spin lock; it does not acquire the
accelerator +0x88 mutex, H2G mutex or gVfGucLock. This selected call-chain
observation does not prove all external owner/teardown lock dependencies safe.

Found a concrete source ordering hole: the consumer previously unlocked G2H
after publishing descriptor head but before applying lifecycle/credit/fault
state. An IRQ consumer and synchronous poll could dequeue adjacent events in
firmware order yet apply them in reverse order. The context spin lock only
serialized application, not its order relative to dequeue. No observed Host
panic attribution or reproduced hardware race is claimed.

The VF consumer now uses a noncopyable scoped ConsumerTransaction holding
the existing G2H mutex from frame read through final event application and
header normalization. All returns release it once. A locked readiness recheck
rejects a queued consumer admitted before its predecessor set protocol fault.
Fault publication now precedes unlock on malformed/unsupported events. No new
lock object or PF path is added; the consumer contains no synchronous send,
sleep, nested poll or invalidation wait. The existing context-spin interaction
remains a lock-order obligation for callers and future hooks.

The already-inspected native software handler 1f9a0/a4 is now whole-body
pinned in both payloads. It invokes the routed reader and interprets its
normalized short header without acquiring these producer/accelerator locks.
Three negative source mutations (guard removal, premature unlock, missing
locked admission) are rejected. These are structural regressions, not a
runtime concurrency/DMA proof. Full offline suite passed at production fix
in /tmp/ngreen-static.YIaufE with the two existing SDK macro warnings;
the added native-body fixture also passes targeted paired-payload checks.
No artifact was deployed, no VM started, and Host containment hold remains.

Task allocator follow-up: complete allocate 14b9e2a2/c8, deallocate
14b9e36a/6e and init 14b9de06/138 reviewed/pinned; declared Task +0x140/148
vtable identities resolve to these methods. Init retains each supplied nonnull
IORangeAllocator and stores it in task +0x168's indexed array, with limit/free
accounting arrays initialized alongside it. This demonstrates allocator owner
references, not allocator option settings or exclusive task ownership.

Allocate selects the allocator using mapping +0x78, dispatches ordinary
allocate (+0x150) with mapping length/alignment or fixed allocateRange (+0x158)
with requested address/length under flag 0x4000, and increases indexed usage
only on success. Failure returns zero. Deallocate recomputes the applicable
length, dispatches allocator +0x160 with the saved address, then decreases
indexed usage unconditionally. Neither method locally locks the accelerator
or issues/waits for GuC retirement. Index and init-count provenance still need
caller validation; absence of local checks is not proof of malformed inputs.

Read the entire local XNU 12377.121.6 IORangeAllocator.cpp (400 lines) and
header (171 lines). Source SHA-256 respectively
6a01112b12cb82651b238440c3c63176ce0546ad672831e0d33532d835bb1708 and
2dc8ef8a4af34218317feae5d704a77204fd364ab7c9bae89e5d52e29a9f762e.
That reference optionally locks with kLocking (default factory options zero),
allocates from free fragments, and deallocate merges/inserts ranges directly;
it contains no GPU completion mechanism. This is a protocol comparison, not
proof of concrete runtime allocator class/options or exact KC equivalence.
Even a locking allocator would serialize its free list, not establish GPU
quiescence. Concrete allocator factory policy and completion-consumer lock
dependencies remain pending. Three new whole-body and two vtable checks pass
offline; no production/runtime changes.

Factory/VA follow-up: complete createMappingInTaskAtAddressLength
(14b67304/272), allocGPUVirtualAddress (14bb7616/6a),
reserveGPUVirtualAddress (14bb75ae/68) and freeGPUVirtualAddress
(14bb7680/6c) reviewed/pinned. The factory searches the parent's raw mapping
array for the requested task/options. A nondeferred match is retained; a
deferred match is reactivated by moving it from task +0x200 to +0x1e8 and
clearing flag 8 without a new local retain. This preserves the deferred
reference rather than creating an independent owner. Neither path locally
acquires the accelerator mutex. Actual factory caller serialization remains
unresolved, including virtual callers not found by a direct-edge locator.

For new mappings the accelerator virtual factory is followed, for the relevant
flag-2 path, by fixed-address reserve or ordinary VA allocation. Failed ordinary
allocation either invokes task pressure recovery or, under c92 bit 6, prunes
orphans, retries, finishes/frees orphans, then retries again. Exhaustion invokes
mapping release and returns null. These recovery stages can therefore trigger
the previously reviewed event/termination cleanup policy inside VA allocation.

Ordinary VA allocation, unless already flag-1 allocated, calls task +0x140
unless flag 0x20 selects the no-allocation path; zero fails in the ordinary
allocator path. Success adds the mapping to the active task list, records the
address, and publishes flag 1. Fixed reservation writes address/length and
flag 0x4000 before task +0x140; only an exact returned-address match publishes
active-list membership and flag 1. This local body is not a rollback proof for
the task allocator's mismatching-address policy.

freeGPUVirtualAddress removes the mapping from the active or deferred list
according to flag 8, then (unless flag 0x20) calls task virtual +0x148 with
its saved address. It clears address/length and flag 1 afterward. There is no
local GuC acknowledgement before that address return. Therefore a proposed
asynchronous page-table quarantine must also preserve the VA reservation and
its task/mapping owners until safe retirement, or prove equivalent submission
exclusion; retaining only physical page descriptors/pool owners is insufficient
to exclude stale-translation aliasing after VA reuse. This is a required design
invariant, not a demonstrated runtime stale-translation event.

Contracts add four complete bodies, three declared mapping VA vtable targets
and five selected factory/list edges. Next resolve task allocator/free and
factory caller lock scope together with the completion-consumer dependency.
No production hooks or hardware tests were changed.

Caller follow-up: complete Task::release const (14b9e082/1a2), accelerator
free_orphaned_gputasks (14ba57d6/8c), kickOrphanResourceTimer (14ba579a/3c),
garbage_collector (14ba1280/ce) and gart_collector (14ba1376/1d4) reviewed.
Both collectors acquire the accelerator +0x88 IOLock before orphan-task
cleanup and release it afterward; the paired Boot import identities already
resolve these stubs to IOLockLock/Unlock. Busy counters/virtual notification
calls are separate from this mutex, not substitutes for it. This establishes
outer exclusion on these two fixed paths only, not every release caller.

Last Task release aggregates deferred mapping events into task +0x20 using
event-machine virtual +0x1b8, then tests the aggregate. A false test (or feature
c78 bit 3) marks task flag 1, moves it between accelerator lists +0xc48/c58,
kicks the orphan timer, and returns without base decrement. Otherwise it
cleans orphan mappings and visits active mappings; zero prepare count plus
installed flag 4 leads to finishEvent, release_pte and virtual +0x160 before
base counted release. free_orphaned_gputasks tests each task aggregate and,
on true, cleans its deferred mappings and invokes task virtual release. The
newly established event termination bypass therefore reaches task cleanup as
well; task event success is still not proof of GPU/TLB retirement.

Timer kick sets pending +0xe0 before invoking timer +0xf8 virtual +0x1d0 with
100, unless already pending or feature c78 bit 4 prohibits it. This schedules
work, not a synchronous drain. Neither Task release nor orphan-task cleanup
itself acquires a local mutex; collector ownership does not certify unrelated
mapping factories, Shared free, sleep/wake or indirect release paths.

Exploratory linear decoded direct-edge scan also found prune callers in the
mapping factory, Shared free and system_did_wake, and free-orphan callers in
Task release, orphan-task cleanup and systemWillSleep. This is a locator, not
an exhaustive indirect-call inventory. Next establish those effective caller
lock ranges and the GuC completion-consumer dependencies before holding
+0x88 across any new acknowledgement wait. Five new whole-body contracts,
eight direct edges and two mutex-field anchors pass offline. No runtime hook.

Follow-up: reviewed/pinned complete Task free_orphaned_mappings (7e),
prune_orphaned_mappings (82), freeAllGPUMappings (ce), mapping finish/test
wrappers (26 each), reverse iterator construction (e) and traversal (16),
Fast2 testEvent (3e) and testEventUnlocked (6c). The two owner lists are the
task's +0x1e8/+0x200 lists. Ordinary orphan cleanup, unless feature c78 bit 3
is set, traverses the deferred list, finishes each event and invokes mapping
virtual release. Prune tests each event and stops the entire pass at the first
false, otherwise releases it. freeAllGPUMappings instead releases deferred
entries directly, then visits the active list: flag 2 or nonzero prepare count
skips cleanup; otherwise installed flag 4 dispatches release_pte, followed by
virtual +0x160. These are distinct cleanup contracts, not one generic drain.

The formerly unresolved +0x190 predicate is accelerator +0x380's event-machine
testEvent, confirmed by the mapping wrapper and declared Fast2 vtable. It
dispatches testEventUnlocked (+0x180) and, on true, cleanEvent (+0x148).
Unlocked testing examines eight channel/stamp entries, skips channel -1,
compares stamps via signed subtraction, and refreshes cached completion from
mapped stamp memory when needed. Crucially, accelerator termination counter
+0xdc8 nonzero allows a still-outstanding stamp to pass. Thus mapping event
test success is not sufficient evidence of hardware completion during device
termination, and never substitutes for an acknowledged GuC TLB invalidation.
The existing termination-counter audit supplies field provenance; this newly
reviewed consumer connects that bypass to mapping cleanup admission.

Reverse traversal snapshots the predecessor before returning a borrowed
mapping, permitting serial removal of the returned entry. It does not retain
that predecessor or stabilize concurrent mutations. None of these task/list
bodies acquires a local accelerator lock. Effective caller exclusion and the
event finish/wait lock dependencies remain necessary before placing any
synchronous retirement hook. The contracts add nine whole bodies, three
direct cleanup edges, two Fast2 vtable identities, and termination-bypass
anchors. This is offline reference evidence only; no runtime fix/deployment.

Reviewed the complete previously unpinned IOAccelMemoryMap::release() const
at 14bb7364/fe, plus MemoryMapList removeMapping 14b81f24/7c, addMapping
14b81ebe/2a and parent check_orphan_state 14b66e6a/42. The declared mapping
vtable +0x38 (including its two header words) resolves to this release body;
this is a fixed class identity, not an exhaustive dynamic caller inventory.

Release first tests mapping flag 1 and virtual retain count == 1. Other cases
delegate to base counted release. The special path uses flags, a parent flag,
an unresolved manager virtual +0x190 predicate and accelerator feature bits
to choose immediate cleanup or deferred ownership. Immediate cleanup calls
release_pte only when installed flag 4 is set, then optionally virtual +0x160,
and finally delegates release. The deferred branch sets flag 8, removes the
mapping from owner +0x90's list +0x1e8, adds it to that owner's list +0x200,
and tests parent orphan state. It returns without base release or tail-calls
parent virtual +0x1c0 depending on that result. Therefore release is not an
unconditional final decrement, and a collector must preserve this owner policy.

Both list helpers manipulate raw next/previous pointers and counts without
retain/release or local locking. check_orphan_state counts flag-8 mappings in
the parent's mapping array and compares that count to virtual retain count;
this is an ownership comparison, not a GPU idle predicate. None of these four
bodies locally issues/waits for GuC invalidation. Existing outer locking could
still serialize them; its identity and acquisition remain to trace. No claim
of a reproduced race or use-after-free follows from these local observations.

The paired-KC contract now pins all four complete bodies, last-reference and
deferred flag anchors, the declared release vtable target, and four direct
cleanup/list/orphan edges. Next inspect manager +0x190 and the two owner-list
drain paths before deciding where a page-table retirement transaction may
safely hold owners and wait without blocking its completion consumer. No
production hook, VM deployment or Host GPU operation was made.

## Lower-level construction and concrete release caller follow-up

### Deferred AUX flag and ring command emission follow-up

#### Correction: indexed ring accesses do consume accelerator pending flags

##### Concrete reservation/emission vtable pairing

###### Complete waitForSpace reservation and failure ordering

Rechecked complete inherited sys-memory unwire 14bba228/1f8. It completes/
clears and returns the DMA command first, then visits mappings whose installed
PTE bit 4 is set and invokes release_pte. After that loop it completes the
memory descriptor and continues sys-memory unwired/purge-related bookkeeping.
New instruction anchors pin installed-bit admission and descriptor-complete
ordering, complementing the existing full body/cleanup-edge contracts.

No accelerator lock acquisition is visible in this complete body either;
its caller may already hold one. Command/descriptor complete virtuals must not
be interpreted as GuC GPU translation acknowledgements. Previously reviewed
error logging continues after command complete/clear failures; this follow-up
does not repair those failures or discover a new one. Likewise a mapping with
unpublished installed flag after partial commit can bypass this release loop,
so collector scope only at release_pte is not a complete commit-failure repair.
Mapping commit, rollback and final task/table destruction need one coherent
owner/lifetime policy. No production/runtime change. Next: effective unwire
caller lock contract and partial-commit owner admission, without treating DMA
command teardown as proof of GPU quiescence.

Rechecked the already-pinned complete inherited release_pte 14bb7488/80.
Its conditional virtual +0x178 invokes Intel release; the next instruction
reloads mapping flags into EAX, discarding the release result, then clears
installed-PTE bit 4 and performs parent accounting/generation updates. New
paired-KC instruction anchors pin the ignored result and flag clear. These
software transitions do not certify hardware invalidation completion. This
consolidates the existing overlapping instruction anchors and extends them
through the flag store, rather than discovering previously untested dispatch.

No local accelerator lock acquisition appears in this complete body. That
does not establish that callers are unlocked: sys-memory unwire and other
mapping owners may supply outer serialization, still to trace. A collector
cannot use a false manager/Intel return as an instruction to preserve backing
through this inherited boundary, because the result is discarded. Nor can
it treat this API as proof of an acquired lock. Retirement ownership must
intervene before irreversible page return and establish the actual outer
lock/admission independently. This confirms an existing failure-propagation
limitation rather than a new runtime fault. No production/runtime change.

Current source gate scope rechecked: VfContextOperationGuard is instantiated
by vfAttachContextDesc, vfDetachContextDesc and vfSubmitWorkItem. These three
typed entry points are now explicitly contract-checked. The guard enters/
leaves the context-operation count; shutdown closes admission and waits for
that count before sweeping direct contexts. This is the stated context
snapshot domain, not an automatically complete page-table or pool lifetime
domain. Native manager range release, task free, PPGTT free and descriptor
release are not made counted operations merely because GuC submission is
gated. Closing this gate alone cannot stabilize raw native task/table lists.

For retirement integration, define an explicit mapping/destruction admission
domain, stabilize task/table/pool owners before collecting releases, and prove
interaction with the existing context gate and IRQ/CTB completion consumer.
Do not wait for a count while the caller holds a lock required by counted
operations or completion delivery. The existing sleep-context checks are
necessary but do not prove that lock-order condition. This review identifies
why the current context guard cannot simply be reused as a global PPGTT
collector lock; it does not implement a new gate or certify full shutdown.
No production/runtime change. Next: actual native mapping/destruction outer
serialization and completion-consumer lock dependencies.

Retirement owner follow-up rechecks the already-pinned descriptor retain and
manager releasePagePool bodies. Descriptor retain only tail-calls OSAddAtomic64
on descriptor +0x28 with +1; it does not retain the owning IGPagePool OSObject.
The reviewed grow loop stores raw pool/block owners in each descriptor, now
explicitly anchored at b35e. Manager releasePagePool directly dispatches pool
release +0x28 at f3da, clears array slots and frees the owner array; it contains
no local pending-retirement drain. Existing external owner references may
prevent final destruction, but a descriptor counter alone is not one.

A collector must acquire both a descriptor/page reference before ordinary
final return and an explicit pool-owner reference while that owner is still
valid, plus any required task/table transaction references. Release order is
page descriptor first, pool owner last, after acknowledged retirement. Pool
lookup cannot rely on manager's array after teardown clears/frees it. A failed
transaction must retain both layers until proven safe containment/recovery;
dropping the pool reference while retaining only a descriptor would leave a
raw owner pointer without a lifetime guarantee. This requirement changes the
collector ownership design, not a claim of observed UAF or implemented hook.
No production/runtime change. Next: prove the acquisition serialization and
per-transaction ownership across task/table/pool final teardown.

Complete 32-bit PPGTT free 11dc4/80 is now reviewed/pinned with its concrete
free vtable slot, leaf/parent descriptor release edges and IOFree import.
It walks four inline directory records; for each present parent descriptor,
it walks 512 software child records, releases each present leaf descriptor,
frees the 0x4000 software array and releases the parent descriptor. It then
delegates inherited free. Unlike 64-bit free's shrink helpers, this selected
body directly returns descriptors and does not first rewrite parent PTEs.
There is no local invalidation acknowledgement or active-context exclusion.
Outer shutdown may provide additional guarantees not yet established.

The previously reviewed task mode chooses a 32-bit or 64-bit table, so the
retirement design must cover both rather than silently excluding an existing
native task mode. Software-record freeing here must not invalidate deferred
descriptor identities; retained physical backing and final owner references
must be independent of the soon-freed arrays. This is not a runtime UAF/crash
claim, and no executable patch or mode disabling accompanies this review.
Next: finalize descriptor identity/owner lifetime across both destruction
paths and map effective submission exclusion before retirement interception.

Complete 64-bit PPGTT free ccfa/1d4 and inherited private/base page-table
free wrappers 12c82/12 and 14028/12 are now reviewed/pinned. The concrete
64-bit free slot +0x90 is pinned. When the root exists, free walks 512-entry
hierarchy records, resets counts and zeroes software child-record arrays,
then invokes the already-reviewed shrinkLevel variants for leaf, directory,
pointer and root levels. A shared-marker branch skips leaf traversal, preserving
that separate ownership distinction. Four software memset imports are pinned;
zero-displacement calls are not interpreted as self-calls.

There is no local GuC invalidation/acknowledgement before these shrink calls
and final descriptor returns. The inherited wrappers merely delegate free
through their base vtables; selected bodies do not add a drain. Outer task/
context shutdown and other inherited ownership still require proof, so this
is not a claim that normal task destruction demonstrably frees live GPU pages.
It does establish a separate destruction path bypassing releaseRange: a
collector only scoped around manager range-unmap would not cover it. Physical
page clearing by pool return and software metadata clearing here are distinct;
retained descriptor records/parent ownership must survive whichever defer
strategy is chosen. No production/runtime change. Next: stable final-destruction
admission and shared descriptor references, then implement scoped retirement.

Complete IGAccelTask::free 7d4a/e8 is now reviewed/pinned. It first invokes
scheduler releaseSemaphoreWaitBuffers, releases/clears owned fields +0x280,
+0x288 and +0x278, then releases/clears private page table +0x260 before calling
releaseManagedPageTableList at 7ddd. It subsequently releases other task fields
and dispatches inherited base free. The private-table release and list cleanup
ordering are explicit contracts; previously reviewed list cleanup frees raw
nodes rather than retaining/releasing each table.

Consequently a retirement collector cannot carry borrowed task-list table
pointers across asynchronous task destruction without explicit retained owner
references and a proven exclusion rule. The selected free body has no local
invalidation acknowledgement or transaction-drain wait, but its scheduler/
owned-object/base callees and outer task teardown may have additional lifetime
requirements not yet proven. This is not evidence of an observed concurrent
UAF. Next: native private-page-table free and effective task teardown admission,
so page-return interception also covers destruction rather than only range
unmap. No production/runtime change.

Rechecked complete manager releaseFromPageTableForTask f6aa/112 and added
explicit fan-out edge/loop anchors. It captures mapping GPU range via virtual
accessors, walks task +0x268's raw list, calls releaseRange at f74e for each
table and ANDs results without short-circuiting. An empty list returns true.
Its selected body has no local table retain or lock acquisition. Existing
releaseRange unconditionally true does not represent acknowledged retirement.
These behaviors were previously documented; the new anchors make the concrete
transaction scope regression-checked, not newly discovered.

A candidate retirement owner must cover the complete affected-table fan-out,
not just a single leaf/table: acquire proven outer serialization and stabilize
the list/table ownership, prevent new relevant submissions, unlink affected
PTEs while retaining old page descriptors before any final return/zeroing,
perform the required invalidation and observe its matching completion, then
release retained pages. Failure must retain old tables/backing and invoke
actual Host containment rather than return into ordinary backing destruction.
Admission must not be reopened before the transaction finishes. These are
implementation requirements, not an implemented hook or tested runtime flow.

Manager-only post-call invalidation cannot meet the pre-zeroing requirement;
a collector must intervene before final descriptor/page return during the
fan-out. A single unowned global collector cannot establish per-task/thread
exclusivity or cover reentrancy, shared descriptors, bootstrap and unrelated
pool users. Next: effective mapping-release outer lock/admission and stable
task/table ownership before choosing collector storage/hook boundaries.
No production/runtime change; dynamic hold remains.

PPGTT retirement follow-up rechecks the already-reviewed bodies and pins
concrete ordering edges: leaf shrink writes the parent dummy PTE at cf0b,
decrements parent count, and calls descriptor release at cf1b. The descriptor
release atomic decrement's old-count-one branch tail-calls pool releasePage
at bb7d. releasePage calls imported memset at b894 on descriptor CPU page
before its optional pool lock or free-list bookkeeping. These edges and the
memset import are now explicit contracts in addition to complete-body hashes.
This is stronger ordering evidence, not a new discovery or runtime reproduction.

The lifetime intervention point must therefore precede zeroing, not merely
defer free-list reuse or pool pruning. A post-unmap invalidation cannot restore
page contents already overwritten while stale GPU page-walk references may
remain. A repair needs transactional ownership across table unlink and final
page return, covering shared descriptors and all affected address spaces;
its acknowledgement/transport-failure policy must retain backing until safe.
Intercepting every descriptor release globally without such ownership could
also block bootstrap or unrelated pool users and is not implemented here.
No production/runtime change. Next: map transaction boundaries and determine
where to retain/defer final page return before choosing a concrete hook.

The FIFO vtable +0x140 at d65c0 is an external relocation to inherited
IOAccelChannel2::setEventStamp, now pinned in both payloads. Its on-disk zero
is not a null runtime method. The event-machine +0x1e0 slot at cebe8 is the
already-pinned inherited writeStampCommand import. Thus the selected FIFO
sequence acquires/constructs stamp command data before reservation and publishes
channel event stamp after ring submission; these slots are not inferred lock
or hardware-completion methods. Existing paired-KC contracts review the inherited
stamp APIs, and their software stamp bookkeeping is not a TLB/DMA completion
certificate. This resolves two concrete dispatch identities, not full outer
locking or runtime owner identity. No production/runtime change.

Keep the remaining review focused on ring admission and PPGTT retirement:
do not expand stamp metadata traversal into an unbounded prerequisite audit
unless its concrete locking/lifetime edge changes the repair decision.

Complete FIFO submitRingCommands 4c5c0/106, alignRing 41bea/28 and ring
submitCommands 430aa/5e are now reviewed/pinned. FIFO skips disabled-feature,
null-buffer and empty-input paths; otherwise it obtains event/stamp information,
computes command plus alignment plus submit overhead, calls waitForSpace and
checks false before writing. On true it optionally aligns, emits commands,
invokes ring virtual +0x138, updates FIFO stamp state and invokes channel
virtual +0x140. There is no explicit local lock acquisition in the selected
body; invoked methods and outer owners must establish serialization. No claim
of an actual race follows solely from this absence.

alignRing repeatedly writes zero dwords until cursor & caller mask is zero,
without its own capacity reservation. submitCommands may emit a scheduler-mode
dependent prefix, invokes writeBuffer, then virtual +0x158 for stamp emission
and returns true without inspecting writeBuffer's result. FIFO likewise does
not inspect submitCommands's result. The already reviewed waitTimeout false
postcondition gap therefore reaches a checked FIFO path too: checking AL is
insufficient when the wait method returns true without proven available space.
This does not establish runtime timeout reachability or certify the stamp/
prefix callees. No production/runtime change. Next: outer FIFO ownership and
event/stamp allocation serialization, followed by bounded reservation/write
admission that preserves completion/backing semantics.

Further offline-only hardening checks native ring size equals the decoded
context control size, that the size is a power of two, and that its native
cursor mask equals size-1, before tail publication. The preceding identity
guard ensures ringObject is nonnull and its backing is the retained object;
the decoded size is at least one page. Complete initRingControl 7c4fe/62 is
reviewed/pinned: enabled state encodes configured KiB via page_shift and
(page-count-1)<<12 plus valid bit, then writes the primary context and optional
secondary context. The reviewed x86 4-KiB page configuration matches the
existing submit decoder; no new claim about other page-size architectures.
Two additional source mutations remove geometry checks, bringing the negative
guard tests to five. Geometry checks prevent inconsistent tail publication,
not earlier native CPU overrun or missing GPU invalidation acknowledgement.

VF submit now captures the registered retained ring backing under the context
lock while holding the existing queue ownership guard. Before tail publication,
it requires the hardware context's current ring backing to match that retained
object and requires its mapped length to cover the context ring-control size.
Mismatch marks a protocol fault and returns false without tail publication or
CTB submission. Existing alignment/control-valid/tail-below-size guards remain.
The source contract pins this guard and its pre-publication placement.

Follow-up negative tests remove backing identity comparison, invert the extent
comparison, or remove the mismatch branch's local false return. The third
mutation initially escaped the positional contract by matching the later
tail-validation false return. The contract now requires false within the
mismatch branch itself; all three in-memory mutations are rejected. This
strengthens source-regression coverage, not a formal control-flow/DMA proof.
No additional production behavior changed in this test follow-up.

This is an offline-only production change, not deployed or dynamically verified.
It prevents advertising a tail for mismatched/undersized backing; it cannot undo
earlier CPU writes by native writers and does not prove actual capacity,
completion, full ring locking or page-table retirement. Existing queue ownership
and retained context-record lifetime are the scope of the check; no new raw
global pointer or unowned backing is introduced. Native failed submit may panic,
which is still not Host DMA containment. Dynamic testing remains on hold.

Complete unsigned utilGetProperty 280a6/18c is now reviewed/pinned. The ring
configuration uses key RingSizeKB and default 0x20. The helper first queries
the supplied registry entry, then, only if that property is absent, its
Development dictionary. It accepts exact OSNumber/OSData metaclass matches;
wrong-type presence returns the default rather than trying Development.
Finally it checks IODeviceTree:/options, where an OSData string may be parsed
through OSNumber::withNumber with width 32 and override the prior value.
The temporary parsed number is released; the fromPath entry is not locally
released in this body (API ownership still needs separate verification).

The first OSData branch passes its reported length into imported memcpy_chk
with a four-byte destination size. There is no local exact-size test: a short
value partially overwrites the initialized default; an oversized value relies
on the imported checked-copy failure policy, not a safe fallback in this
helper. This is configuration parsing, not proof of malformed runtime input
or a reproduced panic. The copy/import identities are pinned. Later RingSizeKB
range/power-of-two validation still applies to successfully returned values.
Other utilGetProperty template instantiations and the entire populateAccelConfig
remain outside this complete-body review. No executable/runtime change;
next typed-property length/ownership policy and effective configuration ordering.

Selected populateAccelConfig window 275be..2760d now establishes the missing
configuration invariant: unsigned (KiB-4) <= 0x1fc and popcount(KiB) < 2.
Together these accept powers of two from 4 through 512 KiB inclusive, despite
the diagnostic string describing strict bounds. Invalid values are replaced
with 32 KiB. The window hash/owner bounds are pinned; a later firmware-mode
store 27cc8 explicitly overrides size to 16 KiB and is separately pinned.
These accepted sizes safely fit the getter's 32-bit left shift and supply
power-of-two masks and at least eight bytes when those reviewed assignments
govern initialization.

This is a selected configuration-window review, not a complete
populateAccelConfig control-flow audit or runtime size observation. An
exploratory displacement scan located these +0x119c writes but cannot exclude
derived aliases or other initialization paths. The earlier getter/init lack
of local validation is therefore not sufficient to allege malformed normal
configuration. Outer configuration ownership and ordering remain to verify.
No executable/runtime change. Next: full effective configuration ordering,
backing allocation bounds and ring-owner serialization; retain the independent
timeout-success and unchecked-writer issues rather than conflating them with
size validation.

Complete ring init 41414/1ec and context getRingBufferSize 7c560/14 are now
reviewed/pinned. Init stores a borrowed context, obtains accelerator through
context +0x58's object +0x10, copies engine ID, obtains a context resource CPU
address and writes its initial head. It stores size at ring +0x8c, size-8 at
+0x88 and size-1 at +0x90; head/tail, readiness and prefix state are cleared.
It allocates the ring backing via accelerator +0x150 using the size getter;
allocation failure returns false. Later scheduler setup failure releases and
clears that backing before false. Some setup success calls an external helper
whose zero-displacement placeholder is not treated as a self-call.

The getter returns accelerator +0x119c shifted left ten in a 32-bit register.
Neither this getter nor init locally verifies a nonzero, power-of-two size,
shift overflow or size >= eight, despite later cursor masking relying on
size-1. The actual producer/configuration of +0x119c must establish those
invariants; this review does not claim an invalid configured size occurs.
Likewise repeated getter calls are assumed stable under initialization
ownership, and resource/context lifetimes require outer-owner proof. This
establishes the origin of ring geometry rather than inventing a constant
capacity for a future guard. No production/runtime change. Next: actual
configuration of +0x119c and allocator mapping size, then owner serialization.

Complete writeQWord 41d48/132 is now reviewed/pinned, closing the selected
base dword/qword/buffer writer body review. Its pending TLB/AUX and recursive
software-prefix ordering matches the other two writers. It performs a single
eight-byte CPU store, advances/masks the cursor, and subtracts eight available
bytes without local capacity or contiguous-tail validation. Its final apparent
call to the next method is an imported stack_chk_fail relocation, not a
writeBuffer edge; only the software-prefix call 41e31 is a real direct edge.
The import and virtual emission are separately pinned.

The repair requirements now distinguish four boundaries: reservation must
establish actual space rather than elapsed wait; every writer must reject
faulted/unreserved use before mutating pending flags or CPU bytes; submission
must reject invalid ownership/state before advertising a tail; backing/table
retirement must wait for the appropriate hardware completion. Satisfying one
boundary does not imply the other three. The reviewed common bodies rely on
outer reservation and serialization, so a VF admission repair needs their
effective caller/locking model rather than unrelated global counters. All
three bodies being pinned does not certify all writer callers or memory
accesses in the project. No production/runtime change; next establish ring
construction geometry, ownership/serialization and actual submit tail bounds.

Complete writeBuffer 41e7a/16a is now reviewed/pinned. Like writeDWord, it
consumes pending TLB/AUX bits only when readiness is set, clears the pending
bit before emitting the corresponding command and clears readiness afterward.
The ordinary recursive software-prefix branch sets ring +0x6c before writing
its three-dword prefix, preventing that branch from recursively adding itself.
The selected virtual TLB, AUX and recursive edges are pinned.

It obtains the CPU ring address, copies the requested dwords contiguously,
subtracts count*4 from available bytes, advances/masks the cursor and returns
true. There is no local capacity/contiguous-tail check, source/destination null
guard, count-overflow check or protocol-fault gate. Even a zero count enters
pending-command handling first. These are caller preconditions, not proof of
reachable faults under valid reservation/locking. In particular a false
reservation ignored by a caller cannot be recovered merely by trusting this
method's true return. The per-engine pending bitmap update is a non-atomic
read/modify/store in this body; outer serialization must be established before
calling it a race. Clearing pending before emission prevents ordinary same-bit
recursive insertion, but is not an invalidation completion acknowledgement.

No production/runtime change. The repair boundary must cover reservation,
write helpers and submission under a consistent owner/admission contract;
retaining old page-table/backing lifetime is a separate required transaction.

Complete generateFlipWait 7e13c/da is now reviewed/pinned. It gets the ring
from FIFO channel +0x130, requests one dword then writes display-machine
+0x200 directly with writeDWord. It next obtains a resource GPU address,
constructs a seven-dword command buffer, requests seven dwords, writes that
buffer and clears display-machine +0x1f8/+0x200. Neither reservation result
is checked anywhere in this full body. It does not locally submitToRing or
wait for completion; downstream caller submission remains to trace.

The product kern_gen11.cpp has no named generateFlipWait/writeDWord/writeBuffer
route. This selected source-search result does not prove an exhaustive absence
of indirect/binary modification or runtime execution. The complete method
establishes the unchecked caller obligation more strongly than the earlier
short windows, but does not by itself show its fixed-size requests overflow
valid ring geometry or reproduce a fault. Timeout-success behavior is a
separate path even when requests fit capacity. A repair that only improves
the boolean reservation result cannot protect this caller's subsequent writes
or its clearing of display state. Admission must cover the actual write and
submission graph, retaining resources on fault rather than silently advancing.
No production/runtime change. Next: effective caller/submit graph and writing
helper postconditions, before selecting a bounded VF-only implementation.

A source search finds no product route named waitForSpace, waitTimeout or
checkForProgress. This does not exclude all binary patches or indirect
runtime changes. Existing lifecycle tests already checked portions of this
progress/timeout graph; the recent complete-body pins supplement rather than
replace those contracts. Avoid representing these repeated edges as new
runtime validation.

An exploratory decoded direct-call scan, excluding external relocation
placeholders, found 22 waitForSpace call sites in native __text. This is not
an indirect-call inventory or proof of runtime reachability. Selected edges
are now pinned: FIFO submitStampCommand 4c552 and submitRingCommands 4c63b
immediately test AL and branch on false. Display generateFlipWait calls
7e169 and 7e1d0 have no immediate result test before proceeding toward
writeDWord/writeBuffer. Other callers include semaphore, blit and sync-event
paths. Full caller-body and effective VF route review remains necessary.

Therefore changing waitForSpace alone to return false on timeout cannot be
assumed safe across retained callers: some checked callers can propagate a
failure, while unchecked ones may continue command writing. A repair needs
an owner-aware admission rule that prevents subsequent writes/submission,
preserves backing and distinguishes reservation failure from GPU completion.
Do not patch the common method blindly or substitute Guest panic for Host
containment. No production/runtime change accompanies these selected contracts.

The scheduler4 vtable +0x150 is now resolved/pinned to checkForProgress at
1db54/8. The complete method unconditionally returns true; it checks no ring
head, completion token or hardware status. The existing VF startup uses
accelerator +0x1250 as its scheduler receiver, consistent with this native
typed vtable interpretation. Runtime object identity still requires observation;
do not generalize this result to every scheduler class.

For the reviewed scheduler4 dispatch, waitTimeout's true-result branch skips
debugGraphicsEngine and its reset-failed counter/panic path, adds elapsed
accounting and returns even if the last predicate remained true. Thus the
native helper provides no capacity postcondition on this timeout branch.
Whether an outer owner prevents this branch or ensures a later recovery
remains to prove. It is not safe to claim checkForProgress true proves GPU
progress or that timeout automatically performs reset. Next: establish caller
handling and effective VF routing before a fail-closed reservation repair;
do not simply panic and treat that as Host DMA containment.

Correction after complete callee review: 43886/40 is debugGraphicsEngine,
not a direct reset/recovery routine. It calls diagnostic bodies 438c6 and
439b2, then tail-dispatches imported signalHardwareError with restart request
1 and a context-derived stamp. Those diagnostic sub-bodies and downstream
event restart behavior remain separate audit work. This wrapper does not
itself prove reset, restored capacity or DMA quiescence. Earlier references
to recovery 43886 should be read as an unresolved error-handling path, not
verified recovery. Its complete body is now pinned in both native payloads.

The complete waitTimeout cold body 90a72/12 is also pinned; its import is
panic, with message indicating GPU reset did not succeed. That message is
not evidence a hardware reset actually occurred before this branch. Guest
panic still does not stop Host DMA. The accelerator +0x1250 virtual receiver
and its effective +0x150 implementation remain to resolve before interpreting
the preceding return paths. No production/runtime change.

Complete waitTimeout 41aa4/106 and three waitForSpace block predicates are
now reviewed/pinned. The first predicate begins at 41baa, not 41bab (which
omits its push instruction); its complete length is 0x13. The other lengths
are 0x16 and 0x17. Predicates respectively test head > tail (unsigned),
required bytes + 8 > head (unsigned), and free bytes < requirement (signed).
They observe ring geometry, not a particular invalidation completion token.

waitTimeout initially evaluates the predicate and returns zero if already
satisfied. Otherwise it uses assert_wait_timeout/event plus thread_block,
adds 0x186a0 per iteration, rereads masked head/free bytes, and reevaluates.
The comparison threshold is 0x12a05f200 or 0x746a528800 depending on accelerator
+0x1190 bit 5. The counter is nominal per-iteration accounting, not a fresh
wall-clock comparison. Scheduler delay, recovery and downstream blocking are
not bounded by that comparison alone; do not describe this as a hard deadline.

If the predicate remains true at the threshold, it records mach_absolute_time,
dispatches accelerator +0x1250 object's virtual +0x150 with engine ID, and
conditionally invokes ring recovery 43886. A subsequent ring +0x94 threshold
can branch to cold helper 90a72; recovery/cold bodies and effective receiver
vtable remain pending. Returning paths add recovery-time delta to the counter
and return that amount, without a local final predicate check or boolean
failure status. Thus waitForSpace's later true is not independently proof that
requested capacity was obtained after recovery. Caller/recovery postconditions
must supply that proof before any repair changes readiness or backing lifetime.
Imported waits/time calls are pinned, not misread as self-calls. No runtime change.

The full symbol-bounded waitForSpace 416c8/3d0 body has now been reviewed and
pinned, superseding the earlier partial-window scope. It adds native overhead
to requested dwords, checks pending engine bits, dispatches +0x150 for TLB
space and sets readiness +0x6d; AUX adds three dwords and sets +0x6e. Both
readiness stores precede the capacity check. The rounded byte requirement
must fit ring size minus eight; an oversized request returns false without
locally clearing those readiness bytes. Whether callers ever issue such a
request, and how they handle false before subsequent writes, remains to prove.

The remaining body reads masked head information from ring +0x18, recalculates
free bytes, handles cursor wrap by zero-filling the unused tail, and may call
helper 41aa4 with stack-constructed predicates. It accounts the helper's return
in elapsed statistics and returns true on the completed ordinary path; the
helper/predicate bodies have not yet been fully reviewed. Do not infer a
bounded wait or failure propagation merely from this method name. Arithmetic
uses 32-bit counts and signed free-space comparisons; caller size bounds
and valid ring geometry remain required.

This method reserves command space and observes ring-head progress, not an
acknowledgement for a particular TLB invalidation. Neither its successful
return nor the readiness bit demonstrates retirement of an old page table.
No executable patch or runtime mutation. Next: review the wait helper and
predicates, caller handling of false, and submission ordering that separates
command insertion from completion.

The base, Compute and Main ring vtables now pin +0x150 to their respective
getFlushTLBSpace methods and +0x160 to their writeFlushTLB methods. Complete
0x20-byte reservation bodies at 428ec, 4e6d8 and 8507c are reviewed/pinned.
Base returns 5 dwords when accelerator +0xfd6 bit 16 is clear, 10 when set;
Compute/Main return 6 or 12 under the same bit. These totals match the
previously reviewed single/double command emissions. This establishes the
selected native vtable pairing and size calculation, not complete caller
space accounting, readiness lifetime, effective patched runtime dispatch or
hardware command completion. Other ring-class pairings remain to inspect.
No production change; the deferred native invalidation still cannot stand
in for synchronous retirement of old PPGTT tables.

Tracing base registers in the writeDWord body establishes that ring +0x10
supplies the accelerator and ring +0x40 supplies the engine ID. The indexed
qword accesses at accelerator +0x1340 and +0x1380 are therefore pending bitmaps,
not unrelated ring-array fields. Earlier exploratory wording that grouped
these indexed ring accesses with unrelated-object matches was too broad.
The GL/display object matches remain distinct; equal offsets alone are still
insufficient. This positive base-register trace supersedes that classification.

Complete writeDWord 41c12/136 and writeFlushAuxTLB 42a22/46 are now reviewed
and pinned. writeDWord checks the engine bit and ring +0x6d readiness; when
both are set, it clears the bit before invoking virtual +0x160 to emit the
TLB command, then clears readiness. AUX follows analogous +0x1380/+0x6e
handling with direct call 42a22. The AUX emitter accepts engine IDs 0..5
selected by mask 0x2b and emits three dwords; unsupported IDs return without
emission. Engine provisioning/caller readiness invariants remain to verify.

A partial waitForSpace window 416c8..41790 independently shows the same
accelerator bitmap and engine ID, requests extra space via virtual +0x150,
and sets readiness +0x6d. This window is not a complete waitForSpace review.
writeDWord later obtains the CPU ring address, writes the requested dword,
advances/wraps its cursor and decreases available bytes. It has no local GPU
completion wait. Clearing pending flags means command insertion has begun,
not that hardware has executed the invalidation. Effective vtable targets,
complete reservation handling and submission/completion ordering remain open.
No production/runtime change or claim that all consumers have been inventoried.

Complete reviewed/pinned bodies: flushHardwareAfterGttUpdateOfAux 2d1ee/16,
base writeFlushTLB 4290c/116, Compute writeFlushTLB 4e6f8/172 and Main
writeFlushTLB 8509c/172. The AUX helper tests the same accelerator feature
bit and ORs +0x1380 with 0x3f; it is also not a synchronous invalidator.

The base ring method derives a destination from task resource GPU address,
combines native flags and emits one or two five-dword buffers depending on
accelerator +0xfd6 bit 16. Compute/Main emit one or two six-dword buffers
under that bit, incorporating a context selector from 42364 and the resource
GPU address. Six direct writeBuffer call sites are pinned in both payloads.
These methods construct commands and return; there is no local completion
wait or GuC invalidation acknowledgement in the reviewed bodies. Their command
constants are recorded by body hashes, not newly certified against Intel
generation-specific specifications. Caller execution ordering, effective VF
routes, resource-address lifetime and command completion remain to trace.

An exploratory __text scan for memory displacements 1341..1347 found no
operands. As with the earlier 1340 scan, this does not exclude indexed-byte
access, derived field pointers, aliases or outlined consumers. Do not mistake
unrelated ring-buffer indexed +1340 accesses for the accelerator pending
flags. These four added bodies narrow the candidate graph but do not yet
connect flag consumption to safe PPGTT retirement. No production/runtime change.

Reviewed and pinned the remaining expandLevel methods at e212/de and e2f0/98.
Both obtain a page from the manager's pool, store its descriptor in the child
record, and initialize all 512 hardware entries with the masked dummy page
and flags 3 before publishing the parent hardware entry. The directory method
additionally allocates/zeros a 0x4000 software-record array. On that allocation
failure it releases the newly allocated descriptor, clears the child descriptor
and returns false without publishing a parent hardware entry. Imported calls
at e27e and e297 are IOMalloc and memset, not zero-displacement self-calls.
The leaf method has no software-array allocation after page acquisition.

Both methods guard the parent pointer for the hardware entry store but then
increment the parent count unconditionally. Their reviewed expandRange call
sites pass a parent; this conditional null dereference is not an independently
proven reachable defect. Neither method contains a local invalidation or
completion barrier. This construction ordering does not establish retirement
of a formerly live descriptor or safety of caller rollback.

Also reviewed/pinned complete releaseRange 144ac/d4 and
flushHardwareAfterGttUpdate 2d1d8/16, with explicit virtual-unmap, subsequent
direct-flush edge and unconditional-success anchors. releaseRange invokes
virtual offset 0x130, then calls the flush method and returns true. The flush
method only tests accelerator +0xc78 bit 1 and, when set, ORs +0x1340 with 0x3f;
it issues no request and waits for no acknowledgement. This corroborates the
already documented deferred-flush behavior; it is not a new defect discovery.
Combined with native unmap's pruning/release before this call, the PPGTT
retirement proof must locate the effective consumer and its locking/admission
policy. The custom VF GGTT unmap has its own synchronous invalidation path;
do not conflate that repaired path with unmodified native PPGTT pruning.

No executable patch, deployment, VM start or Host GPU manipulation. Next:
trace effective deferred-flush consumption and establish a transaction owner
that retains old tables/backing until required GPU invalidation completes.
An exploratory linear __text operand scan for displacement 0x1340 found
constructor/start writes, the flush OR and many accesses in unrelated object
types or indexed ring-buffer expressions. Equal displacement is not field
identity. This scan is not a complete consumer inventory: pointer arithmetic,
other bytes of the flags word, outlined code and register aliasing remain to
trace. Do not infer that the flag has no consumer from this negative search.

Complete symbol-bounded bodies reviewed in both native payloads and now hash
pinned: mapRange d0ce/e6, pageWalk3 e3ae/68, unmapRange da60/be,
mapRangeDummy db1e/e4 and expandRange d1b4/246. These pins detect payload drift;
they do not prove runtime communication or GPU completion.

Both mapping methods first expand the hierarchy. Expansion failure invokes
shrinkRange and returns false. After expansion succeeds, a later pageWalk3
failure returns false without undoing already-written leaf PTEs or invoking
shrinkRange. Each successful store increments the leaf record count. Normal
single-threaded expansion should populate the requested hierarchy: reachability
of this later failure still needs caller locking, shared-record and range
invariant proof. Do not present the conditional prefix hazard as a reproduced
failure. The independently reviewed higher-level commit loop also retains
earlier successful mapping segments on a subsequent failure.

pageWalk3 checks descriptor pointers at the first two hierarchy levels, but
dereferences their software-array pointers without separate null checks.
expandRange likewise treats an existing descriptor as sufficient to descend.
Shared descriptors and the fixed 1-GiB synchronization window therefore need
an explicit caller-range proof, not a blanket claim that shared mappings crash.
expandRange creates the root even for an empty range; its inclusive end and
mapping methods' exclusive ends use unchecked arithmetic. Alignment and valid
address-range guarantees remain caller obligations under review.

unmapRange skips failed page walks, writes the manager's dummy physical PTE
with flags 3 for successful walks, decrements the leaf count without testing
the old PTE, and tail-calls shrinkRange. Do not infer a boolean success result
from the symbol name or residual return register. None of these five bodies
locally supplies GPU invalidation/completion before table pruning or backing
reuse. Caller releaseRange/cleanup and GuC acknowledgement ordering must close
that lifetime boundary. Mapping dummy PTE composition uses addition rather
than OR; attribute/address overlap must be checked against caller policy.

No production hook or runtime change accompanies this audit. Next: review the
remaining lower-level expansion bodies, then map the full commit-failure
rollback and release/invalidation owner graph before choosing a repair.

# PTE commit status and failed-wire final free follow-up

Reviewed/pinned complete base commit_pte (0x64) and Intel
commitIntoGPUPageTable (0x140). Mapping prepare's outlined helper directly
calls commit_pte after successful parent prepare. Certain map flag modes skip
the hardware commit virtual; otherwise object +0x170 dispatches Intel commit.
False returns before installed-PTE flag/counter publication; true sets the
installed flag, updates parent accounting and increments generation. This
flags/generation transition is not proof of GPU execution or DMA quiescence.

Intel commit's bypass flag returns true; otherwise it invokes manager
commitIntoPageTableForTask at 0x11275 and forwards the result. Native slot and
direct manager edge are pinned in both payloads. Manager body/partial map
failure behavior and exact bypass ownership are still pending; tracing import
placeholders are not interpreted as self-calls.

Re-reviewed the entire already-pinned sys-memory free (0x1d6). It calls unwire
only when wired flag +0xc bit 1 is set, then releases descriptor +0xd0 and
pool backing/provenance before inherited base free. It does not inspect,
clear, complete or release command +0x148 in the unwired branch. Previously
reviewed inherited memory free also has no command cleanup; Intel's declared
free slot imports this base sys-memory free. Thus these final-free bodies do
not supply missing failed-wire command cleanup. If the command still retains
the descriptor, releasing the sys-memory descriptor reference does not alone
prove freed backing/UAF: it can leave the command/descriptor reference leaked.
The earlier premature descriptor complete and possible active-registration
panic remain separate concerns. No assertion of observed crash cause.

New wired-only free branch and commit-helper/virtual anchors are pinned.
Target cleanup must close admission and respect command/descriptor pairing;
do not force-release backing merely to eliminate the leak. No runtime mutation.
