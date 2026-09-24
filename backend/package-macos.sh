#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ "$(uname -s)" == Darwin ]] || { echo 'Run this script on macOS.' >&2; exit 1; }
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
BUILD_DIR="${MACOS_BUILD_DIR:-build-macos}"
BACKEND_BINARY="$BUILD_DIR/cognitive-os-agent"
if [[ ! -x "$BACKEND_BINARY" ]]; then
  cmake -S . -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR" --target cognitive-os-agent -j "${JOBS:-3}"
fi
APP="dist/Cognitive OS.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
python3 tools/gen_icon.py
cp "$BACKEND_BINARY" "$APP/Contents/MacOS/cognitive-os-agent"
cp dist/CognitiveOS.icns "$APP/Contents/Resources/CognitiveOS.icns"
xcrun clang -fobjc-arc -Wall -Wextra -framework Cocoa -framework WebKit \
  tools/desktop_macos.m -o "$APP/Contents/MacOS/CognitiveOS"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.cognitiveos.desktop</string>
<key>CFBundleName</key><string>Cognitive OS</string>
<key>CFBundleDisplayName</key><string>Cognitive OS</string>
<key>CFBundleExecutable</key><string>CognitiveOS</string>
<key>CFBundleIconFile</key><string>CognitiveOS.icns</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.7.0</string>
<key>CFBundleVersion</key><string>8</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSAppTransportSecurity</key><dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict></plist>
PLIST
plutil -lint "$APP/Contents/Info.plist"
IDENTITY="${MACOS_SIGN_IDENTITY:--}"
codesign --force --options runtime --sign "$IDENTITY" "$APP/Contents/MacOS/cognitive-os-agent"
codesign --force --options runtime --sign "$IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"
mkdir -p dist
ditto -c -k --sequesterRsrc --keepParent "$APP" "dist/Cognitive-OS-macos-$(uname -m).zip"
echo "Built $APP and architecture-specific ZIP (identity: $IDENTITY)."
echo 'Public distribution additionally requires Developer ID signing and notarization.'
