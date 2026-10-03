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
PF behavior remains native. The route inventory is now 94 (91 accelerator,
3 framebuffer admission symbols); both payloads pin the event-timeout virtual.

This is a provisional error-containment boundary, not implemented GPU recovery,
successful acceleration or host DMA containment. A guest panic cannot stop
already-issued DMA or guarantee PF health, so the runtime hold and independent
host watcher/deadline are still mandatory. Normal completion, stable reclamation
and a real hardware-safe recovery design remain unfinished. Offline tests
enforce fault-before-panic and prohibit native/helper/backing side effects.

## Mapping getter provenance follow-up (offline)

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
