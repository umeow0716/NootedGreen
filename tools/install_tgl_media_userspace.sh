#!/bin/zsh
# Install the two exact userspace paths used by Tahoe's Intel media loader.
# This does not modify either Apple-signed executable or any kernel payload.

set -euo pipefail

[[ $EUID -eq 0 ]] || {
	print -u2 "TGL media userspace installer must run as root"
	exit 77
}

source_va=/Library/Extensions/AppleIntelTGLGraphicsVADriver.bundle
source_vame=/Library/Extensions/AppleIntelTGLGraphicsVAME.bundle
gpu_root=/Library/GPUBundles
target_va=$gpu_root/AppleIntelTGLGraphicsVADriver.bundle
target_vame=$gpu_root/AppleIntelICLGraphicsVAME.bundle
stage_va=$gpu_root/.AppleIntelTGLGraphicsVADriver.bundle.installing
stage_vame=$gpu_root/.AppleIntelICLGraphicsVAME.bundle.installing
backup_va=/Users/Shared/AppleIntelTGLGraphicsVADriver.bundle.pre-ngreen
backup_vame=/Users/Shared/AppleIntelICLGraphicsVAME.bundle.pre-ngreen

expected_va_info=3860edc1972450cd364b21c706f3e22bbc9314dc9f03eff51f4bcdccb3abd936
expected_va_exec=d265f2038135c2c5c3649b621d9aea48cf1cb74a1fb36069bb1b3562b3750129
expected_code_resources=54c07b4c0c0587dc53fa4b37dec98526993eb4eae392938164f42157b4e90378
expected_version=87a7d54c5eaf1782619d3ea7f31887ddbfb3663edb11132c956f4431c8904a3a
expected_vame_info=40b7f16a953fb565ab6cf2e07b00bb0a44ca3a9a297c3d6b5f483f0659c7a8cf
expected_vame_exec=677d73e17af59813f3254da7a98b81392c61e7f0811e3eb21ea93adf443f3230

fail() {
	print -u2 "FAIL: $1"
	exit 1
}

file_hash() {
	/usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

verify_file() {
	local path=$1 expected=$2
	[[ -f $path && ! -L $path ]] || fail "missing or linked file: $path"
	[[ $(file_hash "$path") == $expected ]] || fail "hash mismatch: $path"
}

verify_va_tree() {
	local root=$1 count
	[[ -d $root && ! -L $root ]] || fail "invalid VA bundle root: $root"
	count=$(/usr/bin/find "$root" \( -type f -o -type l \) | /usr/bin/wc -l | /usr/bin/tr -d ' ')
	[[ $count == 4 ]] || fail "unexpected VA bundle file count: $count"
	verify_file "$root/Contents/Info.plist" "$expected_va_info"
	verify_file "$root/Contents/MacOS/AppleIntelTGLGraphicsVADriver" "$expected_va_exec"
	verify_file "$root/Contents/_CodeSignature/CodeResources" "$expected_code_resources"
	verify_file "$root/Contents/version.plist" "$expected_version"
}

verify_vame_source() {
	local root=$1 count
	[[ -d $root && ! -L $root ]] || fail "invalid VAME bundle root: $root"
	count=$(/usr/bin/find "$root" \( -type f -o -type l \) | /usr/bin/wc -l | /usr/bin/tr -d ' ')
	[[ $count == 4 ]] || fail "unexpected VAME source file count: $count"
	verify_file "$root/Contents/Info.plist" "$expected_vame_info"
	verify_file "$root/Contents/MacOS/AppleIntelTGLGraphicsVAME" "$expected_vame_exec"
	verify_file "$root/Contents/_CodeSignature/CodeResources" "$expected_code_resources"
	verify_file "$root/Contents/version.plist" "$expected_version"
}

verify_vame_target() {
	local root=$1 count
	[[ -d $root && ! -L $root ]] || fail "invalid VAME target root: $root"
	count=$(/usr/bin/find "$root" \( -type f -o -type l \) | /usr/bin/wc -l | /usr/bin/tr -d ' ')
	[[ $count == 5 ]] || fail "unexpected VAME target file count: $count"
	verify_file "$root/Contents/Info.plist" "$expected_vame_info"
	verify_file "$root/Contents/MacOS/AppleIntelTGLGraphicsVAME" "$expected_vame_exec"
	verify_file "$root/Contents/MacOS/AppleIntelICLGraphicsVAME" "$expected_vame_exec"
	verify_file "$root/Contents/_CodeSignature/CodeResources" "$expected_code_resources"
	verify_file "$root/Contents/version.plist" "$expected_version"
	/usr/bin/cmp -s \
		"$root/Contents/MacOS/AppleIntelTGLGraphicsVAME" \
		"$root/Contents/MacOS/AppleIntelICLGraphicsVAME" || \
		fail "ICL VAME alias differs from signed TGL executable"
}

verify_va_tree "$source_va"
verify_vame_source "$source_vame"
/usr/bin/codesign --verify --ignore-resources --verbose=4 \
	"$source_va/Contents/MacOS/AppleIntelTGLGraphicsVADriver"
/usr/bin/codesign --verify --ignore-resources --verbose=4 \
	"$source_vame/Contents/MacOS/AppleIntelTGLGraphicsVAME"

/bin/mkdir -p "$gpu_root"
[[ ! -L $gpu_root ]] || fail "GPU bundle root is a symlink"

for stale in "$stage_va" "$stage_vame"; do
	if [[ -e $stale ]]; then
		[[ -d $stale && ! -L $stale ]] || fail "unsafe stale staging path: $stale"
		/bin/rm -rf "$stale"
	fi
done

/usr/bin/ditto "$source_va" "$stage_va"
/usr/bin/ditto "$source_vame" "$stage_vame"
/usr/bin/ditto \
	"$source_vame/Contents/MacOS/AppleIntelTGLGraphicsVAME" \
	"$stage_vame/Contents/MacOS/AppleIntelICLGraphicsVAME"
/usr/sbin/chown -R root:wheel "$stage_va" "$stage_vame"
verify_va_tree "$stage_va"
verify_vame_target "$stage_vame"
/usr/bin/codesign --verify --ignore-resources --verbose=4 \
	"$stage_va/Contents/MacOS/AppleIntelTGLGraphicsVADriver"
/usr/bin/codesign --verify --ignore-resources --verbose=4 \
	"$stage_vame/Contents/MacOS/AppleIntelICLGraphicsVAME"

if [[ -e $target_va ]]; then
	[[ ! -e $backup_va ]] || fail "VA backup path already exists"
	/bin/mv "$target_va" "$backup_va"
fi
if [[ -e $target_vame ]]; then
	[[ ! -e $backup_vame ]] || fail "VAME backup path already exists"
	/bin/mv "$target_vame" "$backup_vame"
fi

/bin/mv "$stage_va" "$target_va"
/bin/mv "$stage_vame" "$target_vame"
verify_va_tree "$target_va"
verify_vame_target "$target_vame"
/bin/sync

print "NGRN_TGL_MEDIA_USERSPACE_READY va=$expected_va_exec vame=$expected_vame_exec"
