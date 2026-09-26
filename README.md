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

### Current safety and validation state

This branch is under protocol review for the pinned macOS Tahoe TGL
accelerator payload. It is not a general support declaration and it does not
inherit runtime claims from the older Sonoma/Raptor Lake display experiments.

- An identified SR-IOV VF rejects the physical framebuffer driver and uses the
  PF-owned GuC transport, memory-IRQ page and assigned GGTT range.
- VF scheduler selection, PM/fallback disablement, bootstrap symbols and
  accelerator start/stop routes are mandatory. Missing private ABI state fails
  admission instead of falling back to physical MMIO.
- Native blit and barrier producers run only when GGTT, CTB, memory IRQ and
  shutdown gates are all ready. Their real native result is propagated.
- Physical engine start/stop/reset-list handling is native. The former GDRST,
  blanket EMR masking, ring rewrites, GGTT[0] diagnostic remaps and recurring
  health-monitor timers have been removed.
- Static analyzers and exhaustive host-side protocol models pass. This is not a
  Metal, media, DMA-quiescence or display validation result.
- The Tahoe VM remains off until the active route and byte-patch audit has no
  open safety blocker. Runtime results will be recorded only after a controlled
  boot against the pinned payload.

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
-v keepsyms=1 debug=0x100 IGLogLevel=8 -ngreentglwithgfx -NGreenDebug liludump=250 msgbuf=725288 liludbuf=725288 ngreen-dmc=adlp -allow3d -disablegfxfirmware
```

Where:
// Boot-arg "ngreen-dmc":
//   not set or "skip" → safe fallback: passthrough original + AUX only (proven working)
//   "tgl"             → load TGL DMC v2.12 blob + TGL display engine registers
//                       + ICL/TGL combo PHY signal levels (PHY_A eDP, PHY_B DP)
//   "adlp"            → load ADL-P DMC v2.16 blob + ADL-P display engine registers
//                       + combo PHY signal levels (PHY_A eDP)
//   "icl"             → passthrough original ICL DMC load + ICL combo PHY signal levels

> **Note:** `-ngreentglwithgfx` loads both the physical TGL framebuffer and
> accelerator and is not the VF topology. An SR-IOV VF is accelerator-only and
> rejects the physical framebuffer during admission.

- **Note:** NootedGreen does not redirect userspace GPU-bundle lookup paths.
  Bundle deployment and trust must be handled explicitly for the target OS.

| Arg | Purpose |
|---|---|
| `-NGreenDebug` | Enable NootedGreen debug logging |
| `-ngreentglfb` | Load only the TGL framebuffer kext (FB-only mode). On Gen11+, this is generally NOT enough for a coherent display because per-plane DBUF allocation is HW-kext side. Diagnostic / FB-driver-isolation use only. |
| `-ngreentglwithgfx` | Load both the TGL framebuffer AND the TGL HW accelerator kext. **Recommended for normal operation on TGL/RPL hardware.** Pairs the FB driver with `AppleIntelTGLGraphics.kext` so the watermark/DBUF programming pipeline runs at mode-set time. |
| `-ngreentglgfx` | Load only the TGL HW kext, no FB. Diagnostic — hardware will not display anything without an FB driver. |
| `-ngreenicl` | Load the legacy ICL framebuffer + HW kexts instead of TGL. For older Gen11 hardware where TGL spoof isn't suitable. |
| `-disablegfxfirmware` | Physical-path diagnostic only. A VF uses the PF-owned GuC image and must not upload firmware. |
| `-ngwegcoex` / `ngwegcoex=1` | Enable WEG coexistence mode. |
| `ngreenSched=N` | Select GPU scheduler type: `3` = GuC firmware, `4` = IGScheduler4, `5` = host preemptive (default: `3` on real TGL, `5` on RPL/ADL) |
| `ngreen-dmc=skip|tgl|adlp` | DMC policy: skip CSR load, or force TGL/ADL-P DMC path for diagnostics. |
| `-allow3d` | Force 3D acceleration |
| `-nbdyldoff` | Disable the optional shared-cache media-model patches and TGL userspace-bundle discovery logs. |
| `-ngreendp0` / `ngreendp0=1` | Force fallback mode: set `DisplayPipeSupported=0` in accelerator capabilities |
| `-ngreendp1` / `ngreendp1=1` | Explicitly keep native `DisplayPipeSupported` path (default behavior) |
| `-ngreenv93` / `ngreenv93=1` | Enable V93 plane guard diagnostics (disabled by default). |
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
- Recurring watchdog/monitor experiments are not part of the maintained driver.
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

NootedGreen Gen11+ paths are experimental and admitted by explicit hardware and
binary identity, not by an estimated completion percentage:

| Platform | Current boundary |
|----------|------------------|
| **Tiger Lake PF** | Native physical path exists; no current hardware validation is claimed. |
| **Raptor Lake SR-IOV VF (8086:a7a8)** | Static protocol target for the pinned Tahoe TGL payload. Controlled VM acceleration testing has not started. |
| **Raptor/Alder/Rocket Lake PF** | Not claimed supported; unsafe legacy reset and partial-context workarounds were removed. |
| **Meteor/Arrow Lake VF** | Explicitly rejected until per-GT/media-13 GGTT discovery is implemented. |
| **Ice Lake PF** | Dedicated ICL path exists but has not completed this Tahoe review. |

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
