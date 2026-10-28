#!/bin/sh
# Replace the UMD's impostor.dylib with ngshim (run as root). NGXe admits this
# exact path past library validation; the UMD bundle signature is unaffected
# because the UMD imports nothing from it.
set -eu
cd "$(dirname "$0")"
dir=/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS
[ -e /Users/Shared/impostor.dylib.wex-orig ] || cp "$dir/impostor.dylib" /Users/Shared/impostor.dylib.wex-orig
install -m 755 -o root -g wheel build/impostor.dylib "$dir/impostor.dylib"
echo installed
