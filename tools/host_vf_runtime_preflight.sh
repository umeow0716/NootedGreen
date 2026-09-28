#!/usr/bin/env bash

# Read-only, fail-closed host gate for a future Intel SR-IOV VF runtime test.
# This script never starts/stops a domain, changes libvirt XML, rebinds PCI
# devices, writes sysfs, or installs a guest artifact.

set -euo pipefail

readonly domain_name="macos-tahoe-sriov"
readonly libvirt_uri="qemu:///system"
readonly pf_bdf="0000:00:02.0"
readonly vf_bdf="0000:00:02.1"
readonly expected_vendor="0x8086"
readonly expected_device="0xa7a8"
readonly trigger_pattern='(DMAR|IOMMU).*(00:02\.0|0000:00:02\.0).*(fault|Fault)|(fault|Fault).*(DMAR|IOMMU).*(00:02\.0|0000:00:02\.0)|i915.*(GPU HANG|reset[^[:cntrl:]]*(timed out|timeout)|fence[^[:cntrl:]]*(timed out|timeout)|GuC[^[:cntrl:]]*(timed out|timeout)|VF[^[:cntrl:]]*pause[^[:cntrl:]]*(timed out|timeout))'

failures=0

pass() {
	printf 'PASS: %s\n' "$1"
}

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	failures=$((failures + 1))
}

need_command() {
	if command -v "$1" >/dev/null 2>&1; then
		pass "command available: $1"
	else
		fail "missing required command: $1"
	fi
}

xml_value() {
	local expression=$1
	local xml=$2
	printf '%s' "$xml" | xmllint --xpath "string(${expression})" - 2>/dev/null
}

driver_name() {
	local bdf=$1
	local driver_path
	driver_path=$(readlink -f "/sys/bus/pci/devices/${bdf}/driver" 2>/dev/null || true)
	basename "$driver_path"
}

read_one_line() {
	local path=$1
	local value=""
	if [[ -r $path ]]; then
		IFS= read -r value < "$path" || true
	fi
	printf '%s' "$value"
}

if (($# != 0)); then
	printf 'Usage: %s\n' "$0" >&2
	exit 64
fi

for command_name in virsh xmllint journalctl systemd-inhibit readlink basename pgrep grep; do
	need_command "$command_name"
done

if ((EUID == 0)); then
	pass "running as root for authoritative kernel-journal access"
else
	fail "must run as root; unprivileged journal access cannot certify host health"
fi

domain_state=$(virsh -c "$libvirt_uri" domstate "$domain_name" 2>/dev/null || true)
if [[ $domain_state == "shut off" ]]; then
	pass "domain is shut off"
else
	fail "domain must be shut off (observed: ${domain_state:-unavailable})"
fi

domain_info=$(virsh -c "$libvirt_uri" dominfo "$domain_name" 2>/dev/null || true)
if [[ $domain_info == *$'Autostart:      disable'* ]]; then
	pass "domain autostart is disabled"
else
	fail "domain autostart is not proven disabled"
fi
if [[ $domain_info == *$'Managed save:   no'* ]]; then
	pass "domain has no managed-save image"
else
	fail "domain managed-save state is not proven absent"
fi

domain_xml=$(virsh -c "$libvirt_uri" dumpxml --inactive "$domain_name" 2>/dev/null || true)
if [[ -z $domain_xml ]]; then
	fail "cannot read inactive domain XML"
else
	on_poweroff=$(xml_value '/domain/on_poweroff' "$domain_xml" || true)
	on_reboot=$(xml_value '/domain/on_reboot' "$domain_xml" || true)
	on_crash=$(xml_value '/domain/on_crash' "$domain_xml" || true)
	watchdog_action=$(xml_value '/domain/devices/watchdog/@action' "$domain_xml" || true)
	hostdev_count=$(xml_value 'count(/domain/devices/hostdev[@type="pci"]/source/address[@domain="0x0000" and @bus="0x00" and @slot="0x02" and @function="0x1"])' "$domain_xml" || true)

	if [[ $on_poweroff == destroy && $on_reboot == destroy && $on_crash == destroy ]]; then
		pass "libvirt poweroff/reboot/crash policies are one-shot destroy"
	else
		fail "one-shot policies required: poweroff=destroy reboot=destroy crash=destroy (observed ${on_poweroff:-missing}/${on_reboot:-missing}/${on_crash:-missing})"
	fi
	if [[ $watchdog_action == destroy ]]; then
		pass "guest watchdog destroys instead of resetting the domain"
	else
		fail "guest watchdog action must be destroy (observed: ${watchdog_action:-missing})"
	fi
	if [[ $hostdev_count == 1 ]]; then
		pass "domain contains exactly one ${vf_bdf} PCI hostdev source"
	else
		fail "domain must contain exactly one ${vf_bdf} PCI hostdev source (observed: ${hostdev_count:-unavailable})"
	fi
fi

pf_vendor=$(read_one_line "/sys/bus/pci/devices/${pf_bdf}/vendor")
pf_device=$(read_one_line "/sys/bus/pci/devices/${pf_bdf}/device")
vf_vendor=$(read_one_line "/sys/bus/pci/devices/${vf_bdf}/vendor")
vf_device=$(read_one_line "/sys/bus/pci/devices/${vf_bdf}/device")
if [[ $pf_vendor == "$expected_vendor" && $pf_device == "$expected_device" &&
      $vf_vendor == "$expected_vendor" && $vf_device == "$expected_device" ]]; then
	pass "PF/VF PCI identities match Intel ${expected_device}"
else
	fail "unexpected PF/VF identity (${pf_vendor:-?}:${pf_device:-?}/${vf_vendor:-?}:${vf_device:-?})"
fi

pf_driver=$(driver_name "$pf_bdf")
vf_driver=$(driver_name "$vf_bdf")
if [[ $pf_driver == i915 ]]; then
	pass "PF ${pf_bdf} is bound to i915"
else
	fail "PF ${pf_bdf} must be bound to i915 (observed: ${pf_driver:-none})"
fi
if [[ $vf_driver == vfio-pci ]]; then
	pass "VF ${vf_bdf} is bound to vfio-pci"
else
	fail "VF ${vf_bdf} must be bound to vfio-pci (observed: ${vf_driver:-none})"
fi

sriov_numvfs=$(read_one_line "/sys/bus/pci/devices/${pf_bdf}/sriov_numvfs")
sriov_totalvfs=$(read_one_line "/sys/bus/pci/devices/${pf_bdf}/sriov_totalvfs")
if [[ $sriov_numvfs == 1 && $sriov_totalvfs =~ ^[1-9][0-9]*$ ]]; then
	pass "PF exposes exactly one VF (${sriov_numvfs}/${sriov_totalvfs})"
else
	fail "expected sriov_numvfs=1 and positive total (observed ${sriov_numvfs:-unreadable}/${sriov_totalvfs:-unreadable})"
fi

pf_group=$(basename "$(readlink -f "/sys/bus/pci/devices/${pf_bdf}/iommu_group" 2>/dev/null || true)")
vf_group=$(basename "$(readlink -f "/sys/bus/pci/devices/${vf_bdf}/iommu_group" 2>/dev/null || true)")
if [[ -n $pf_group && -n $vf_group && $pf_group != "$vf_group" ]]; then
	pass "PF/VF use distinct IOMMU groups (${pf_group}/${vf_group})"
else
	fail "PF/VF IOMMU isolation is not proven (observed ${pf_group:-none}/${vf_group:-none})"
fi

if pgrep -af '[q]emu-system' | grep -Eq '(guest=macos-tahoe-sriov|name=macos-tahoe-sriov)'; then
	fail "a QEMU process for ${domain_name} is still present"
else
	pass "no QEMU process for ${domain_name} is present"
fi

if ((EUID == 0)); then
	boot_id=$(read_one_line /proc/sys/kernel/random/boot_id)
	latest_kernel_line=$(journalctl -k -b -n 1 --no-pager -o cat 2>/dev/null || true)
	if [[ -n $boot_id && -n $latest_kernel_line ]]; then
		pass "kernel journal is readable for boot ${boot_id}"
	else
		fail "kernel journal returned no authoritative current-boot evidence"
	fi
	if journalctl -k -b --no-pager -o cat 2>/dev/null | grep -Eiq "$trigger_pattern"; then
		fail "current boot already contains a VF-test containment trigger"
	else
		pass "current boot contains no configured PF DMAR/i915 containment trigger"
	fi
fi

if ((failures != 0)); then
	printf 'BLOCKED: %d preflight requirement(s) failed; do not start the VM.\n' "$failures" >&2
	exit 1
fi

printf 'READY: read-only host preflight passed. This does not start or authorize a runtime test.\n'
