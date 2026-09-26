# Source-review coverage — incomplete

The user's baseline requires all program files to be reviewed. This ledger
does not certify that requirement as complete, and passing CI is not source
review or hardware validation. The VM must remain off while the protocol
audit's runtime blockers are open.

## Scope

At c312229, the tracked source/build/metadata inventory contains 1,305 files:

| Area | Files | Current review boundary |
| --- | ---: | --- |
| NootedGreen | 33 | Mixed; details below, not all findings closed |
| tools | 23 | Offline tests/build checks read; older extraction/mapper tools pending |
| MacKernelSDK | 1,163 | Selected API declarations only; full review pending |
| Lilu.kext | 39 | Selected headers/upstream patching code; full dependency review pending |
| HookCase-master | 7 | Build checked, full source/assembly review pending |
| sle_Internal | 35 | Metadata only in this count; embedded binaries are a separate review obligation |
| NootedGreen.xcodeproj | 4 | Selected build references/settings, full review pending |
| .github | 1 | Build/test workflow read; no deployment step |

The earlier 1,256-file count omitted assembly and some build metadata. The
expanded scope above includes `.s`, `.S`, `.inc`, `.tool`, `.plist`, scheme and
workspace metadata. Subsequent deletion of the two retired logger sources
reduces this snapshot to 1,303. These are inventory counts, not completion
percentages. Binary libraries, kext executables, firmware instructions and
external reference trees are not magically reviewed by counting source files.

## Main project coverage

| Files/area | Evidence and remaining boundary |
| --- | --- |
| kern_gen11.cpp/.hpp | Partial, protocol-focused review plus targeted native disassembly; large physical display/accelerator sections and declarations remain open |
| kern_green.cpp/.hpp | Read end-to-end; PCI identity/overread/BAR publication fixes made; DVMT, device lifecycle, property failures, per-GPU gates and other findings remain open |
| kern_genx.cpp/.hpp | Read; VF admission/DVMT/PM fixes made; physical behavior and legacy stubs not fully validated |
| kern_patcherplus.cpp/.hpp | Read and compared with upstream Lilu routing/replacement behavior; transaction/rollback and protection restoration concerns remain |
| DYLDPatches.cpp/.hpp | Read; removed unused AMD tables and isolated Sonoma patches; user-space shared-cache and per-binary scoping remain open |
| DisplayMergeNub.cpp/.h | Read; input/refcount/recursion fixes; property update atomicity and failure recovery remain open |
| IntelDPLinkTraining.cpp/.hpp | Read and tables compared with i915; corrected PHY layout, physical-only guards; full platform/stepping/link-training integration remains open |
| kern_start.cpp | Read; lifecycle integration still depends on the unfinished driver |
| Firmware.cpp, FirmwareADLP.cpp | All payload bytes compared to pinned upstream containers; bounds fixed; NOT a firmware-instruction semantic review |
| kern_gpu_capabilities.hpp, kern_guc_ring.hpp, kern_ggtt_bounds.hpp, kern_pattern_match.hpp, kern_dvmt_patch.hpp, kern_context_pool.hpp, kern_binary_identity.hpp, kern_pci_identity.hpp | Implementations read and offline-tested; tests cover the helpers, not all caller lifetime/hardware contracts |
| kern_model.hpp | Read; cosmetic branding lookup only, not a capability/support table; marketing labels not independently certified |
| kern_netdbg.cpp/.hpp (removed) | Read completely; unbuilt/unreferenced retired logger with overread, port-shadowing and error/locking defects; recoverable in Git |
| Firmware.hpp | Read; external DMC declarations only; payload and loader limitations remain in the protocol audit |
| AppleIntelParams.hpp | Read all 624 lines; compiler layout dump confirms unasserted controller/framebuffer tail offsets drift from comments. No direct callers of the checked mismatched fields found; generator and pinned-binary verification remain incomplete |
| Info.plist | Read all 124 lines; main personality/build identifiers and SchedulerType consumer inspected; profile metadata has no main-source/tools consumer found, display injection remains hardware-specific; not a supported-device matrix |
| IGGucBinary.h, IGHucBinary.h | Full review/verification not yet recorded; do not infer coverage from inclusion or successful compilation |

## Evidence rules

- `TAHOE_SRIOV_PROTOCOL_AUDIT.md` records concrete findings, source references,
  tests and unresolved conditions. A read file may still have severe defects.
- The pinned TGL binary was inspected by function, not exhaustively. UUID
  admission limits private-layout use but is neither integrity verification
  nor evidence that the known layout is fully safe.
- No Metal/media baseline, DMA-quiescence proof, complete PF/VF driver,
  virtual-display/Sunshine integration or all-source approval exists yet.
- Preserve the original user baseline in the parent `WORK_BASELINE.md` and
  append checkpoints; do not replace it with a narrower task.
