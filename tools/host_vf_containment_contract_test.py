#!/usr/bin/env python3
"""Offline source contract for the host VF containment scripts."""

from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parent
PREFLIGHT = ROOT / "host_vf_runtime_preflight.sh"
RUNNER = ROOT / "host_vf_contained_run.sh"


def require(text: str, needle: str, label: str) -> int:
    position = text.find(needle)
    if position < 0:
        raise AssertionError(f"missing {label}: {needle}")
    return position


def shell_body(text: str) -> str:
    return "\n".join(
        line for line in text.splitlines()
        if not line.lstrip().startswith("#")
    )


def main() -> None:
    preflight = PREFLIGHT.read_text()
    runner = RUNNER.read_text()
    preflight_body = shell_body(preflight)
    runner_body = shell_body(runner)

    for path in (PREFLIGHT, RUNNER):
        subprocess.run(["bash", "-n", str(path)], check=True)
    refusal = subprocess.run(
        [str(RUNNER)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if refusal.returncode != 64 or "--arm-exactly-one-contained-run" not in refusal.stderr:
        raise AssertionError("controller does not refuse an unarmed invocation")

    if re.search(r"\bvirsh\b[^\n]*(?:start|destroy)\b", preflight_body):
        raise AssertionError("read-only preflight contains a VM mutation")
    for forbidden in (
        r"/driver/(?:un)?bind",
        r"driver_override",
        r"(?:>|tee\s+)/sys/",
        r"\b(?:modprobe|rmmod|reboot|poweroff)\b",
    ):
        if re.search(forbidden, runner_body):
            raise AssertionError(f"runner contains forbidden host mutation: {forbidden}")

    require(preflight, 'readonly domain_name="macos-tahoe-sriov"', "exact domain")
    require(preflight, 'readonly pf_bdf="0000:00:02.0"', "exact PF")
    require(preflight, 'readonly vf_bdf="0000:00:02.1"', "exact VF")
    require(preflight, "on_poweroff == destroy", "poweroff one-shot check")
    require(preflight, "on_reboot == destroy", "reboot one-shot check")
    require(preflight, "on_crash == destroy", "crash one-shot check")
    require(preflight, "watchdog_count == 1", "single-watchdog check")
    require(preflight, "watchdog_model == itco", "Q35 iTCO model check")
    require(preflight, "watchdog_action == none", "inert watchdog action check")
    require(preflight, "host deadline is the only timeout authority",
            "host-only timeout authority")
    require(preflight, 'driver/@type="qcow2" and not(readonly)',
            "writable qcow2 inventory")
    require(preflight, 'qemu-img check --output=json "$disk_path"',
            "read-only qcow2 integrity gate")
    require(preflight, "journalctl -k -b", "current-boot journal gate")
    require(preflight, '((EUID == 0))', "root journal gate")

    if runner.count('virsh -c "$libvirt_uri" start "$domain_name"') != 1:
        raise AssertionError("runner must contain exactly one exact-domain start")
    if runner.count('virsh -c "$libvirt_uri" destroy "$domain_name"') != 1:
        raise AssertionError("all destroys must share one bounded exact-domain helper")

    internal = runner[require(runner, "internal_run_mode()", "internal controller"):]
    watch = require(internal, 'start_kernel_watch "$evidence_dir"', "watcher start")
    deadline = require(internal, 'schedule_deadline "$evidence_dir"', "deadline start")
    start = require(internal, 'virsh -c "$libvirt_uri" start "$domain_name"', "VM start")
    if not watch < deadline < start:
        raise AssertionError("watcher and independent deadline must precede VM start")

    arm = runner[require(runner, "arm_mode()", "public arm mode"):]
    preflight_call = require(arm, '"$preflight_path"', "public preflight")
    service = require(arm, "systemd-run --unit=", "transient guard service")
    if not preflight_call < service:
        raise AssertionError("public preflight must precede transient service creation")

    require(runner, 'readonly max_runtime_seconds=45', "fixed short deadline")
    require(runner, 'readonly cooldown_seconds=20', "fixed cooldown")
    require(runner, "journalctl -k -f -n0", "new-message kernel watcher")
    require(runner, "systemd-inhibit --what=sleep", "sleep inhibitor")
    require(runner, "--arm-exactly-one-contained-run", "explicit arm token")
    require(runner, 'python3 -B "$manifest_verifier"', "immutable manifest gate")
    if runner.count('python3 -B "$manifest_verifier"') != 2:
        raise AssertionError("manifest must be verified before and inside the systemd service")
    require(runner, '[[ $(driver_name "$pf_bdf") != i915', "PF postflight")
    require(runner, '$(driver_name "$vf_bdf") != vfio-pci', "VF postflight")

    print("PASS: fail-closed host VF preflight/containment source contract")


if __name__ == "__main__":
    main()
