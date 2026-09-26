# NootedGreen

Experimental Lilu plugin for newer Intel iGPU bring-up on macOS using an
explicitly admitted Tiger Lake driver payload. Native Ice Lake drivers are not
intercepted.

## What it does

Adapts an admitted Apple Tiger Lake graphics payload to newer Intel iGPUs.
Physical hardware retains Apple's native engine, firmware, force-wake and display
lifecycle. An identified SR-IOV VF instead uses its PF-provisioned GGTT, GuC,
memory interrupt and direct-LRCA transport.

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
enabled dual subslices, 16 EUs per DSS, and 64 EUs total. The VF no longer
embeds those numbers: during its one-shot GuC bootstrap it negotiates Intel's
VF/PF MMIO-relay ABI 1.0 and asks the PF for the allow-listed topology, media
engine and timestamp-clock fuses. Invalid or internally inconsistent replies
fail admission before Apple's accelerator personality is published.

The Tahoe `25G229` VF bootstrap fixes on this branch are deliberately narrow:

- V213 converts the VF's missing stolen-memory GGTT range into the 4 GiB
  aperture represented by its 8 MiB GGTT BAR0 window. It does not falsify the
  memory manager's stolen-memory range.
- V214 treats a task as the bootstrap kernel task only for a VF proven by
  VF_CAP and only while `IntelAccelerator+0x150` is unassigned, avoiding the
  null kernel-task clone path in `newPageTableForTask`. A later-generation PF
  always keeps Apple's native classification.
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
- The admitted media-12 VF path writes the separately validated BAR0 PTE
  aperture directly. The former uninitialized software-shadow/VF2PF-relay
  fallback has been removed; media-13 remains fail-closed until its per-GT
  discovery and GGTT update ABI are implemented.
- All direct-VF map, rotated-map, dummy-map and unmap PTE writes use one aligned
  64-bit store. Physical-driver cache attributes are validated but omitted:
  on media-12 GGTT those positions select PF-owned VFID/local-memory state.
  Once CTB has run, every mapping mutation waits for a heavy GuC invalidation.
- VF scheduler selection, PM/fallback disablement, bootstrap symbols and
  accelerator start/stop routes are mandatory. Missing private ABI state fails
  admission instead of falling back to physical MMIO.
- Apple's pinned `getGPUInfo()` receives the PF-relayed slice, DSS, EU, media
  engine and timestamp-clock values at its five raw-register load sites. L3
  bank count is derived from the relayed mirror fuse; no PCI-ID topology table
  or target-specific 64-EU constant remains.
- Native blit and barrier producers run only when GGTT, CTB, memory IRQ and
  shutdown gates are all ready. Their real native result is propagated.
- The direct CTB producer admits only the six GuC v70 FAST request shapes this
  VF bridge implements. Exact request bodies determine retirement permission
  and G2H credit reservations before anything is written to the transport.
- Direct-LRCA attach validates every persistent Tahoe descriptor field before
  native pool access. GuC registration materializes the first-submit
  `FORCE_RESTORE` bit that Apple's legacy execlist adds only to a stack copy;
  reserved bits, unsupported classes/instances and malformed priorities fail
  before transport publication.
- Physical engine start/stop/reset-list handling is native. The former GDRST,
  blanket EMR masking, ring rewrites, GGTT[0] diagnostic remaps and recurring
  health-monitor timers have been removed.
- The optional physical TGL framebuffer path admits only the two audited Tahoe
  16.0.32 UUIDs. Its former platform/topology/power/link-training control-flow
  patches and cross-object accessor repairs are removed; only an exact-count
  `ReadRegister64` eight-byte bounds correction remains. A VF rejects this
  framebuffer path and does not acquire a display engine from it.
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
-v keepsyms=1 debug=0x100 IGLogLevel=8 -ngreentglfb -NGreenDebug liludump=250 msgbuf=725288 liludbuf=725288
```

FB+GFX:
```
-v keepsyms=1 debug=0x100 IGLogLevel=8 -ngreentglwithgfx -NGreenDebug liludump=250 msgbuf=725288 liludbuf=725288 -disablegfxfirmware
```

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
| `-disablegfxfirmware` | Physical-path diagnostic only. A VF uses the PF-owned GuC image and must not upload firmware. |
| `ngreenSched=N` | Select GPU scheduler type: `3` = GuC firmware, `4` = IGScheduler4, `5` = host preemptive (default: `3` on real TGL, `5` on RPL/ADL) |
| `-nbdyldoff` | Disable the optional shared-cache media-model patches and TGL userspace-bundle discovery logs. |
| `-ngreendp0` / `ngreendp0=1` | Force fallback mode: set `DisplayPipeSupported=0` in accelerator capabilities |
| `-ngreendp1` / `ngreendp1=1` | Explicitly keep native `DisplayPipeSupported` path (default behavior) |
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

The maintained Gen11+ path loads the audited TGL kexts and userspace bundles from `/Library/Extensions`.

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
| 5 | **Host preemptive** | Physical host-managed scheduler. It is never valid for an SR-IOV VF. |

**Selection priority:**

1. A classified VF always selects type 4; no boot/property override can enter a physical scheduler
2. Boot argument `ngreenSched=N` for a physical GPU
3. `SchedulerType` key in Info.plist for a physical GPU
4. Default: `3` on a native TGL PF, `5` on a later-generation PF

## Native Tiger Lake path selection

The compatibility field `isRealTGL` is true only when the original PCI device
ID is a known Tiger Lake ID **and** physical-function ownership has been
established. It is false for every VF, including a Tiger Lake VF. Code that
selects an instruction or topology layout therefore checks the original GPU
generation separately; code that selects ownership checks PF/VF identity.
CPUID is diagnostic only.

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

### Driver path resolution

NootedGreen keeps a strict load-path policy for the reviewed bring-up path:

- **TGL from `/Library/Extensions`**
	- `AppleIntelTGLGraphicsFramebuffer.kext`
	- `AppleIntelTGLGraphics.kext`

The former `-ngreenicl` branch applied Sonoma byte sequences, fixed topology
and private firmware hooks to an unversioned payload and has been removed.
Actual Ice Lake hardware remains on Apple's native, unmodified driver path.

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
| **Ice Lake PF** | Natively supported by Apple; NootedGreen does not intercept its framebuffer or accelerator. |

The VF physical-TLB bridge no longer sweeps all `0xCEE8` instructions in the
accelerator image. Live native callers are patched inside solved symbol bounds,
fully replaced callers remain untouched, and the base `IGGuC` invalidator is
routed to the synchronous GuC v70 request after CTB readiness. The on-disk
inventory test pins every physical-TLB access in both admitted Tahoe payloads.
This strengthens the static isolation boundary; it is not yet a runtime
hardware-acceleration claim.

Every remaining accelerator/framebuffer instruction patch is likewise limited
to a solved owning-function range and checked against the admitted on-disk
payload. Whole-image searches remain only for non-instruction metadata such as
the AGDP board-id key.

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
