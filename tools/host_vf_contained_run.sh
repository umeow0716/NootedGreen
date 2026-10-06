#!/usr/bin/env bash

# Fail-closed one-shot controller for a future, explicitly authorized VF run.
# The public mode refuses unless given the exact arm token. It runs the read-only
# preflight, then delegates to a transient systemd service. The service starts
# the kernel watcher and an independent deadline before starting the VM once.
# It never rebinds PCI devices, writes sysfs, unloads i915, or reboots the host.

set -euo pipefail

# Keep every exact command-output comparison stable when the controller moves
# from the invoking user's environment into a root systemd service.
export LC_ALL=C
export LANG=C
export LANGUAGE=C

readonly domain_name="macos-tahoe-sriov"
readonly libvirt_uri="qemu:///system"
readonly pf_bdf="0000:00:02.0"
readonly vf_bdf="0000:00:02.1"
readonly evidence_root="/var/log/macos-vf-tests"
readonly tracefs_root="/sys/kernel/tracing"
readonly trace_runtime_seconds=45
readonly tgl_start_runtime_seconds=90
readonly cooldown_seconds=20
readonly service_runtime_seconds=180
readonly trigger_pattern='(DMAR|IOMMU).*(00:02\.0|0000:00:02\.0).*(fault|Fault)|(fault|Fault).*(DMAR|IOMMU).*(00:02\.0|0000:00:02\.0)|i915.*(Atomic update failure on pipe|GPU HANG|reset[^[:cntrl:]]*(timed out|timeout)|fence[^[:cntrl:]]*(timed out|timeout)|GuC[^[:cntrl:]]*(timed out|timeout)|VF[^[:cntrl:]]*pause[^[:cntrl:]]*(timed out|timeout))'
readonly script_path="$(readlink -f "$0")"
readonly script_dir="$(dirname "$script_path")"
readonly preflight_path="${script_dir}/host_vf_runtime_preflight.sh"
readonly manifest_verifier="${script_dir}/host_vf_runtime_manifest.py"
readonly trace_analyzer="${script_dir}/host_vf_trace_analyzer.py"

usage() {
	printf 'This command is dangerous and is not authorized by a passing preflight alone.\n' >&2
	printf 'Usage (only after separate review):\n' >&2
	printf '  sudo %s --arm-exactly-one-contained-run --manifest /absolute/manifest.tsv\n' "$0" >&2
	printf '  sudo %s --arm-exactly-one-contained-tgl-start --manifest /absolute/manifest.tsv\n' "$0" >&2
}

runtime_seconds_for_mode() {
	case $1 in
		trace-only) printf '%s\n' "$trace_runtime_seconds" ;;
		tgl-start-only) printf '%s\n' "$tgl_start_runtime_seconds" ;;
		*) return 1 ;;
	esac
}

require_root() {
	if ((EUID != 0)); then
		printf 'FAIL: root is required for kernel journal containment.\n' >&2
		exit 1
	fi
}

validate_evidence_dir() {
	local path=$1
	[[ $path =~ ^/var/log/macos-vf-tests/[0-9]{8}T[0-9]{6}Z-[0-9a-f-]+$ ]]
}

domain_state() {
	virsh -c "$libvirt_uri" domstate "$domain_name" 2>/dev/null || true
}

driver_name() {
	local bdf=$1
	local path
	path=$(readlink -f "/sys/bus/pci/devices/${bdf}/driver" 2>/dev/null || true)
	basename "$path"
}

log_line() {
	local evidence_dir=$1
	shift
	printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "${evidence_dir}/control.log"
}

bounded_destroy() {
	local evidence_dir=$1
	local reason=$2
	(
		flock 9
		log_line "$evidence_dir" "CONTAIN reason=${reason}: requesting bounded destroy of ${domain_name}"
		timeout -k 2s 10s virsh -c "$libvirt_uri" destroy "$domain_name" \
			>>"${evidence_dir}/virsh-destroy.log" 2>&1 || true
		for ((attempt = 0; attempt < 40; attempt++)); do
			if [[ $(domain_state) == "shut off" ]]; then
				log_line "$evidence_dir" "PASS domain is shut off after ${reason}"
				return 0
			fi
			sleep 0.25
		done
		log_line "$evidence_dir" "FAIL domain did not reach shut off after ${reason}"
		return 1
	) 9>"${evidence_dir}/destroy.lock"
}

deadline_mode() {
	local evidence_dir=${1:-}
	require_root
	if ! validate_evidence_dir "$evidence_dir" || [[ ! -d $evidence_dir ]]; then
		printf 'FAIL: invalid deadline evidence directory.\n' >&2
		exit 64
	fi
	printf '%s\n' "deadline-fired" > "${evidence_dir}/deadline.trigger"
	bounded_destroy "$evidence_dir" "monotonic-deadline"
}

start_kernel_watch() {
	local evidence_dir=$1
	local fifo="${evidence_dir}/kernel.fifo"
	mkfifo -m 0600 "$fifo"
	journalctl -k -f -n0 --no-pager -o short-monotonic \
		>"$fifo" 2>"${evidence_dir}/journalctl.stderr" &
	journal_pid=$!
	(
		while IFS= read -r line; do
			printf '%s\n' "$line" >> "${evidence_dir}/kernel-follow.log"
			if grep -Eiq "$trigger_pattern" <<< "$line"; then
				printf '%s\n' "$line" > "${evidence_dir}/kernel.trigger"
				bounded_destroy "$evidence_dir" "kernel-trigger"
				exit 0
			fi
		done < "$fifo"
	) &
	watcher_pid=$!
	sleep 0.25
	if ! kill -0 "$journal_pid" 2>/dev/null || ! kill -0 "$watcher_pid" 2>/dev/null; then
		log_line "$evidence_dir" "FAIL kernel watcher did not remain alive"
		return 1
	fi
	log_line "$evidence_dir" "PASS independent current-boot kernel watcher is live"
}

schedule_deadline() {
	local evidence_dir=$1
	local deadline_unit=$2
	local runtime_seconds=$3
	systemd-run --unit="$deadline_unit" --on-active="${runtime_seconds}s" \
		--timer-property=AccuracySec=1s --collect --no-block \
		"$script_path" --internal-deadline "$evidence_dir" \
		>>"${evidence_dir}/deadline-unit.log" 2>&1
	systemctl is-active --quiet "${deadline_unit}.timer"
	log_line "$evidence_dir" "PASS independent ${runtime_seconds}s deadline is active"
}

stop_deadline() {
	local deadline_unit=$1
	systemctl stop "${deadline_unit}.timer" >/dev/null 2>&1 || true
}

stop_trace_capture() {
	local evidence_dir=$1
	local failed=0
	local end_marker_seen=0
	local instance_removed=0

	if [[ -z ${trace_instance:-} ]]; then
		return 0
	fi
	if [[ ! $trace_instance =~ ^/sys/kernel/tracing/instances/ng_vf_[0-9]{8}T[0-9]{6}Z$ ]]; then
		log_line "$evidence_dir" "FAIL refusing to clean unexpected trace instance ${trace_instance}"
		return 1
	fi

	if [[ -d $trace_instance ]]; then
		if [[ -n ${trace_pid:-} ]]; then
			if kill -0 "$trace_pid" 2>/dev/null; then
				printf '%s\n' "NG_VF_TRACE_END" > "${trace_instance}/trace_marker" || failed=1
				for ((attempt = 0; attempt < 20; attempt++)); do
					if grep -Fq "NG_VF_TRACE_END" "${evidence_dir}/host-ftrace.log" 2>/dev/null; then
						end_marker_seen=1
						break
					fi
					sleep 0.05
				done
				((end_marker_seen == 1)) || failed=1
			else
				failed=1
			fi
		fi
		printf '%s\n' 0 > "${trace_instance}/tracing_on" || failed=1
		if [[ -n ${trace_pid:-} ]]; then
			kill "$trace_pid" 2>/dev/null || true
			wait "$trace_pid" 2>/dev/null || true
		fi
		printf '%s\n' nop > "${trace_instance}/current_tracer" || failed=1
		if rmdir -- "$trace_instance"; then
			instance_removed=1
		else
			failed=1
		fi
	else
		instance_removed=1
	fi

	trace_pid=""
	if ((instance_removed == 1)); then
		trace_instance=""
	fi
	if ((failed != 0)); then
		log_line "$evidence_dir" "FAIL host ftrace capture cleanup was incomplete"
		return 1
	fi
	log_line "$evidence_dir" "PASS host ftrace capture stopped and private instance removed"
}

start_trace_capture() {
	local evidence_dir=$1
	local guard_unit=$2
	local trace_name="ng_vf_${guard_unit#macos-vf-guard-}"
	local event expected

	trace_instance="${tracefs_root}/instances/${trace_name}"
	[[ $trace_instance =~ ^/sys/kernel/tracing/instances/ng_vf_[0-9]{8}T[0-9]{6}Z$ ]]
	[[ -d ${tracefs_root}/instances && ! -e $trace_instance ]]
	mkdir "$trace_instance"

	printf '%s\n' 0 > "${trace_instance}/tracing_on"
	printf '%s\n' mono > "${trace_instance}/trace_clock"
	printf '%s\n' 1 > "${trace_instance}/options/funcgraph-abstime"
	printf '%s\n' 1 > "${trace_instance}/options/funcgraph-duration"
	printf '%s\n' 1 > "${trace_instance}/options/funcgraph-proc"
	printf '%s\n' \
		pf_state_worker_func \
		i915_ggtt_set_space_owner \
		intel_pipe_update_start \
		intel_pipe_update_end \
		> "${trace_instance}/set_ftrace_filter"
	for expected in pf_state_worker_func i915_ggtt_set_space_owner \
		intel_pipe_update_start intel_pipe_update_end; do
		grep -Eq "^${expected}( \\[i915\\])?$" "${trace_instance}/set_ftrace_filter"
	done
	printf '%s\n' function_graph > "${trace_instance}/current_tracer"
	for event in intel_pipe_update_start intel_pipe_update_vblank_evaded \
		intel_pipe_update_end; do
		[[ -f ${trace_instance}/events/i915/${event}/enable ]]
		printf '%s\n' 1 > "${trace_instance}/events/i915/${event}/enable"
	done
	: > "${trace_instance}/trace"
	{
		printf 'trace_clock='; cat "${trace_instance}/trace_clock"
		printf 'current_tracer='; cat "${trace_instance}/current_tracer"
		printf '%s\n' 'set_ftrace_filter:'
		cat "${trace_instance}/set_ftrace_filter"
		printf '%s\n' 'enabled_events:'
		cat "${trace_instance}/set_event"
	} > "${evidence_dir}/host-ftrace-config.txt"

	cat "${trace_instance}/trace_pipe" > "${evidence_dir}/host-ftrace.log" &
	trace_pid=$!
	sleep 0.25
	kill -0 "$trace_pid"
	printf '%s\n' 1 > "${trace_instance}/tracing_on"
	printf '%s\n' "NG_VF_TRACE_BEGIN" > "${trace_instance}/trace_marker"
	for ((attempt = 0; attempt < 20; attempt++)); do
		if grep -Fq "NG_VF_TRACE_BEGIN" "${evidence_dir}/host-ftrace.log" 2>/dev/null; then
			log_line "$evidence_dir" "PASS private host ftrace capture is live"
			return 0
		fi
		sleep 0.05
	done
	log_line "$evidence_dir" "FAIL host ftrace collector did not record its start marker"
	return 1
}

postflight() {
	local evidence_dir=$1
	local failures=0
	if [[ $(domain_state) != "shut off" ]]; then
		log_line "$evidence_dir" "FAIL postflight domain is not shut off"
		failures=$((failures + 1))
	fi
	if pgrep -af '[q]emu-system' | grep -Eq '(guest=macos-tahoe-sriov|name=macos-tahoe-sriov)'; then
		log_line "$evidence_dir" "FAIL postflight found surviving QEMU"
		failures=$((failures + 1))
	fi
	if [[ $(driver_name "$pf_bdf") != i915 || $(driver_name "$vf_bdf") != vfio-pci ]]; then
		log_line "$evidence_dir" "FAIL postflight PF/VF binding changed"
		failures=$((failures + 1))
	fi
	local sriov_numvfs=""
	IFS= read -r sriov_numvfs < "/sys/bus/pci/devices/${pf_bdf}/sriov_numvfs" || true
	if [[ $sriov_numvfs != 1 ]]; then
		log_line "$evidence_dir" "FAIL postflight sriov_numvfs=${sriov_numvfs:-unreadable}"
		failures=$((failures + 1))
	fi
	lspci -Dnnk -s "$pf_bdf" > "${evidence_dir}/pf-postflight.txt" 2>&1 || failures=$((failures + 1))
	if [[ -e ${evidence_dir}/kernel.trigger ]]; then
		log_line "$evidence_dir" "FAIL a host containment trigger occurred"
		failures=$((failures + 1))
	fi
	if ((failures != 0)); then
		return 1
	fi
	log_line "$evidence_dir" "PASS postflight topology and PF query remained readable"
}

internal_run_mode() {
	local evidence_dir=${1:-}
	local guard_unit=${2:-}
	local run_mode=${3:-}
	local runtime_seconds
	local manifest_path="${evidence_dir}/runtime-manifest.tsv"
	local deadline_unit="${guard_unit}-deadline"
	local run_failed=0
	local cleanup_trap=""
	journal_pid=""
	watcher_pid=""
	trace_instance=""
	trace_pid=""
	require_root
	if ! validate_evidence_dir "$evidence_dir" || [[ ! -d $evidence_dir ]] ||
	   [[ ! $guard_unit =~ ^macos-vf-guard-[0-9]{8}T[0-9]{6}Z$ ]] ||
	   ! runtime_seconds=$(runtime_seconds_for_mode "$run_mode"); then
		printf 'FAIL: invalid internal-run arguments.\n' >&2
		exit 64
	fi

	cleanup() {
		local status=$?
		local cleanup_evidence_dir=$1
		local cleanup_deadline_unit=$2
		trap - EXIT INT TERM
		if [[ $(domain_state) != "shut off" ]]; then
			bounded_destroy "$cleanup_evidence_dir" "controller-exit" || status=1
		fi
		stop_deadline "$cleanup_deadline_unit"
		if [[ -n $watcher_pid ]]; then kill "$watcher_pid" 2>/dev/null || true; fi
		if [[ -n $journal_pid ]]; then kill "$journal_pid" 2>/dev/null || true; fi
		stop_trace_capture "$cleanup_evidence_dir" || status=1
		exit "$status"
	}
	printf -v cleanup_trap 'cleanup %q %q' "$evidence_dir" "$deadline_unit"
	trap "$cleanup_trap" EXIT INT TERM

	log_line "$evidence_dir" "BEGIN one-shot contained VF run"
	"$preflight_path" >> "${evidence_dir}/preflight.log" 2>&1
	virsh -c "$libvirt_uri" dumpxml --inactive "$domain_name" \
		> "${evidence_dir}/domain-recheck.xml"
	python3 -B "$manifest_verifier" "$manifest_path" "${script_dir}/.." \
		"${evidence_dir}/domain-recheck.xml" >> "${evidence_dir}/manifest-recheck.log" 2>&1
	start_kernel_watch "$evidence_dir"
	start_trace_capture "$evidence_dir" "$guard_unit"
	schedule_deadline "$evidence_dir" "$deadline_unit" "$runtime_seconds"

	# This is the only VM start in the controller. The kernel watcher, private
	# trace collector and independent deadline must all be live first.
	if ! virsh -c "$libvirt_uri" start "$domain_name" \
		>>"${evidence_dir}/virsh-start.log" 2>&1; then
		log_line "$evidence_dir" "FAIL one-shot domain start returned failure"
		bounded_destroy "$evidence_dir" "start-failure" || true
		exit 1
	fi
	log_line "$evidence_dir" "STARTED ${domain_name} exactly once"

	while true; do
		state=$(domain_state)
		if [[ $state == "shut off" ]]; then
			break
		fi
		if [[ -z $state ]]; then
			log_line "$evidence_dir" "FAIL libvirt state became unavailable"
			bounded_destroy "$evidence_dir" "state-unavailable" || true
			run_failed=1
			break
		fi
		if ! kill -0 "$watcher_pid" 2>/dev/null || ! kill -0 "$journal_pid" 2>/dev/null ||
		   ! kill -0 "$trace_pid" 2>/dev/null; then
			log_line "$evidence_dir" "FAIL kernel watcher or ftrace collector exited while VM was live"
			bounded_destroy "$evidence_dir" "observer-exit" || true
			run_failed=1
			break
		fi
		sleep 0.25
	done

	stop_deadline "$deadline_unit"
	log_line "$evidence_dir" "COOLDOWN ${cooldown_seconds}s with kernel watcher still active"
	for ((second = 0; second < cooldown_seconds; second++)); do
		sleep 1
	done
	if ! stop_trace_capture "$evidence_dir"; then
		run_failed=1
	fi
	if python3 -B "$trace_analyzer" --require-vf-flr \
		"${evidence_dir}/host-ftrace.log" \
		> "${evidence_dir}/host-ftrace-analysis.txt" 2>&1; then
		log_line "$evidence_dir" "PASS host ftrace analysis completed without a crossed vblank"
	else
		log_line "$evidence_dir" "FAIL host ftrace analysis rejected the run"
		run_failed=1
	fi
	if ! postflight "$evidence_dir"; then
		run_failed=1
	fi
	if [[ -e ${evidence_dir}/deadline.trigger ]]; then
		log_line "$evidence_dir" "NOTE run ended by the fixed deadline"
	fi
	log_line "$evidence_dir" "END contained VF run status=${run_failed}"
	return "$run_failed"
}

arm_mode() {
	local manifest_path=$1
	local run_mode=$2
	local runtime_seconds
	require_root
	runtime_seconds=$(runtime_seconds_for_mode "$run_mode") || {
		printf 'FAIL: invalid contained-run mode.\n' >&2
		exit 64
	}
	for command_name in systemd-run systemctl systemd-inhibit virsh journalctl \
		timeout flock lspci python3 readlink dirname tee date grep mkfifo sleep \
		sha256sum git install mktemp rm pgrep basename cat mkdir rmdir kill; do
		command -v "$command_name" >/dev/null 2>&1 || {
			printf 'FAIL: missing command %s\n' "$command_name" >&2
			exit 1
		}
	done
	[[ -x $preflight_path ]] || {
		printf 'FAIL: preflight is not executable: %s\n' "$preflight_path" >&2
		exit 1
	}
	[[ -x $manifest_verifier ]] || {
		printf 'FAIL: manifest verifier is not executable: %s\n' "$manifest_verifier" >&2
		exit 1
	}
	[[ -x $trace_analyzer ]] || {
		printf 'FAIL: trace analyzer is not executable: %s\n' "$trace_analyzer" >&2
		exit 1
	}
	[[ $manifest_path = /* && -f $manifest_path && ! -L $manifest_path ]] || {
		printf 'FAIL: manifest must be an absolute, regular, non-symlink file.\n' >&2
		exit 1
	}
	"$preflight_path"

	local stamp boot_id evidence_dir guard_unit domain_xml_snapshot
	stamp=$(date -u +%Y%m%dT%H%M%SZ)
	IFS= read -r boot_id < /proc/sys/kernel/random/boot_id
	evidence_dir="${evidence_root}/${stamp}-${boot_id}"
	guard_unit="macos-vf-guard-${stamp}"
	domain_xml_snapshot=$(mktemp /tmp/macos-vf-domain.XXXXXX.xml)
	trap 'rm -f -- "${domain_xml_snapshot:-}"' EXIT
	virsh -c "$libvirt_uri" dumpxml --inactive "$domain_name" > "$domain_xml_snapshot"
	python3 -B "$manifest_verifier" "$manifest_path" "${script_dir}/.." \
		"$domain_xml_snapshot"
	install -d -m 0700 "$evidence_dir"
	install -m 0600 "$manifest_path" "${evidence_dir}/runtime-manifest.tsv"
	install -m 0600 "$domain_xml_snapshot" "${evidence_dir}/domain-before.xml"
	rm -f -- "$domain_xml_snapshot"
	trap - EXIT
	{
		printf 'domain=%s\n' "$domain_name"
		printf 'boot_id=%s\n' "$boot_id"
		printf 'run_mode=%s\n' "$run_mode"
		printf 'max_runtime_seconds=%s\n' "$runtime_seconds"
		printf 'cooldown_seconds=%s\n' "$cooldown_seconds"
		printf 'source_commit=%s\n' "$(git -C "${script_dir}/.." rev-parse HEAD)"
		sha256sum "$script_path" "$preflight_path" "$trace_analyzer"
	} > "${evidence_dir}/manifest.txt"

	systemd-run --unit="$guard_unit" --description="Contained macOS Intel VF validation" \
		--collect --no-block --property=Type=exec --property=KillMode=control-group \
		--property="RuntimeMaxSec=${service_runtime_seconds}s" --property=TimeoutStopSec=15s \
		systemd-inhibit --what=sleep --mode=block --who=NootedGreen-VF \
			--why="Contained Intel VF validation" \
			"$script_path" --internal-run "$evidence_dir" "$guard_unit" "$run_mode"
	printf 'ARMED: %s (evidence %s)\n' "$guard_unit" "$evidence_dir"
}

case ${1:-} in
	--arm-exactly-one-contained-run)
		(($# == 3)) && [[ ${2:-} == --manifest ]] || { usage; exit 64; }
		arm_mode "$3" trace-only
		;;
	--arm-exactly-one-contained-tgl-start)
		(($# == 3)) && [[ ${2:-} == --manifest ]] || { usage; exit 64; }
		arm_mode "$3" tgl-start-only
		;;
	--internal-run)
		(($# == 4)) || { usage; exit 64; }
		internal_run_mode "$2" "$3" "$4"
		;;
	--internal-deadline)
		(($# == 2)) || { usage; exit 64; }
		deadline_mode "$2"
		;;
	*)
		usage
		exit 64
		;;
esac
