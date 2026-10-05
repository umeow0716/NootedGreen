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
readonly max_runtime_seconds=45
readonly cooldown_seconds=20
readonly service_runtime_seconds=120
readonly trigger_pattern='(DMAR|IOMMU).*(00:02\.0|0000:00:02\.0).*(fault|Fault)|(fault|Fault).*(DMAR|IOMMU).*(00:02\.0|0000:00:02\.0)|i915.*(Atomic update failure on pipe|GPU HANG|reset[^[:cntrl:]]*(timed out|timeout)|fence[^[:cntrl:]]*(timed out|timeout)|GuC[^[:cntrl:]]*(timed out|timeout)|VF[^[:cntrl:]]*pause[^[:cntrl:]]*(timed out|timeout))'
readonly script_path="$(readlink -f "$0")"
readonly script_dir="$(dirname "$script_path")"
readonly preflight_path="${script_dir}/host_vf_runtime_preflight.sh"
readonly manifest_verifier="${script_dir}/host_vf_runtime_manifest.py"

usage() {
	printf 'This command is dangerous and is not authorized by a passing preflight alone.\n' >&2
	printf 'Usage (only after separate review): sudo %s --arm-exactly-one-contained-run --manifest /absolute/manifest.tsv\n' "$0" >&2
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
	systemd-run --unit="$deadline_unit" --on-active="${max_runtime_seconds}s" \
		--timer-property=AccuracySec=1s --collect --no-block \
		"$script_path" --internal-deadline "$evidence_dir" \
		>>"${evidence_dir}/deadline-unit.log" 2>&1
	systemctl is-active --quiet "${deadline_unit}.timer"
	log_line "$evidence_dir" "PASS independent ${max_runtime_seconds}s deadline is active"
}

stop_deadline() {
	local deadline_unit=$1
	systemctl stop "${deadline_unit}.timer" >/dev/null 2>&1 || true
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
	local manifest_path="${evidence_dir}/runtime-manifest.tsv"
	local deadline_unit="${guard_unit}-deadline"
	local run_failed=0
	local cleanup_trap=""
	journal_pid=""
	watcher_pid=""
	require_root
	if ! validate_evidence_dir "$evidence_dir" || [[ ! -d $evidence_dir ]] ||
	   [[ ! $guard_unit =~ ^macos-vf-guard-[0-9]{8}T[0-9]{6}Z$ ]]; then
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
	schedule_deadline "$evidence_dir" "$deadline_unit"

	# This is the only VM start in the controller. Both independent safety
	# channels above must already be live before this line is reachable.
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
		if ! kill -0 "$watcher_pid" 2>/dev/null || ! kill -0 "$journal_pid" 2>/dev/null; then
			log_line "$evidence_dir" "FAIL kernel watcher exited while VM was live"
			bounded_destroy "$evidence_dir" "watcher-exit" || true
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
	require_root
	for command_name in systemd-run systemctl systemd-inhibit virsh journalctl \
		timeout flock lspci python3 readlink dirname tee date grep mkfifo sleep \
		sha256sum git install mktemp rm pgrep basename; do
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
		printf 'max_runtime_seconds=%s\n' "$max_runtime_seconds"
		printf 'cooldown_seconds=%s\n' "$cooldown_seconds"
		printf 'source_commit=%s\n' "$(git -C "${script_dir}/.." rev-parse HEAD)"
		sha256sum "$script_path" "$preflight_path"
	} > "${evidence_dir}/manifest.txt"

	systemd-run --unit="$guard_unit" --description="Contained macOS Intel VF validation" \
		--collect --no-block --property=Type=exec --property=KillMode=control-group \
		--property="RuntimeMaxSec=${service_runtime_seconds}s" --property=TimeoutStopSec=15s \
		systemd-inhibit --what=sleep --mode=block --who=NootedGreen-VF \
			--why="Contained Intel VF validation" \
			"$script_path" --internal-run "$evidence_dir" "$guard_unit"
	printf 'ARMED: %s (evidence %s)\n' "$guard_unit" "$evidence_dir"
}

case ${1:-} in
	--arm-exactly-one-contained-run)
		(($# == 3)) && [[ ${2:-} == --manifest ]] || { usage; exit 64; }
		arm_mode "$3"
		;;
	--internal-run)
		(($# == 3)) || { usage; exit 64; }
		internal_run_mode "$2" "$3"
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
