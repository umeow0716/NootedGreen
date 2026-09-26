# NootedGreen

Experimental Lilu plugin for Intel Gen11 and newer iGPU bring-up on macOS,
using Apple's Ice Lake or Tiger Lake driver families where applicable.

## What it does

Patches Apple's Tiger Lake (Gen12) graphics drivers to work with newer Intel iGPUs. Handles device-id spoofing, MMIO addressing, ForceWake, GPU topology, display controller init, combo PHY calibration, and GT workarounds.

## Status

**Work in progress.** Historical physical-RPL testing reached the login screen
and exercised the accelerator, but those Sonoma results do not validate the
current Tahoe SR-IOV VF path. This branch has passed its offline protocol,
sanitizer, analyzer and remote build gates; controlled VF boot validation is
still intentionally blocked by the incomplete source/lifetime audit.

The remaining DYLD hook only applies composite media-model strings in signed
shared-cache pages and logs discovery of the TGL userspace bundles. The former
unversioned CoreDisplay control-flow stubs, CoreLSKD path, ICL Metal ID bypass
and GPU-bundle path redirect have been retired. `isRealTGL` is a compatibility
name for a native Tiger Lake **physical GPU** path, selected from the original
PCI identity and PF ownership; guest CPUID is diagnostic only.

### `codex/tahoe-sriov-vf` target override

This experimental branch is hardware-specific to an i7-13620H (`8086:a7a8`)
SR-IOV VF. A host `DRM_I915_QUERY_TOPOLOGY_INFO` query reports 1 slice, 4
enabled dual subslices, 16 EUs per DSS, and 64 EUs total. The RPL topology
patches therefore advertise 8 traditional subslices × 8 EUs instead of the
upstream i7-13700H 96-EU constants, and use the measured 100–1500 MHz range.
Do not use this branch unchanged on a 32-, 80-, or 96-EU SKU.

The Tahoe `25G229` VF bootstrap fixes on this branch are deliberately narrow:

- V213 converts the VF's missing stolen-memory GGTT range into the 4 GiB
  aperture represented by its 8 MiB GGTT BAR0 window. It does not falsify the
  memory manager's stolen-memory range.
- V214 treats a task as the bootstrap kernel task only while
  `IntelAccelerator+0x150` is unassigned, avoiding the null kernel-task clone
  path in `newPageTableForTask`.
- V216 resets a stale `IGAccelTask::fTaskCounter` immediately before that
  unassigned bootstrap allocation. This keeps address mode, PPGTT, managed
  page tables, and stamp/scratch setup on one consistent kernel-task path.
  The older late V215 per-object rewrite was removed because it changed task
  identity after page-table initialization. V216 also removes the unretained
  global task cache; fallbacks may use only the live kernel task currently
  owned at `IntelAccelerator+0x150`.

### Important (Current Test State)

- **CRITICAL FIX (V75):** Removed post-`awaitPublishing` property override that was causing cursor corruption (TV static). Display now stable.
- Early IGPU identity detection is confirmed working: Lilu reads `AAPL,ig-platform-id` (`9A490000`) via OpenCore DeviceProperties during `DeviceInfo` scan.
- Platform-ID and device-ID injection now handled cleanly by config.plist only — NootedGreen no longer interferes mid-initialization.
- System boots to login screen reliably with no visual corruption or kernel panics.
- Unversioned CoreDisplay DYLD assertion/control-flow patches are no longer
  installed. Any surviving full-Metal policy is kernel-side and must pass its
  own binary/route admission.
- Blit3D init policy on spoofed paths is now conservative by default: Apple's original `IGHardwareBlit3DContext::initialize()` is **disabled unless explicitly opted in** with `-ngreenV69AllowOriginal`.
- `-ngreenV69AllowOriginal` is **diagnostic only**. It can still panic at `IGHardwareBlit3DContext::initialize + 0x4c` (`SecurityAgent`/IOAccel submit path) on unsupported setups.
- `DisplayPipeSupported` native path is now the default. Use `-ngreendp0` only for forced fallback testing.
- V77 display-pipe client termination is disabled by default (delay defaults to full monitor window).
- **V88 scanout fill remains opt-in** (`-ngreenv88`). Default boots do not paint diagnostic bars over normal UI layout.
- **GPU reset storm fully tamed (V157):** `resetGraphicsEngine` circuit-breaker (V153+V154) confirmed working — V153 fires from call #1 (coerces `ret=1025→0` when RCS ring head == tail), V154 opens at call #4 (skips original reset entirely after 3 consecutive quiescent coercions). Ring CTL is restored to `0x7001` (RING_ENABLE set) on every fake-success path via V155 saved-CTL restore.
- **Execlist cleanup in progress (V158):** Root cause of remaining `userspace watchdog timeout` panics is identified — `EXEC=0x40018098` (execlist FIFO stalled, bit 30 set) and `CSB=0x1001` (15 unread context-status entries) persist post-fake-success, causing Apple's driver to keep scheduling resets. V158 addresses this by writing four null descriptors to `RING_ELSP` (cancelling pending EL0/EL1 contexts) and advancing the CSB read pointer to match the write pointer on every V153/V154 return-0 path.
- **V80L root cause identified:** The `userspace watchdog timeout` KP (T+120s, 2 induced WS crashes) was traced to the V80L plane-linearization block inside `v71EmrEnforcer`. It was writing `PLANE_CTL` / `PLANE_STRIDE` / `PLANE_SURF` every 50ms while WindowServer was actively compositing those same registers — WS detected a render failure, crash-looped, and watchdogd fired. V80L is now limited to the first 3 enforcer ticks (≤150ms after `start()`), which preserves the visible brief display flash at early boot without fighting WS after it takes over the compositor. Use `-ngreenv80l` for continuous mode (testing only).

### Current Active Investigation — ExecList LRCA page1 repair

**Root cause:** `IGHardwareContext::restoreFromSafeImage` returns `true` on RPL and skips the `g_cInitGfxRingContextRCS` memcpy that would normally write a valid Gen12 ring-context LRI (MI_LRI header + RING_HEAD/TAIL/START/CTL register pairs) into LRCA page1. The LRCA page1 at GGTT[0x19] (the ExecList submission slot hardware reads on context-restore) is left at GEM fill (`0x00ffffff` for arg=2 contexts, `0x00bfbfbf` for arg=0 contexts). Hardware cannot parse the ring registers → context submits but immediately idles without going ACTIVE → `fwWaitForHardwareRegisterValue` times out (15 polls) → HANGCHECK → retry loop.

**Fix (V508/V509):**
- `IGHardwareContext::withOptions` post-hook (V508) saves the ctx pointer to `cb->lastRCSCtx` and copies LRCA page1 from the actual context buffer (GGTT[`ctxPage1Idx`] — which `restoreFromSafeImage` does write correctly) to the submission slot (GGTT[0x19]). No hardcoded ring values; the copy uses exactly what Apple wrote.
- `IGHardwareContext::initWithOptions` post-hook (V509) logs for diagnostics — confirms the context buffer is valid (`DW1=11001015`/`11081019`) right after init.
- `populateResetRegisterList` post-hook (V507) re-runs the copy on every engine reset cycle using `cb->lastRCSCtx`. The display retry loop reuses an existing context without calling `withOptions` again, so V508 never fires after initial context creation; V507 covers all subsequent submission attempts.

**Validity check:** `(ap[1] & 0x1F801000) != 0x11001000` — catches both fill patterns and any other non-LRI value. Passes `0x11001005` and `0x11081019` (the two real headers seen from Apple's context images, which differ by LRI count field for different context types).

**Status confirmed from logs:**
- V508[1]: slot `00ffffff` → copied from ctx buffer → `DW1=11001015` ✓
- V508[4] (arg=0): slot `00bfbfbf` → copied → `DW1=11081019`, persists in all subsequent V508 calls ✓
- Accelerator starts: V45 `getState()=0x1e` (reg=1 match=1 pub=1), Metal/GL/VA plugins loaded ✓
- V507 re-repair now fires on every `populateResetRegisterList` return to cover the display retry loop

**Remaining blocker:** Preamble ring (16 DWs, two PIPE_CONTROLs) executes (`HEAD=TAIL=0x40`), but stamp write (`value=2` to GPU address `0x40001200` = GGTT[0x40001]+0x200) may not land — GGTT[0x40001] PTE not yet verified. If unmapped, `fwWaitForHardwareRegisterValue` polls forever. Next step: add GGTT[0x40001] PTE + content dump to HANGCHECK.

### Recent Progress

- **fRegCache/fWriteAccessor boot hang root cause identified and fixed:** `AppleIntelPlane::fRegCache` is at **+0x88** (verified via disasm) — it holds an `AppleIntelPlaneRegCache *` that Apple's code dereferences for vtable DSB accessor dispatch. Writing `ccont` to `fRegCache` causes an immediate hang because `ccont` is the wrong object type (vtable mismatch on DSB method dispatch). The correct ccont slot is `fWriteAccessor` at **+0x90**. All three plane ccont-init hooks in V204 now use `getMember<void *>(that, 0x90) = ccont` instead of `that->fRegCache = …`, leaving Apple's real `fRegCache` pointer untouched. `AppleIntelPlane` struct fully verified: `fEnabled`=+0x84 (byte), `fRegCache`=+0x88, `fWriteAccessor`=+0x90, inline shadows `PLANE_CTL`=+0x100, `PLANE_STRIDE`=+0x104, `PLANE_COLOR_CTL`=+0x154 (all confirmed from disasm).
- **V221 `waitForStamp` restored:** Routes `IOAccelEventMachine2::waitForStamp` — if a stamp times out before `Gen11::gGfxAccelStartDone` is set (i.e. before `IntelAccelerator::start()` returns), fakes success (`*outStamp = stamp`, return 0) to unblock a CoreDisplay deadlock that occurs when CoreDisplay calls stamp-3 before the GPU accelerator finishes initialising. `gGfxAccelStartDone` is set `true` in the `start()` hook wrapper immediately after the real start returns; from that point stamps are handled natively.
- **V500 `set_id_mode` diagnostic restored:** Routes `IOAccelLegacySurface::set_id_mode` and logs failures (capped at 32 per boot). On failure, extracts `badBits = mode & 0xff8073c0` and `goodBits = mode & 0x007f8c3f` to identify which reserved mode bits the caller is setting. Pure diagnostic — does not alter return value.
- **DBUF root cause identified (`-ngreentglwithgfx` required on Gen11+):** Per-plane `DBUF_BUF_CFG_<PIPE>_<PLANE>` allocation lives in the watermark/atomic-commit pipeline that ships with `AppleIntelTGLGraphics.kext` (the HW accelerator kext), not in `AppleIntelTGLGraphicsFramebuffer.kext`. With FB-only mode (`-ngreentglfb`), `DBUF_BUF_CFG_A_PA` stays at `0x00000000` even though `PLANE_CTL` enable bit is set — the display engine fetches from a zero-block DBUF range and produces duplicated/fragmented output (most visible on the boot Apple-logo + loading-bar phase and the login screen). Loading both kexts via `-ngreentglwithgfx` lets Apple's native code program DBUF correctly. V203 register dump (Pipe/Trans/DSC/M-N/DBUF) confirmed `DBUF_BUF_CFG_A_PA = 0` as the smoking gun in the FB-only case.
- **V204 ccont init hooks re-enabled:** `AppleIntelScaler::init` and `AppleIntelPlane::init` are now routed in the FBT branch so `ccont` is set into `that[+0x28]` / `that[+0x90]` at construction time (matches Visual Ehrmanntraut's working configuration). The per-method ccont patches (`disableScaler` / `enablePlane` / `programPipeScaler` / `updateRegisterCache` / `disableDisplayEngine` / `enableDisplayEngine` / `hwSetPowerWellStateAux/DDI` / `raWriteRegister32b`) remain enabled as belt-and-suspenders for any path that bypasses the constructor.
- **dp0 path (CPU compositor / `-ngreendp0`) active investigation:**
  - **V99S**: Hook in `raWriteRegister32` intercepts all `PLANE_SURF` writes ≥`0x10000000` (non-aperture) and redirects them to `0x0`; simultaneously forces `PLANE_CTL` linear and `PLANE_STRIDE=0xa0`. Keeps the display scanning BAR2 aperture while the Intel driver tries to migrate to non-aperture GGTT.
  - **V99G**: One-shot GGTT remap — when V99S first intercepts a non-aperture `PLANE_SURF` write, copies PTEs from the non-aperture surface pages (`GGTT[surfPage..surfPage+3999]`) to `GGTT[0..3999]` so that `SURF=0x0` scans the same physical pages that WindowServer's CPU compositor writes to. Confirmed visible: Apple boot flash appears briefly on dp0.
  - **V88 dp0 guard**: Added `!isDisplayPipeForceDisabled()` guard to the V88 scanout-fill block. Without this, V88 iteration 5 read the hardware `PLANE_SURF` echo (`0x412be000`) before V99S redirected it, causing V88's plane toggle to arm with the wrong surface and paint non-aperture pages with color bands (confirmed red horizontal bar artefact — now fixed).
  - **Remaining blocker**: `kIOWindowServerActiveAttribute = 0x3 → 0x1` transition fires ~215–233 ms after WindowServer goes active. After reaching state `0x1`, WS stops CPU compositing — screen freezes on the last written frame. Root cause under investigation: stamp-9 timeout in `IOAccelDisplayPipe` transactions (V55/Path D testing), possible inability to open `IOAccelDisplayPipeUserClient2`, or WS degrading due to a GPU restart signal propagation.
- **V80L fix:** `userspace watchdog timeout` KP root cause confirmed and fixed. V80L (plane-linearization writes in `v71EMrEnforcer`) limited to first 3 ticks only. Brief display flash preserved; WS crash-loop eliminated. `-ngreenv80l` enables continuous mode for testing.
- **V158:** Cancel pending execlist contexts + drain CSB after every fake-success `resetGraphicsEngine` return. Writes 4×0 to `RING_ELSP` (null EL0+EL1 context descriptors), then reads `RING_CONTEXT_STATUS_PTR` and advances read_ptr to write_ptr. Applied on both V153 and V154 return-0 paths.
- **V158:** Cancel pending execlist contexts + drain CSB after every fake-success `resetGraphicsEngine` return. Writes 4×0 to `RING_ELSP` (two null 64-bit context descriptors = cancel EL0 + EL1), then reads `RING_CONTEXT_STATUS_PTR` and writes back with read_ptr set to write_ptr. Logs `EXEC` value after the null writes to confirm whether execlist FIFO cleared. Applied on both V153 and V154 return-0 paths. Build confirmed clean. Boot test pending.
- **V157:** Removed `rcsCtl == 0` guard from V153 quiescent check — hardware restores CTL to `0x7001` before V153 reads it, so the condition was always false. Only `rcsHead == rcsTail && err == 0` is needed. Boot log confirmed: V153 now fires from call #1.
- **V156:** Fixed `RING_ENABLE` bit (ORed `| 1` at all CTL write sites). Dropped `bCtl == 0` from V154 check. Stabilized ring-enabled state across all fake-success paths.
- **V155:** Snapshot of RCS CTL (with `RING_ENABLE` forced) taken before the original `resetGraphicsEngine` call; restored on V153 and V154 return-0 paths to prevent the driver disabling the ring.
- **V154:** RPL-only circuit-breaker — after 3 consecutive quiescent `ret=1025` coercions by V153, skip calling the original `resetGraphicsEngine` entirely. Prevents the health monitor (V60M, 2 s tick × 60 = 120 s budget) from burning the full watchdog window on redundant resets.
- **V153:** Coerce `resetGraphicsEngine ret=1025 → 0` when RCS ring is quiescent (`rcsHead == rcsTail`, `ERROR_GEN6 == 0`) on non-real TGL. Increments consecutive-quiescent counter for V154 arming.
- **V152:** BCS ring drain — force `RING_TAIL ← RING_HEAD` before BCS reset to stop repeated "ring not empty" resets on RPL where the BCS engine cannot execute under the TGL driver.
- **DYLD safety cleanup:** Removed the unadmitted CoreDisplay control-flow
  patches and unreachable CoreLSKD/ICL fallbacks. DYLD now limits mutation to
  media model composite strings in shared-cache pages; TGL bundle handling is
  diagnostic only.
- **DisplayPipe defaults updated:** Native `DisplayPipeSupported` is default. `-ngreendp0` is explicit fallback.
- **V77 default policy updated:** kill delay default is full monitor window (no automatic client termination unless explicitly requested).
- **V96 connector state update:** `getOnlineInfo` forces online + changed for stronger hotplug/state propagation.
- **V110:** V59 delayed checks + V74 EMR enforcer run unconditionally on non-real TGL; V60 monitor remains opt-in via `-ngreenexp`.
- **V75:** **CRITICAL FIX** — Removed post-`awaitPublishing` property override race condition. Display output now stable and corruption-free.
- **V74:** Permanent EMR enforcer — 50ms timer runs indefinitely, keeping ERROR_GEN6 masked.
- **V111C:** For non-real TGL, original Blit3D initializer is no longer auto-selected in full-MTL mode; it is opt-in only via `-ngreenV69AllowOriginal`.

### GPU Reset Storm — Root Cause Analysis (RPL / macOS 14.7.1)

On RPL under the TGL driver spoof, Apple's `resetGraphicsEngine` returns `1025` (ETIMEDOUT) on every call because the TGL reset sequence issues commands the RPL hardware cannot execute. This triggers a `userspace watchdog timeout` panic as follows:

1. The V60M health monitor fires every ~2 s for up to 120 s (60 ticks).
2. Each tick calls `resetGraphicsEngine` — before V153/V154 this always returned `1025` and reset the watchdog counter.
3. After 120 s of no successful reset, the kernel watchdog fires.

V153/V154 break the storm by detecting a quiescent RCS ring and returning `0` (success) without touching the hardware. V154 then short-circuits all subsequent calls for the remainder of the boot session. V155/V156/V157 ensure the RCS ring stays enabled (`CTL=0x7001`) across all fake-success returns.

The remaining issue post-V157 is that `RING_EXECLIST_STATUS` (`EXEC`) stays at `0x40018098` (bit 30 = execlist queue stalled) and `RING_CONTEXT_STATUS_PTR` shows write_ptr ahead of read_ptr across all V60M ticks — meaning Apple's driver still sees pending context work and continues scheduling resets even though V154 returns instantly. V158 targets this by nulling the execlist submission port and draining the CSB on every fake-success return.

Key registers (all offsets from `RENDER_RING_BASE = 0x2000`):

| Register | Offset | Role in fix |
|----------|--------|-------------|
| `RING_ELSP` | `+0x230` | Write 4×0 to cancel pending EL0+EL1 context descriptors |
| `RING_EXECLIST_STATUS` | `+0x234` | Bit 30 = execlist FIFO stalled; cleared by ELSP null writes |
| `RING_CONTEXT_STATUS_PTR` | `+0x3a0` | Bits[15:8]=write_ptr, [7:0]=read_ptr; advance read→write to drain CSB |
| `RING_CTL` | `+0x3c` | Bit 0 = RING_ENABLE; V155 saves and restores this on every fake-success |
| `ERROR_GEN6` | `0x40A0` | Must be 0 for V153 quiescent condition |

### Current Status

System boots to login on RPL macOS 14.7.1 (`23H222`). GPU reset storm tamed (V153/V154). `userspace watchdog timeout` KP fixed (V80L). CTL stable at `0x7001`.

**dp0 investigation** (`-ngreendp0`): V99G GGTT remap + V99S PLANE_SURF redirect confirmed working — boot flash (Apple logo) is visible briefly with linear+stride 0xa0 force. Root cause of black screen with X-tiled CTL identified: in FB-only mode the GPU tile engine is not running, so physical pages are written linearly by the CPU compositor while `PLANE_CTL` programs X-tiled scanout → display engine reads wrong bytes → black. Linear+0xa0 produces a visible but fragmented ("8 copies") image because the EFI/stolen-memory pages at `GGTT[0..3999]` contain linearly-written data that the HW interprets at the wrong stride. Multi-buffer GGTT mapping now available via `ngreen-buf=N` — each unique non-aperture IOSurface is assigned a fixed aperture slot so double/triple-buffered WS flips are routed to independent `GGTT[slot*4000..]` ranges and `PLANE_SURF` is rewritten to the matching slot address. Active blocker: `kIOWindowServerActiveAttribute 0x3→0x1` ~215–233 ms after WS goes active; WS stops compositing after that transition. V55/Path D testing in progress.

V88 visual fill remains opt-in only (`-ngreenv88`). Default boots preserve normal Apple UI layout.

## Requirements

- [Lilu](https://github.com/acidanthera/Lilu) 1.7.2+
- macOS Tahoe 26.x for the current SR-IOV work; older targets are historical
  and require their own validation
- Supported Intel iGPU (see **Compatibility** below)
- Discrete GPU disabled via SSDT (recommended) or `disable-gpu` DeviceProperty on its PCI path

For current RPL test configuration, OpenCore `DeviceProperties` IGPU injection is required:

- `PciRoot(0x0)/Pci(0x2,0x0)`
- `AAPL,ig-platform-id` = `AABJmg==` (0x9A490000 in little-endian)
- `device-id` = `SZoAAA==` (0x9A490000 in little-endian)
- `force-online` = `AQAAAA==` (enable force-online WEG patch)
- `complete-modeset` = `AQAAAA==` (enable complete-modeset WEG patch)
- `rps-control` = `AQAAAA==` (enable rps-control WEG patch)
- `built-in` = `AA==`
- `AAPL,slot-name` = `built-in`
- `hda-gfx` = `onboard-1`

These properties are essential for correct platform identification and WEG coexistence mode on RPL with TGL driver spoof.

## Boot args

Boot args advised for testing (Hookcase in `/Library/Extensions/` too):

FB-only:
```
-v keepsyms=1 debug=0x100 IGLogLevel=8 -ngreentglfb -NGreenDebug liludump=250 msgbuf=725288 liludbuf=725288 ngreen-dmc=adlp
```

FB+GFX:
```
-v keepsyms=1 debug=0x100 IGLogLevel=8 -ngreentglwithgfx -NGreenDebug liludump=250 msgbuf=725288 liludbuf=725288 ngreen-dmc=adlp -allow3d -disablegfxfirmware -ngreenfullmtlcore
```

Where:
// Boot-arg "ngreen-dmc":
//   not set or "skip" → safe fallback: passthrough original + AUX only (proven working)
//   "tgl"             → load TGL DMC v2.12 blob + TGL display engine registers
//                       + ICL/TGL combo PHY signal levels (PHY_A eDP, PHY_B DP)
//   "adlp"            → load ADL-P DMC v2.16 blob + ADL-P display engine registers
//                       + combo PHY signal levels (PHY_A eDP)
//   "icl"             → passthrough original ICL DMC load + ICL combo PHY signal levels

> **Note:** `-ngreentglwithgfx` is required on Gen11/TGL hardware to load BOTH the TGL framebuffer AND the TGL HW (accelerator) kext. FB-only mode (`-ngreentglfb`) does not allocate per-plane DBUF on Gen11+ — the watermark/DBUF programming pipeline lives in the HW kext (`AppleIntelTGLGraphics.kext`), so without it the display engine fetches from a zero-block DBUF range and produces duplicated/fragmented output. `-ngreendp0 -ngreenv88` are diagnostic flags for the dp0/CPU-compositor investigation path. Remove them for normal operation.

- **Note:** NootedGreen does not redirect userspace GPU-bundle lookup paths.
  Bundle deployment and trust must be handled explicitly for the target OS.

| Arg | Purpose |
|---|---|
| `-NGreenDebug` | Enable NootedGreen debug logging |
| `-ngreentglfb` | Load only the TGL framebuffer kext (FB-only mode). On Gen11+, this is generally NOT enough for a coherent display because per-plane DBUF allocation is HW-kext side. Diagnostic / FB-driver-isolation use only. |
| `-ngreentglwithgfx` | Load both the TGL framebuffer AND the TGL HW accelerator kext. **Recommended for normal operation on TGL/RPL hardware.** Pairs the FB driver with `AppleIntelTGLGraphics.kext` so the watermark/DBUF programming pipeline runs at mode-set time. |
| `-ngreentglgfx` | Load only the TGL HW kext, no FB. Diagnostic — hardware will not display anything without an FB driver. |
| `-ngreenicl` | Load the legacy ICL framebuffer + HW kexts instead of TGL. For older Gen11 hardware where TGL spoof isn't suitable. |
| `-disablegfxfirmware` | Disable GuC/HuC firmware loading — required on RPL/ADL because scheduler selection (`ngreenSched`) happens after the HW-branch `processKext`, so the driver attempts firmware init before NootedGreen can override the scheduler type. Not needed on real TGL (GuC loads natively). |
| `-ngwegcoex` / `ngwegcoex=1` | Enable WEG coexistence mode. |
| `ngreenSched=N` | Select GPU scheduler type: `3` = GuC firmware, `4` = IGScheduler4, `5` = host preemptive (default: `3` on real TGL, `5` on RPL/ADL) |
| `ngreen-dmc=skip|tgl|adlp` | DMC policy: skip CSR load, or force TGL/ADL-P DMC path for diagnostics. |
| `-allow3d` | Force 3D acceleration |
| `-nbdyldoff` | Disable the optional shared-cache media-model patches and TGL userspace-bundle discovery logs. |
| `-ngreenexp` / `ngreenexp=1` | Enable experimental runtime monitor/timer paths (V60 monitor and extra diagnostics) |
| `-ngreenv60` / `ngreenv60=1` | Force-enable V60 monitor. |
| `-ngreenv60off` / `ngreenv60off=1` | Force-disable V60 monitor. |
| `-ngreenv60rw` / `ngreenv60rw=1` | Enable aggressive V60 MMIO write path. |
| `-ngreendp0` / `ngreendp0=1` | Force fallback mode: set `DisplayPipeSupported=0` in accelerator capabilities |
| `-ngreendp1` / `ngreendp1=1` | Explicitly keep native `DisplayPipeSupported` path (default behavior) |
| `ngreenV77DelayKill=N` | Delay V77 display-pipe client termination by `N` monitor iterations (`0..60`, default `60` = effectively disabled) |
| `-ngreenv88` / `ngreenv88=1` | Enable V88 scanout fill + plane toggle diagnostics (draws test bars/colors; off by default) |
| `-ngreenv93` / `ngreenv93=1` | Enable V93 plane guard diagnostics (disabled by default). |
| `-ngreenfullmtl` / `ngreenfullmtl=1` | Kernel-side full-Metal policy override. It does not enable a DYLD control-flow patch and does not auto-enable Apple's original Blit3D initializer. |
| `-ngreenfullmtlcore` / `ngreenfullmtlcore=1` | Equivalent kernel-side-only full-Metal policy override; the unified `-ngreenfullmtl` remains a fallback. |
| `-ngreenV69AllowOriginal` | Opt in to Apple's original Blit3D initialize on non-real TGL when safety preconditions are met. **High risk / diagnostic only**; can panic on unsupported setups. |
| `-ngreenV69SkipOriginal` | Hard-disable Apple's original Blit3D initialize on non-real TGL, even if `-ngreenV69AllowOriginal` is present. |
| `-ngreenV69ForceOriginalUnsafe` | Override safety rejection and force original Blit3D init on spoofed RPL/ADL (crash-debug only). |
| `-ngreenv80l` / `ngreenv80l=1` | Run V80L plane-linearization writes continuously (every 50ms) instead of the default first-3-ticks-only behavior. **Testing only** — causes WS crash-loop + watchdog KP on normal boots. |
| `ngreen-buf=N` | GGTT multi-buffer slots for the dp0 SURF-redirect path: `1`=single, `2`=double (default), `3`=triple. Each slot occupies 4000 GGTT pages (0xFA0000 bytes). Slot 0 → `SURF=0x0`, slot 1 → `SURF=0xFA0000`, slot 2 → `SURF=0x1F40000`. Apple's non-aperture IOSurface pages are remapped into their assigned slot on every flip; SURF is rewritten to the matching aperture address. Single-buffer collapses all flips to slot 0 (original behaviour). Double/triple allow the display engine to scan independent physical pages per IOSurface without cross-contamination. |
| `-ngreenforceprops` / `ngreenforceprops=1` | Enable legacy forced IGPU property injection (`AAPL,ig-platform-id`, `model`, `saved-config`, etc.). Disabled by default in compatibility-first mode. |
| `IGLogLevel=8` | Maximum Intel GPU driver logging |
| `-liludbg` | Enable Lilu debug logging |
| `liludump=N` | Dump Lilu logs after `N` seconds (example: 125 or 200). |

## Hookcase

Hookcase (change `AppleInteePortHal` and `AppleIntelPortHal` implementation):

```cpp
// Register selection by platform:
//   ICL  (AppleIntelFramebufferController path): 0xC4030 (ICL_SHOTPLUG_CTL_DDI)
//   TGL:                                         0x44470
//   ADL-P / RPL-P:                               0x1638a0
uint32_t registerValue = callback->readReg32(0x1638a0); // ADL-P/RPL-P
```

## Useful logs or kp log to debug:

need your logs or kernel panic log.. without them I cannot do anything...
so you must boot with - for example an empty nblue - inside the system and get previous lilulog and logs related THAT boot.

```bash
log show --style syslog --predicate 'processID == 0' --last 15m --info --debug > /tmp/x.log
grep "[IGFB]" /tmp/x.log > /tmp/fb.log
```

Example Lilu log path:

```text
/private/var/log/Lilu_1.7.2_23.6.txt
/Libtary/Logs/DiagnosticReports/..
```

Additional developer note:

```text
[N.B. library is in continue developement]

Developer from all over the world, are you ready?  Still needs some patches to my fully working RPL-P laptop but... it's open to developer now.
No more IOPCIPrimaryMatch, we work on IOResource so just put your kexts (+ bundle) in /Library/Extensions folder or use /System/Library/Extensions kext

https://github.com/sgiammori/NootedGreen

IOResources solving now is done like this for TGL kexts or IcL ketxts : first look at LE kexts and if any kexts is found than fallback to find in SLE kexts : * framebuffer for fb * + * graphics for gpu * + * bundle for metal *

=> they need permissions fix (also Hookcase) so before move to /L/E do in some random folder, check below
=> IOPCIPrimarymatch must be set in both *TGLGraphics* kexts in /Library/Extensiona
```

## Workflow for kexts (fb+Graphics+Hookcase in LE)

```
- sudo chmod -R 755 Apple*
- sudo chown -R root:wheel Apple*
- move the files to /L/E
- delete /Library/KernelCollections/AuxiliaryKernelExtensions.kc
- redo sudo chown -R root:wheel /Library/Extensions/Apple*
- sudo kextcache -i /
(if System asks you permissions, just allow and restart before to test)

(maybe necessary this below also)

sudo kmutil load -p /Library/Extensions/AppleIntelTGLGraphics.kext 2>&1
sudo kextcache -i /
```

## Compatibility-First Defaults

Recent changes switch NootedGreen to safer defaults for cross-machine portability:

- Legacy hardcoded IGPU property seeding is now **opt-in**, not default.
- Experimental watchdog/monitor logic is still **opt-in** via `-ngreenexp`.
- Native display-pipe path is now default; forced fallback mode is opt-in via `-ngreendp0`.
- Coexistence paths avoid forcing DVMT/framebuffer processing when the DVMT module is not enabled.

This reduces machine-specific assumptions in default boots and keeps aggressive behavior available only when explicitly requested for debugging.

## GPU Scheduler

The Intel TGL graphics driver supports three scheduler types, selectable at boot:

| Type | Name | Description |
|------|------|-------------|
| 3 | **GuC firmware** | Default Apple scheduler — loads GuC binary firmware. Requires matching firmware blobs. |
| 4 | **IGScheduler4** | Intermediate scheduler. |
| 5 | **Host preemptive** | Host-based scheduler — no firmware required. Ring command streamer managed by the driver. **Recommended for unsupported hardware.** |

**Selection priority:**

1. Boot argument `ngreenSched=N` (highest priority)
2. `SchedulerType` key in Info.plist (NootedGreen personality)
3. Default: `3` (GuC firmware) on real TGL, `5` (host preemptive) on RPL/ADL

Type 5 bypasses `IGScheduler::initFirmware()` entirely, avoiding GuC/HuC binary loading which fails on spoofed devices. The RCS ring is initialized directly by the host driver.

## Native Tiger Lake path selection

The compatibility field `isRealTGL` is true only when the original PCI device
ID is a known Tiger Lake ID **and** physical-function ownership has been
established. It is false for every VF, including a Tiger Lake VF. CPUID is
logged for diagnostics and never selects GPU register layouts or PF/VF policy.

When `isRealTGL = false`, generation- and VF-specific paths may be considered,
but each invasive path must still pass its own PCI, VF capability, binary UUID
and runtime-state admission as appropriate. Historical examples include:

- **Topology overrides** — L3 bank count, max EU count, subslice count hardcoded for 96EU RPL config
- **BCS engine bypass** — skip blitter engine init in `hwDevStart` (RPL BCS is dead under TGL driver)
- **GuC binary stub** — `loadGuCBinary` returns 1 instead of loading firmware (wrong microarch)
- **MultiForceWakeSelect=1** — redirect ForceWake to hooked `SafeForceWakeMultithreaded` (RPL ACK=0 on native path)
- **BCS engine reset** — stop+clear dead BCS ring after `start()` (V51)

When `isRealTGL = true`, spoof-path overrides are skipped and the Apple Tiger
Lake driver remains on its native physical-GPU path.

### Required GPU driver bundles

For GPU acceleration, the following userspace driver bundles must be installed in `/Library/Extensions/`:

| Bundle | Purpose |
|--------|---------|
| `AppleIntelTGLGraphicsMTLDriver.bundle` | Metal driver |
| `AppleIntelTGLGraphicsGLDriver.bundle` | OpenGL driver |
| `AppleIntelTGLGraphicsVADriver.bundle` | Video Acceleration driver |
| `AppleIntelTGLGraphicsVAME.bundle` | VA Media Engine |
| `AppleIntelGraphicsShared.bundle` | Shared graphics library |

These bundles are loaded by name (via `MetalPluginName`, `IOGLBundleName`, etc.), not by `CFBundleIdentifier`. They are not shipped with NootedGreen and must be sourced separately.

### Driver path resolution and fallback order

NootedGreen keeps a strict load-path policy for Gen11/Gen12 bring-up:

- **Primary (preferred): TGL from `/Library/Extensions`**
	- `AppleIntelTGLGraphicsFramebuffer.kext`
	- `AppleIntelTGLGraphics.kext`
- **Fallback: ICL from `/System/Library/Extensions`**
	- `AppleIntelICLLPGraphicsFramebuffer.kext`
	- `AppleIntelICLGraphics.kext`

Runtime guards enforce this behavior:

- If TGL framebuffer loads, ICL framebuffer processing is skipped.
- If TGL accelerator loads, ICL accelerator processing is skipped.

NootedGreen no longer rewrites `gpu_bundle_find_trusted`; the operating system's
normal bundle search and trust policy determines whether these bundles load.

## Compatibility

NootedBlue legacy support (fully preserved):

- **Haswell** (10.12+)
- **Broadwell / Braswell** (10.14+)
- **Gemini Lake** (10.14+)

NootedGreen (Gen11/Gen12 — TGL driver spoofing):

| Platform | Status | Est. | Notes |
|----------|--------|------|-------|
| **Tiger Lake** | Experimental | — | Native path requires a known TGL PCI identity and PF ownership. No real TGL hardware validation is claimed. |
| **Raptor Lake-P** | ~70% | V80L | Primary dev platform (i7-13700H). System boots to login on macOS 14.7.1 (`23H222`). GPU reset storm tamed: V153/V154 circuit-breaker confirmed working. `userspace watchdog timeout` KP root cause identified and fixed: V80L plane-linearization in `v71EmrEnforcer` was fighting WindowServer over plane registers every 50ms — now limited to first 3 ticks. Brief display flash at boot preserved. Active work: V158 execlist/CSB drain. |
| **Alder Lake** | ~35% | — | Same Gen12 arch as RPL, should behave similarly. Untested. |
| **Rocket Lake** | ~25% | — | Gen12 LP but different display engine. Untested. |
| **Ice Lake** | Experimental | — | Dedicated ICL kernel paths exist, but the former unversioned userspace Metal ID bypass was removed. Untested on real ICL hardware. |

## Building

Open `NootedGreen.xcodeproj` and select the **NootedGreen** scheme to build the Gen11/Gen12 plugin, or one of the original NootedBlue schemes for legacy hardware. Build with Xcode.

## Authors

- **Stefano Giammori** ([@sgiammori](https://github.com/sgiammori)) — reverse engineering, driver development, hardware testing

## Thanks to..

- **Visual Ehrmanntraut** — author of the upstream ChefKiss code this project builds on
- [@shl628](https://github.com/lshbluesky)
- [@jalavoui](https://github.com/macintelk) — "big jala" developer of NootedBlue
- **Claude Code** (Claude Opus 4.7 + Claude Sonnet 4.6) — AI pair-programming, code generation, debug analysis

## License

[Thou Shalt Not Profit License 1.0](LICENSE)
