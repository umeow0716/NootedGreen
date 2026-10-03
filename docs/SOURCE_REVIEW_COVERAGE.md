# Source-review coverage — incomplete

The user's baseline requires all program files to be reviewed. This ledger
does not certify that requirement as complete, and passing CI is not source
review or hardware validation. The VM must remain off while the protocol
audit's runtime blockers are open.

## Scope

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
