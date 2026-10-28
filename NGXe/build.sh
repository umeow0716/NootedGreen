#!/bin/sh
# Build NGXe.kext on macOS with the Command Line Tools.
#   build.sh [Lilu Resources dir] [MacKernelSDK dir]   (defaults: this repo's copies)
set -eu
cd "$(dirname "$0")"
LILU=$(cd "${1:-../Lilu.kext/Contents/Resources}" && pwd)
KSDK=$(cd "${2:-../MacKernelSDK}" && pwd)
SDK=$(xcrun --show-sdk-path)
OUT=build/NGXe.kext
rm -rf build
mkdir -p "$OUT/Contents/MacOS"
KFLAGS="-arch x86_64 -O2 -mkernel -nostdlib -fno-builtin -fno-stack-protector \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -DPRODUCT_NAME=NGXe -DMODULE_VERSION=1.0.0 -DLILU_KEXTPATCH_SUPPORT \
  -isysroot $SDK -I$KSDK/Headers -I$LILU -mmacosx-version-min=11.0"
clang $KFLAGS -c kmod_info.c -o build/kmod_info.o
clang++ $KFLAGS -std=c++17 -Wall -Wno-deprecated-declarations -Wno-unused-variable \
  -fapple-kext -fno-rtti -fno-exceptions \
  -Xlinker -kext -L$KSDK/Library/x86_64 -lkmod -lkmodc++ -lcc_kext \
  -o "$OUT/Contents/MacOS/NGXe" \
  start.cpp access.cpp render.cpp lv.cpp "$LILU/Library/plugin_start.cpp" build/kmod_info.o
cp Info.plist "$OUT/Contents/Info.plist"
codesign -f -s - "$OUT"
echo "built $OUT"
