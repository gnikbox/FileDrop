#!/bin/bash
# Build FileDrop.app cho macOS: lõi C++ (CMake) + app Swift (swiftc) -> build/macOS/FileDrop.app
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/macOS"
CONFIG="${CONFIG:-Release}"
ARCH="$(uname -m)"
APP="$BUILD/FileDrop.app"

echo "==> Build lõi C++ ($CONFIG)"
cmake -S "$ROOT" -B "$BUILD/core" -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 >/dev/null
cmake --build "$BUILD/core" -j "$(sysctl -n hw.ncpu)"

echo "==> Build app Swift"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

SWIFT_OPT="-O"
[ "$CONFIG" = "Debug" ] && SWIFT_OPT="-Onone -g"

swiftc $SWIFT_OPT -swift-version 5 \
    -target "$ARCH-apple-macos13.0" \
    -import-objc-header "$ROOT/shared/include/filedrop/filedrop_c.h" \
    "$ROOT"/macOS/FileDrop/Sources/*.swift \
    -L "$BUILD/core/shared" -lfiledrop_core -lc++ \
    -o "$APP/Contents/MacOS/FileDrop"

cp "$ROOT/macOS/FileDrop/Resources/Info.plist" "$APP/Contents/Info.plist"
if [ -f "$ROOT/macOS/FileDrop/Resources/AppIcon.icns" ]; then
    cp "$ROOT/macOS/FileDrop/Resources/AppIcon.icns" "$APP/Contents/Resources/"
fi

echo "==> Ký ad-hoc"
codesign --force --sign - "$APP" >/dev/null

echo "Xong: $APP"
