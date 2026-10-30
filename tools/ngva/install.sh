#!/bin/sh
# Build the Tahoe VA front (ngva) and install it with the patched TGL VA driver
# as the accelerator's video bundle (run on the guest as root).
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d)
driver=/Library/Extensions/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsVADriver
bundle=/Library/GPUBundles/NootedGreenTGLMediaAdapter.bundle
clang++ -std=c++17 -O2 -bundle -Wno-vla-cxx-extension ngva.cpp -framework IOSurface -o "$work/ngva"
/usr/bin/python3 patch-vadriver.py "$driver" "$work/driver"
mkdir -p "$bundle/Contents/MacOS"
cat > "$bundle/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>NootedGreenTGLMediaAdapter</string>
  <key>CFBundleIdentifier</key><string>org.umeow.NootedGreenTGLMediaAdapter</string>
  <key>CFBundleName</key><string>NootedGreenTGLMediaAdapter</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
</dict></plist>
PLIST
install -m 755 "$work/ngva" "$bundle/Contents/MacOS/NootedGreenTGLMediaAdapter"
install -m 755 "$work/driver" "$bundle/Contents/MacOS/AppleIntelTGLGraphicsVADriver"
if ls kernels/*.gen12 >/dev/null 2>&1; then
	mkdir -p "$bundle/Contents/Resources"
	install -m 644 kernels/*.gen12 "$bundle/Contents/Resources/"
fi
codesign -f -s - "$bundle/Contents/MacOS/AppleIntelTGLGraphicsVADriver"
codesign -f -s - "$bundle"
rm -rf "$work"
