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


def trigger_pattern(text: str) -> str:
    match = re.search(r"^readonly trigger_pattern='([^']+)'$", text, re.MULTILINE)
    if not match:
        raise AssertionError("missing single-quoted containment trigger pattern")
    return match.group(1)


def grep_matches(pattern: str, line: str) -> bool:
    result = subprocess.run(
        ["grep", "-Eiq", pattern], input=line, text=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    if result.returncode not in (0, 1):
        raise AssertionError(f"invalid containment trigger ERE: {result.stderr}")
    return result.returncode == 0


def main() -> None:
    preflight = PREFLIGHT.read_text()
    runner = RUNNER.read_text()
    preflight_body = shell_body(preflight)
    runner_body = shell_body(runner)
    preflight_trigger = trigger_pattern(preflight)
    runner_trigger = trigger_pattern(runner)

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
    require(preflight, "export LC_ALL=C", "locale-independent preflight")
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
    require(preflight, 'virsh -c "$libvirt_uri" list --state-running --name',
            "global active-domain inventory")
    require(preflight, 'active_vf_owners+=("$running_domain")',
            "active VF owner recording")
    require(preflight, "no active libvirt domain owns target VF",
            "exclusive active VF ownership gate")
    require(preflight, "journalctl -k -b", "current-boot journal gate")
    require(preflight, '((EUID == 0))', "root journal gate")
    require(preflight, 'grep -Ei "$trigger_pattern" >/dev/null',
            "pipefail-safe whole-journal trigger scan")
    require(preflight, 'readonly tracefs_root="/sys/kernel/tracing"',
            "exact tracefs root")
    require(preflight, '"${tracefs_root}/available_filter_functions"',
            "read-only ftrace function inventory")
    for trace_name in (
        "pf_state_worker_func",
        "i915_ggtt_set_space_owner",
        "intel_pipe_update_start",
        "intel_pipe_update_end",
        "intel_pipe_update_vblank_evaded",
    ):
        require(preflight, trace_name, f"preflight trace capability {trace_name}")
    if re.search(r"journalctl[^\n]*\|\s*grep\s+[^\n]*q", preflight_body):
        raise AssertionError("journal preflight uses early-exit grep under pipefail")

    if preflight_trigger != runner_trigger:
        raise AssertionError("preflight and runtime watcher trigger patterns differ")
    trigger_examples = (
        "i915 0000:00:02.0: [drm] *ERROR* Atomic update failure on pipe A",
        "i915 0000:00:02.0: [drm] *ERROR* GPU HANG: ecode 12:1",
        "i915 0000:00:02.0: GuC reset timed out",
        "DMAR: [DMA Read NO_PASID] device [00:02.0] fault addr 0x1000",
    )
    for line in trigger_examples:
        if not grep_matches(runner_trigger, line):
            raise AssertionError(f"containment pattern misses: {line}")
    non_trigger_examples = (
        "i915 0000:00:02.0: VF1 FLR",
        "amdgpu 0000:03:00.0: Atomic update failure on pipe A",
        "DMAR: device [00:14.0] fault addr 0x1000",
    )
    for line in non_trigger_examples:
        if grep_matches(runner_trigger, line):
            raise AssertionError(f"containment pattern is overbroad: {line}")

    if runner.count('virsh -c "$libvirt_uri" start "$domain_name"') != 1:
        raise AssertionError("runner must contain exactly one exact-domain start")
    if runner.count('virsh -c "$libvirt_uri" destroy "$domain_name"') != 1:
        raise AssertionError("all destroys must share one bounded exact-domain helper")

    internal = runner[require(runner, "internal_run_mode()", "internal controller"):]
    watch = require(internal, 'start_kernel_watch "$evidence_dir"', "watcher start")
    trace = require(internal, 'start_trace_capture "$evidence_dir" "$guard_unit"',
                    "ftrace capture start")
    deadline = require(internal, 'schedule_deadline "$evidence_dir"', "deadline start")
    start = require(internal, 'virsh -c "$libvirt_uri" start "$domain_name"', "VM start")
    if not watch < trace < deadline < start:
        raise AssertionError(
            "watcher, private ftrace capture and independent deadline must precede VM start"
        )

    arm = runner[require(runner, "arm_mode()", "public arm mode"):]
    preflight_call = require(arm, '"$preflight_path"', "public preflight")
    service = require(arm, "systemd-run --unit=", "transient guard service")
    if not preflight_call < service:
        raise AssertionError("public preflight must precede transient service creation")

    require(runner, 'readonly trace_runtime_seconds=45',
            "fixed trace-only deadline")
    require(runner, 'readonly tgl_start_runtime_seconds=90',
            "fixed TGL start-only deadline")
    require(runner, 'readonly metal_smoke_runtime_seconds=150',
            "fixed Metal-smoke deadline")
    require(runner, 'readonly media_smoke_runtime_seconds=150',
            "fixed media-smoke deadline")
    require(runner, 'readonly service_runtime_seconds=240',
            "bounded service runtime")
    require(runner, 'runtime_seconds_for_mode()', "closed runtime-mode selector")
    require(runner, 'trace-only) printf', "trace-only mode allowlist")
    require(runner, 'tgl-start-only) printf', "TGL start-only mode allowlist")
    require(runner, 'metal-smoke) printf', "Metal-smoke mode allowlist")
    require(runner, 'media-smoke) printf', "media-smoke mode allowlist")
    require(runner,
            'schedule_deadline "$evidence_dir" "$deadline_unit" "$runtime_seconds"',
            "mode-selected deadline")
    require(runner, "export LC_ALL=C", "locale-independent controller")
    require(runner, 'readonly cooldown_seconds=20', "fixed cooldown")
    require(runner, "journalctl -k -f -n0", "new-message kernel watcher")
    require(runner, 'readonly tracefs_root="/sys/kernel/tracing"',
            "private tracefs root")
    require(runner, 'printf \'%s\\n\' mono > "${trace_instance}/trace_clock"',
            "monotonic trace clock")
    trace_capture = runner[
        require(runner, "start_trace_capture()", "trace capture function"):
        require(runner, "postflight()", "postflight function")
    ]
    trace_create = require(trace_capture, 'mkdir "$trace_instance"',
                           "private trace instance creation")
    trace_initial_stop = require(
        trace_capture,
        'printf \'%s\\n\' 0 > "${trace_instance}/tracing_on"',
        "trace disabled before configuration",
    )
    trace_start = require(
        trace_capture,
        'printf \'%s\\n\' 1 > "${trace_instance}/tracing_on"',
        "explicit trace start",
    )
    if not trace_create < trace_initial_stop < trace_start:
        raise AssertionError("private trace must be stopped while it is configured")
    require(runner, 'printf \'%s\\n\' function_graph > "${trace_instance}/current_tracer"',
            "function graph tracer")
    require(runner, 'printf \'%s\\n\' 1 > "${trace_instance}/options/funcgraph-proc"',
            "task-aware function graph")
    require(runner, 'cat "${trace_instance}/trace_pipe" > "${evidence_dir}/host-ftrace.log" &',
            "streaming trace collector")
    require(runner, 'grep -Fq "NG_VF_TRACE_BEGIN"',
            "trace collector liveness marker")
    require(runner, 'grep -Fq "NG_VF_TRACE_END"',
            "trace collector completion marker")
    require(runner, '((end_marker_seen == 1)) || failed=1',
            "trace completion marker fail-closed gate")
    require(runner, '! kill -0 "$trace_pid"',
            "live trace collector health gate")
    require(runner, 'rmdir -- "$trace_instance"',
            "exact private trace instance cleanup")
    if "rm -rf" in runner:
        raise AssertionError("trace capture must not recursively remove tracefs state")
    for trace_name in (
        "pf_state_worker_func",
        "i915_ggtt_set_space_owner",
        "intel_pipe_update_start",
        "intel_pipe_update_end",
        "intel_pipe_update_vblank_evaded",
    ):
        require(runner, trace_name, f"runtime trace coverage {trace_name}")
    require(runner, 'grep -Eiq "$trigger_pattern" <<< "$line"',
            "pipefail-safe per-line trigger scan")
    require(runner, "systemd-inhibit --what=sleep", "sleep inhibitor")
    require(runner, "--arm-exactly-one-contained-run", "explicit arm token")
    require(runner, "--arm-exactly-one-contained-tgl-start",
            "explicit TGL start-only arm token")
    require(runner, "--arm-exactly-one-contained-metal-smoke",
            "explicit Metal-smoke arm token")
    require(runner, "--arm-exactly-one-contained-media-smoke",
            "explicit media-smoke arm token")
    require(runner,
            '"$script_path" --internal-run "$evidence_dir" "$guard_unit" "$run_mode"',
            "immutable internal mode handoff")
    require(runner, 'python3 -B "$manifest_verifier"', "immutable manifest gate")
    if runner.count('python3 -B "$manifest_verifier"') != 2:
        raise AssertionError("manifest must be verified before and inside the systemd service")
    require(runner, '[[ $(driver_name "$pf_bdf") != i915', "PF postflight")
    require(runner, '$(driver_name "$vf_bdf") != vfio-pci', "VF postflight")
    require(runner, "printf -v cleanup_trap 'cleanup %q %q'",
            "scope-independent cleanup trap arguments")
    require(runner, 'bounded_destroy "$cleanup_evidence_dir" "controller-exit"',
            "scope-independent cleanup evidence path")
    require(runner, 'stop_deadline "$cleanup_deadline_unit"',
            "scope-independent cleanup deadline unit")
    require(runner, 'stop_trace_capture "$cleanup_evidence_dir" || status=1',
            "scope-independent trace cleanup")
    trace_stop = require(internal, 'stop_trace_capture "$evidence_dir"',
                         "normal trace stop")
    trace_analyze = require(
        internal,
        'python3 -B "$trace_analyzer" --require-vf-flr',
        "post-capture trace analysis",
    )
    postflight = require(internal, 'postflight "$evidence_dir"', "postflight")
    if not trace_stop < trace_analyze < postflight:
        raise AssertionError("trace must stop and be analyzed before postflight")
    require(runner, '[[ -x $trace_analyzer ]]', "executable trace analyzer gate")
    require(runner, 'sha256sum "$script_path" "$preflight_path" "$trace_analyzer"',
            "trace analyzer evidence identity")

    print("PASS: fail-closed host VF preflight/containment source contract")


if __name__ == "__main__":
    main()
