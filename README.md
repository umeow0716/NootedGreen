# NootedGreen

Lilu plugin that runs Apple's Tiger Lake graphics driver on an Intel Xe-LP
SR-IOV virtual function inside a macOS Tahoe virtual machine.

This fork targets one setup: a Raptor Lake-P iGPU (i7-13620H, `8086:a7a8`)
whose VF is passed to a QEMU/KVM guest by a Linux host running
[i915-sriov-dkms](https://github.com/strongtz/i915-sriov-dkms). Other Xe-LP
GPUs may work but have not been tried.

## Status

Working on macOS Tahoe 26.7 (`25G229`):

- Metal (WindowServer compositing, Metal apps)
- H.264 hardware decode and encode through VideoToolbox
- Looking Glass output at 1920x1080 144 Hz (separate `lg-macos` project)

Known issues:

- The first decoded frame after boot can come out black.
- Only macOS Tahoe is supported. On a minor update the kext checks the
  IOAcceleratorFamily2 layout it depends on and boots without acceleration if it
  changed. Only `25G229` has been tested.
- Occasional GuC context resets on the video engine under long encode runs.

## Requirements

- macOS Tahoe 26.7 (`25G229`)
- [Lilu](https://github.com/acidanthera/Lilu) 1.7.2 or newer
- Tiger Lake kexts and user-space bundles from an older macOS release, installed
  in `/Library/Extensions` (not included):
  `AppleIntelTGLGraphics.kext`, `AppleIntelTGLGraphicsMTLDriver.bundle`,
  `AppleIntelTGLGraphicsGLDriver.bundle`, `AppleIntelTGLGraphicsVADriver.bundle`,
  `AppleIntelTGLGraphicsVAME.bundle`, `AppleIntelGraphicsShared.bundle`
- Host: i915-sriov-dkms with VFs enabled. Without
  strongtz/i915-sriov-dkms#512 the PF logs DMAR faults at address 0 while the
  guest GPU is busy.

User-space pieces built from this repo:

- `NGShim`: replaces `impostor.dylib` in the TGL Metal driver bundle
  (`NGShim/install.sh`).
- `tools/ngva`: VideoToolbox front end plus patched TGL VA driver, installed as
  `/Library/GPUBundles/NootedGreenTGLMediaAdapter.bundle` (`tools/ngva/install.sh`).

## Boot args

Typical set:

```
-ngreentglgfx ngreenSched=4 -ngreenmediaadapter -ngtglearly
```

| Arg | Purpose |
|---|---|
| `-ngreentglgfx` | Load the TGL accelerator kext without the framebuffer (the VF has no display engine). |
| `ngreenSched=N` | Scheduler type. A VF always uses `4`. |
| `-ngreenmediaadapter` | Point VideoToolbox at `NootedGreenTGLMediaAdapter.bundle`. |
| `-ngtglearly` | Load the TGL kext early in boot. |
| `-ngiclva` | Use Tahoe's own `AppleIntelICLGraphicsVADriver` for VideoToolbox instead. H.264 hardware decode works; hardware encode does not yet. |
| `-ngvadump` | Log the first few video decode batches. |
| `-NGreenDebug` | Debug logging. |
| `-ngreentglfb`, `-ngreentglwithgfx` | Physical GPU only: load the TGL framebuffer as well. A VF refuses these. |

## Building

Run `tools/fetch-deps.sh` once to download Lilu and MacKernelSDK, then build the
**NootedGreen** scheme of `NootedGreen.xcodeproj` with Xcode.

## Authors

- **Stefano Giammori** ([@sgiammori](https://github.com/sgiammori)) — original NootedGreen
- **umeow0716** ([@umeow0716](https://github.com/umeow0716)) — SR-IOV VF and macOS Tahoe support

## Thanks to

- **Visual Ehrmanntraut** — author of the upstream ChefKiss code this project builds on
- [@shl628](https://github.com/lshbluesky)
- [@jalavoui](https://github.com/macintelk) — developer of NootedBlue

## License

[Thou Shalt Not Profit License 1.0](LICENSE)
