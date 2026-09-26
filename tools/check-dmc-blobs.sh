#!/usr/bin/env bash
# Read-only Linux-host check against pinned linux-firmware containers.
# No firmware is loaded, installed or written to hardware.
set -euo pipefail
task_repo="$(git rev-parse --show-toplevel)"
task_firmware_dir="${1:-/usr/lib/firmware/i915}"

check_blob() {
    local source="$1" reference="$2" bytes="$3" digest="$4"
    test -r "$reference"
    printf '%s  %s\n' "$digest" "$reference" | sha256sum --check --status
    cmp <(perl -0777 -ne '
        if (/const uint32_t (?:tgl|adlp)_dmc_ver2_\d+_bin\[\]\s*=\s*\{(.*?)\};/s) {
            $body = $1;
            while ($body =~ /0x([0-9A-Fa-f]+)/g) { print pack("V", hex($1)); }
        } else { die "firmware array missing\n"; }
    ' "$source") <(dd if="$reference" bs=1 skip=784 count="$bytes" status=none)
    printf 'PASS main DMC payload: %s (%s bytes)\n' "$source" "$bytes"
}

check_blob "$task_repo/NootedGreen/Firmware.cpp" \
    "$task_firmware_dir/tgl_dmc_ver2_12.bin" 17840 \
    3c013ef0ad96ba73aee8e5bd04a8e27cc9b1c6e9183b1a83ce124485f325afca
check_blob "$task_repo/NootedGreen/FirmwareADLP.cpp" \
    "$task_firmware_dir/adlp_dmc_ver2_16.bin" 24780 \
    2da482ea46a40e54c9ca3b54185959177f393eff98ece21acdac7eb6cacb0fcb
