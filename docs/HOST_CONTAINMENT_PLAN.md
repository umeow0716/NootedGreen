# Host containment plan for VF runtime validation

Updated: 2026-09-27. This document is a fail-closed plan, not authorization to
run the VM. The `ce166c8` candidate caused PF `00:02.0` DMAR write faults, an
i915 hang and a host reboot. A later reboot has also been reported by the user.
Until every precondition below is independently satisfied, only offline source,
binary, build and protocol validation is permitted.

## Current hard hold

- Libvirt domain `macos-tahoe-sriov` is shut off, persistent, has 16 vCPUs and
  16 GiB RAM, and has autostart disabled. It has no managed-save image.
- PF `0000:00:02.0` (`8086:a7a8`) is bound to i915. VF `0000:00:02.1` is bound
  to vfio-pci. The PF and VF are in IOMMU groups 0 and 19 respectively, and the
  PF currently exposes one of seven possible VFs.
- The domain XML now has one-shot `destroy` lifecycle actions, but its Q35
  iTCO watchdog was normalized back to `action='reset'` when libvirt defined a
  watchdog-free candidate. The reset action could silently exercise the VF a
  second time, so this configuration is not a runtime-test candidate.
- Current-boot kernel health has not been proven. Unprivileged journal access
  cannot read the kernel log and non-interactive sudo requires a password.
  Therefore a preflight must fail closed until a user-authorized privileged
  watcher can read the host kernel journal.
- No candidate kext/AuxKC may be installed or loaded, no VM may be started, no
  PCI driver may be rebound and no SR-IOV sysfs value may be written while this
  hold remains in force.
- The host-crash image was found with one missing qcow2 data-cluster refcount.
  A byte-identical sparse backup was preserved, `qemu-img check -r all`
  rebuilt the original image's refcount structure, and a second read-only
  check now reports zero errors.  Future preflight must reject any writable
  qcow2 that fails its own read-only integrity check.

## Preconditions for a future controlled run

1. Record the exact source commit, CI run, artifact zip SHA-256, kext executable
   UUID/SHA-256, final-path-built AuxKC SHA-256 and guest EFI backup. Reject any
   mismatch before the domain starts. The runtime manifest format is pinned by
   `HOST_RUNTIME_MANIFEST.example.tsv`; the controller verifies it once before
   creating its transient service and again inside that service against a fresh
   inactive XML snapshot. The Git worktree must also be clean at the recorded
   commit.
2. Re-review all host-dangerous MMIO/DMA paths reachable from the candidate's
   retained native bootstrap and stop lifecycles. The complete offline suite and
   native macOS build/link must pass at that exact commit.
3. Use a disposable libvirt XML checkpoint with autostart disabled, no managed
   save, exactly one Q35 iTCO watchdog with `action='none'`, and all three
   lifecycle policies set to one-shot behavior:
   `<on_poweroff>destroy</on_poweroff>`, `<on_reboot>destroy</on_reboot>` and
   `<on_crash>destroy</on_crash>`. Libvirt has no `destroy` watchdog action;
   Q35 also normalizes an omitted integrated iTCO device back to
   `action='reset'`. The first run therefore keeps that one device inert with
   `action='none'` and relies on the independent host deadline below. Confirm
   the effective live XML before the run and restore the reviewed persistent
   XML after evidence capture.
4. Hold a host sleep inhibitor before VM start and through the full post-stop
   cooldown. The previous incident entered suspend after the PF hang and then
   logged VF pause timeout/ENOMEM, so suspend must not overlap a VF test.
5. Start an independent privileged host watcher before QEMU. It must consume
   new kernel messages from `journalctl -k -f -n0`, write them to a timestamped
   evidence directory outside the guest, and remain alive if QEMU or the guest
   fails.
6. The watcher must treat any new PF `00:02.0` DMAR/IOMMU fault, `i915` GPU HANG,
   engine reset timeout, fence timeout, GuC timeout or VF pause timeout as a
   containment trigger. On the first trigger it must issue an explicit
   `virsh -c qemu:///system destroy macos-tahoe-sriov`, then verify within a
   bounded deadline that the domain is off. It must preserve all logs and must
   not automatically start a second run.
7. A separate monotonic deadline must destroy the domain even when no trigger
   appears. SSH and serial output are evidence channels; neither replaces this
   host-side deadline.
8. After every stop, verify there is no surviving QEMU process, the VF remains
   bound to vfio-pci, the PF remains bound to i915, `sriov_numvfs` remains 1,
   the PF answers read-only health queries, and no new DMAR/i915 fault appeared
   during cooldown. If the PF does not recover, prohibit another run and perform
   only a deliberate user-visible host recovery/reboot.
9. While the domain is off, enumerate every writable qcow2 from the effective
   inactive XML and require `qemu-img check` to pass.  Do not boot a dirty or
   structurally inconsistent image; preserve a byte-identical recovery copy
   before any repair.
10. Enumerate every running libvirt domain and inspect its active XML.  The
    target VF `0000:00:02.1` must not appear in any active domain, including a
    different guest.  A vfio-pci driver binding proves only host-driver state;
    it does not prove that another QEMU process has not already opened the VF.

The kill watcher and deadline must run independently of the Codex process and
guest network. They may stop only the named libvirt domain; they must not rebind
PCI devices, change `sriov_numvfs`, unload i915 or automatically reboot the host.
Exact scripts and XML mutations require a separate review before execution.
The canonical read-only gate is `../tools/host_vf_runtime_preflight.sh`; the
workspace wrapper is `../../tools/vf-runtime-preflight.sh`. Both deliberately
refuse to authorize or start a run. The separately reviewed controller is
`../tools/host_vf_contained_run.sh`; it defaults to refusal and requires both an
exact arm token and a complete immutable manifest. The gate must run as root
immediately before any future test so its current-boot journal check is
authoritative. A successful exit is only a necessary precondition; it is not
permission to proceed without the independent watcher and monotonic deadline
described above.

## Promotion boundary

The offline context shutdown model covers all 256 underlying state bytes:
unknown values must remain wait-only, reject schedule/deregister completion,
and preserve pending tokens and context identity. Deregistration checks all
four pending-token combinations; schedule checks include high-bit and maximal
malformed runnable payloads. These are helper-policy regressions, not evidence
that firmware stopped DMA. The runtime identity-clear caller separately requires
a tombstone, zero native references, and no protocol fault. None of these tests
extends the context admission gate to native task/page-table/page-pool lifetimes
or proves a deferred PPGTT retirement transaction safe.

One clean boot is not a safety or acceleration baseline. Looking Glass guest
capture/shared-framebuffer integration, virtual-display removal, unattended
guest login and VM autostart stay disabled until repeated contained runs prove
real Metal command completion and media workloads, clean shutdown/quiescence,
and zero new PF DMAR/i915 faults. Only then may display and service integration
be evaluated as a separate phase.

V283 now gives task publication/final unlink, all-task synchronization, live
commit/update/release/unmap, descriptor retirement, PagePool reuse/prune/free
and outer manager teardown one recursive transaction. Replaced shared
descriptors remain retained through an acknowledged Engines invalidation. This
closes the prior PagePool/task-list/common-serialization blocker only. It does
not prove complete command-submission admission, render/depth/CCS failure
propagation, callback/IRQ teardown or every retained native PF-owned MMIO/DMA
path. The hard hold therefore remains unchanged; no dynamic VF test is admitted
by this checkpoint.
