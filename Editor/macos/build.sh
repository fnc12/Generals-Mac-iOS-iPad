#!/bin/bash
# Builds "END Editor.app": the C++ core (CMake) and the SwiftUI client, which links the core's
# static library. Output: build/editor-macos/END Editor.app
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
CORE_BUILD="$ROOT/build/editor-core-release"
OUT="$ROOT/build/editor-macos"
APP="$OUT/END Editor.app"
export PATH="/opt/homebrew/bin:$PATH"

cmake -S "$HERE/../core" -B "$CORE_BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DEND_EDITOR_BUILD_TESTS=OFF -DEND_EDITOR_BUILD_CLI=OFF >/dev/null
cmake --build "$CORE_BUILD" --target end_editor_core

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
swiftc -O -parse-as-library \
    -import-objc-header "$HERE/bridge.h" \
    -I "$HERE/../core/include" \
    "$HERE/Main.swift" "$HERE/ModelPreview.swift" "$HERE/GifExport.swift" "$HERE/MapView.swift" "$HERE/PerfHarness.swift" \
    -L "$CORE_BUILD/lib" -lend_editor_core -lsqlite3 -lz -lc++ \
    -o "$APP/Contents/MacOS/END Editor"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>END Editor</string>
    <key>CFBundleExecutable</key><string>END Editor</string>
    <key>CFBundleIdentifier</key><string>studio.fnc.end-editor</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP" >/dev/null
echo "built: $APP"
