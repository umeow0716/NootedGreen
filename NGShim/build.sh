#!/bin/sh
# Build the TGL UMD compatibility shim (installed as the UMD's impostor.dylib).
set -eu
cd "$(dirname "$0")"
mkdir -p build
clang -arch x86_64 -dynamiclib -fobjc-arc -O2 -Wall -mmacosx-version-min=14.0 \
  -install_name @loader_path/impostor.dylib \
  -framework Foundation -framework Metal -o build/impostor.dylib ngshim.m
codesign -f -s - build/impostor.dylib
echo built build/impostor.dylib
