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
| tools | 23 | Offline tests/build checks read; older extraction/mapper tools pending |
| MacKernelSDK | 1,163 | Selected API declarations only; full review pending |
| Lilu.kext | 39 | Selected headers/upstream patching code; full dependency review pending |
| HookCase-master | 7 | Build checked, full source/assembly review pending |
| sle_Internal | 35 | Metadata only in this count; embedded binaries are a separate review obligation |
| NootedGreen.xcodeproj | 4 | Build references inspected; unreferenced firmware headers and null build entries removed, settings/full project review pending |
| .github | 1 | Build/test workflow read; no deployment step |

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

| Files/area | Evidence and remaining boundary |
| --- | --- |
| kern_gen11.cpp/.hpp | Partial, protocol-focused review plus targeted native disassembly; Mach-O relocation audit removed 39 unreachable private wrappers/stubs and their original slots. Active empty VDD/shutdown replacements, four unversioned private-offset sleep/wake mutations and the PAVP command-4 false-success hook were removed so native physical lifecycle and DRM handling remain authoritative. VF native blit/barrier producers now require the complete transport admission state, preserve owned task/context objects and propagate native results; old V120/V130/V142 fabricated/bypass protocols were removed. All Tahoe TGL private routes/byte patches require the pinned UUID. GGTT, GuC/CTB, IRQ, direct-LRCA, force-wake and legacy-ring containment routes now install only on a classified VF; PF keeps native ownership. Direct GGTT mutations now use an explicit system-memory PTE encoder, aligned 64-bit stores and synchronous post-CTB GuC invalidation rather than Apple's split physical encoding. VF descriptor attach/detach no longer enters Tahoe's unused legacy proxy-slot/LRCA-hash bodies; routed direct submit, idle and lifecycle operations use one context table, removing silent hash-allocation failure and two live physical-TLB patch sites. Unreachable PF fallbacks and original slots plus the physical topology wrapper/force-wake port were removed. The two accelerator payloads have identical executable content and differ only in bundle identifiers. Target-specific topology constants are removed: a one-shot, versioned PF MMIO relay supplies the five VF-invisible values consumed by `getGPUInfo`, with pure protocol/fuse tests and exact unique binary anchors. The broad SKU bypass is now a one-immediate `0x9a40`→`0x9a49` comparison change that preserves every native failure branch. Other reachable accelerator routes still require semantic review |
| kern_green.cpp/.hpp | Read end-to-end; PCI identity/overread, exact GMS decoding, mandatory config-hook admission, BAR0/BAR2 publication, property-result reporting and dead-path cleanup completed; device-removal/power-transition lifetime and hardware-specific property semantics remain open |
| kern_genx.cpp/.hpp and kern_gen11 ICL fallback (removed) | Read end-to-end. Both paths used unversioned private control-flow/ID rewrites; the latter also forced a Sonoma SKU gate, fixed 64-EU topology and firmware hook without a Tahoe payload to verify. Native Ice Lake is no longer intercepted; the maintained compatibility path is the UUID-pinned TGL payload. |
| kern_patcherplus.cpp/.hpp | Read and compared with upstream Lilu routing/replacement behavior; every grouped lookup is now preflighted before its first write; low-level write/route rollback and protection restoration remain open |
| DYLDPatches.cpp/.hpp | Read end-to-end; removed unadmitted CoreDisplay control-flow patches, unreachable CoreLSKD/ICL fallbacks and stale bundle redirection. Remaining mutations are shared-cache composite media-model strings; their necessity and dynamic media behavior remain open |
| DisplayMergeNub.cpp/.h | Read end-to-end; validates inputs, bounds recursion, copies introduced nested dictionaries and stages the complete provider table before one publication. Actual IOKit behavior and races with unrelated external registry writers remain open |
| IntelDPLinkTraining.cpp/.hpp | Read and tables compared with i915; corrected PHY layout, physical-only guards; full platform/stepping/link-training integration remains open |
| kern_start.cpp | Read; lifecycle integration still depends on the unfinished driver |
| Firmware.cpp, FirmwareADLP.cpp (removed) | Payload bytes were compared to pinned upstream containers before removal. Their only consumer was the retired manual DMC/MMIO loader; native physical drivers now own DMC lifecycle, and a VF owns none of it |
| kern_gpu_capabilities.hpp, kern_guc_ring.hpp, kern_ggtt_bounds.hpp, kern_ggtt_rotation.hpp, kern_vf_ggtt_pte.hpp, kern_pattern_match.hpp, kern_dvmt_patch.hpp, kern_context_pool.hpp, kern_binary_identity.hpp, kern_pci_identity.hpp | Implementations read and offline-tested; GPU/PF identity now gates native TGL branches instead of CPUID, all GGTT unmap/CTB lifecycle states are checked, all accepted Apple GGTT attribute masks produce address+present without LM/VFID, 4,096 rotation matrices are verified as bounded permutations, and every 8-bit Gen9+ GMS encoding is checked for exact 32-bit representability. Tests cover pure helpers, not all caller lifetime/hardware contracts. |
| kern_vf_irq_gate.hpp, kern_vf_context_shutdown.hpp, kern_vf_submission_gate.hpp | Read and exhaustively checked as pure state machines. The callback/operation admission gates, all nine direct-LRCA shutdown classifications, all 108 state/pending/runnable MODE_DONE combinations and all 512 native producer/consumer admission combinations are covered. MODE_DONE requires the firmware runnable payload to match the oldest pending enable/disable token. G2H consumers remain open for submission-stopped teardown but reject partial init, sealed transport and protocol-fault quarantine; actual firmware completion and DMA behavior still require controlled hardware validation. |
| kern_vf_memirq.hpp, tools/vf_memirq_test.cpp | Read against Tahoe `IGInterruptBridge` disassembly and current i915 media-12 engine/memory-IRQ tables. All 64 possible source offsets, all 64 active-engine combinations, exact VCS0/VCS2 routing and context-image LRM/LRI writes/bounds are tested; real interrupt delivery remains a controlled-runtime obligation. |
| kern_vf_guc_event.hpp, tools/vf_guc_event_test.cpp | Read against current i915/xe HXG and G2H dispatch. The FAST-only bridge admits exact MODE_DONE, DEREGISTER_DONE and TLB_DONE shapes, classifies exact context-reset/engine-failure shapes as fatal, and rejects all other event actions rather than silently dropping firmware state. All 65,536 actions, lengths 0..32 and header variants are sanitizer-tested; firmware delivery remains a controlled-runtime obligation. |
| kern_vf_runtime.hpp, kern_vf_runtime_patch.hpp | Read against current i915 VF/PF early-MMIO ABI and media-12 fuse decoding. Request framing, CRC-derived magic correlation, reply validation and invalid topology cases are sanitizer-tested; every runtime injection anchor is exact-one in both admitted accelerator payloads. Firmware/PF response behavior remains a controlled-runtime obligation. |
| kern_context_descriptor.hpp | Read against Tahoe context creation/submission/detach disassembly, its six-entry `IGHwCsType` jump table and current i915 Gen8-Gen12 LRCA/engine definitions; packed native descriptor is decoded without alignment assumptions, every persistent/reserved field and exact media-12 class/instance pair is validated, engine class is mapped explicitly, the complete low/high/SW-ID/class/instance/backing identity is shared by duplicate attach, submit and detach checks, and GuC `FORCE_RESTORE` normalization is exhaustively tested; object pointer readability remains a caller contract |
| kern_unaligned.hpp | Read; little-endian 32/64-bit reads and writes tested over offsets 0..15 with canaries; callers retain mapping/lifetime obligations |
| kern_model.hpp | Read; cosmetic branding lookup only, not a capability/support table; marketing labels not independently certified |
| kern_netdbg.cpp/.hpp (removed) | Read completely; unbuilt/unreferenced retired logger with overread, port-shadowing and error/locking defects; recoverable in Git |
| Firmware.hpp, tools/check-dmc-blobs.sh (removed) | Read; declarations and byte-comparison helper became unreferenced with the manual DMC loader and were removed |
| kern_workqueue_unwind.hpp, tools/workqueue_unwind_test.cpp | Read; failed-init resource ordering tested offline; native object destruction and caller failure propagation remain incomplete |
| AppleIntelParams.hpp (removed) | Read all 624 lines and cross-referenced every declared type. After the speculative framebuffer plane/scaler/accessor routes were retired, the generated header had no runtime consumer; its known alignment/type contradictions could only misrepresent the supported ABI, so it and both main-source includes were removed. The Ghidra extraction tools remain offline research inputs and are not driver declarations. |
| Info.plist | Read all 124 lines; main personality/build identifiers and SchedulerType consumer inspected; profile metadata has no main-source/tools consumer found, display injection remains hardware-specific; not a supported-device matrix |
| IGGucBinary.h, IGHucBinary.h (removed) | Read as opaque 2017 Apple firmware arrays; no include or symbol consumer existed. Removed their 244-KiB source payload and product-header entries; recoverable in Git. Runtime firmware remains owned by admitted native/PF driver paths. |

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
