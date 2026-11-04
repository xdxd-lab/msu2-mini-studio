#!/bin/bash
set -euo pipefail
APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE_ROOT="$(cd "$APP_ROOT/.." && pwd)"
CORE_ROOT="$WORKSPACE_ROOT/msu2_linux_v0_3"
BUILD_DIR="${BUILD_DIR:-$APP_ROOT/build}"
BUNDLE="$BUILD_DIR/MSU2 Mini Studio.app"
mkdir -p "$BUILD_DIR/obj" "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
CXX="${CXX:-clang++}"
FLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -mmacosx-version-min=13.0 -pthread -I"$APP_ROOT/include" -I"$CORE_ROOT/include")
OBJECTS=()
for source in protocol delta image session transport priority; do
    "$CXX" "${FLAGS[@]}" -c "$CORE_ROOT/src/$source.cpp" -o "$BUILD_DIR/obj/core_$source.o"
    OBJECTS+=("$BUILD_DIR/obj/core_$source.o")
done
for source in controller diagnostic_log serial_guard flash preview; do
    "$CXX" "${FLAGS[@]}" "-DMSU2_WORKSPACE_ROOT=\"$WORKSPACE_ROOT\"" -c "$APP_ROOT/src/$source.cpp" -o "$BUILD_DIR/obj/$source.o"
    OBJECTS+=("$BUILD_DIR/obj/$source.o")
done
"$CXX" "${FLAGS[@]}" -fobjc-arc -c "$APP_ROOT/src/platform.mm" -o "$BUILD_DIR/obj/platform.o"
OBJECTS+=("$BUILD_DIR/obj/platform.o")
FRAMEWORKS=(-framework Cocoa -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo -framework ImageIO -framework IOKit -framework CoreGraphics -framework UniformTypeIdentifiers)
"$CXX" "${FLAGS[@]}" -fobjc-arc -c "$APP_ROOT/src/ui_views.mm" -o "$BUILD_DIR/obj/ui_views.o"
"$CXX" "${FLAGS[@]}" -fobjc-arc "$APP_ROOT/src/app.mm" "$BUILD_DIR/obj/ui_views.o" "${OBJECTS[@]}" "${FRAMEWORKS[@]}" -o "$BUNDLE/Contents/MacOS/MSU2MiniStudio"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/src/cli.cpp" "${OBJECTS[@]}" "${FRAMEWORKS[@]}" -o "$BUNDLE/Contents/MacOS/msu2ctl"
cp "$APP_ROOT/resources/Info.plist" "$BUNDLE/Contents/Info.plist"
if [[ -f "$APP_ROOT/README_zh-TW.txt" ]]; then cp "$APP_ROOT/README_zh-TW.txt" "$BUNDLE/Contents/Resources/README_zh-TW.txt"; fi
SIGNING_IDENTITY="${MSU2_CODESIGN_IDENTITY:--}"
/usr/bin/codesign --force --sign "$SIGNING_IDENTITY" "$BUNDLE"
/usr/bin/codesign --verify --deep --strict "$BUNDLE"
/usr/bin/shasum -a 256 "$BUNDLE/Contents/MacOS/MSU2MiniStudio" "$BUNDLE/Contents/MacOS/msu2ctl" > "$BUILD_DIR/SHA256SUMS.txt"
printf 'Built: %s\n' "$BUNDLE"
if [[ "$SIGNING_IDENTITY" == "-" ]]; then
    printf '提醒：此版本採臨時簽章，重新建置後可能需要重新核准螢幕擷取權限。\n' >&2
fi
