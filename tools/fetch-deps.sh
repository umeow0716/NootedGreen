#!/bin/sh
# Fetch build dependencies into the repo root: Lilu (DEBUG, for headers and
# plugin library) and MacKernelSDK. Existing copies are left alone.
set -eu
cd "$(dirname "$0")/.."
LILU=1.7.2
SDK=3f750085caa17ec3a7880f11c11bf4f48cd6a164

if [ ! -d Lilu.kext ]; then
	tmp=$(mktemp -d)
	curl -Lfso "$tmp/lilu.zip" "https://github.com/acidanthera/Lilu/releases/download/$LILU/Lilu-$LILU-DEBUG.zip"
	unzip -q "$tmp/lilu.zip" -d "$tmp"
	mv "$tmp/Lilu.kext" .
	rm -rf "$tmp"
fi
if [ ! -d MacKernelSDK ]; then
	git clone -q https://github.com/acidanthera/MacKernelSDK
	git -C MacKernelSDK checkout -q "$SDK"
fi
