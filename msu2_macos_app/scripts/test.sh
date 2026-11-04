#!/bin/bash
set -euo pipefail
APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE_ROOT="$(cd "$APP_ROOT/../msu2_linux_v0_3" && pwd)"
BUILD_DIR="${BUILD_DIR:-$APP_ROOT/build}"
mkdir -p "$BUILD_DIR/tests/obj"
CXX="${CXX:-clang++}"
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -mmacosx-version-min=13.0 -pthread -I"$APP_ROOT/include" -I"$CORE_ROOT/include")
# Optional offline ASan/UBSan run: SANITIZERS=1 BUILD_DIR=/path/to/isolated/build ./scripts/test.sh
if [[ "${SANITIZERS:-0}" == 1 ]]; then
    FLAGS+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
FRAMEWORKS=(-framework Cocoa -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo -framework ImageIO -framework IOKit -framework CoreGraphics)
OBJECTS=()
for source in protocol delta image session transport priority; do
    object="$BUILD_DIR/tests/obj/core_$source.o"
    "$CXX" "${FLAGS[@]}" -c "$CORE_ROOT/src/$source.cpp" -o "$object"
    OBJECTS+=("$object")
done
for source in controller diagnostic_log serial_guard flash preview; do
    object="$BUILD_DIR/tests/obj/$source.o"
    "$CXX" "${FLAGS[@]}" "-DMSU2_WORKSPACE_ROOT=\"$APP_ROOT/..\"" -c "$APP_ROOT/src/$source.cpp" -o "$object"
    OBJECTS+=("$object")
done
"$CXX" "${FLAGS[@]}" -fobjc-arc -c "$APP_ROOT/src/platform.mm" -o "$BUILD_DIR/tests/obj/platform.o"
OBJECTS+=("$BUILD_DIR/tests/obj/platform.o")
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_flash.cpp" "$APP_ROOT/src/flash.cpp" -o "$BUILD_DIR/tests/test_flash"
"$BUILD_DIR/tests/test_flash"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_preview.cpp" "$APP_ROOT/src/preview.cpp" "$BUILD_DIR/tests/obj/core_image.o" -o "$BUILD_DIR/tests/test_preview"
"$BUILD_DIR/tests/test_preview"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_controller.cpp" "${OBJECTS[@]}" "${FRAMEWORKS[@]}" -o "$BUILD_DIR/tests/test_controller"
"$BUILD_DIR/tests/test_controller" "$BUILD_DIR/test-evidence"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_session_trace.cpp" "$BUILD_DIR/tests/obj/core_session.o" "$BUILD_DIR/tests/obj/core_protocol.o" -o "$BUILD_DIR/tests/test_session_trace"
"$BUILD_DIR/tests/test_session_trace"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_controller_trace.cpp" "${OBJECTS[@]}" "${FRAMEWORKS[@]}" -o "$BUILD_DIR/tests/test_controller_trace"
"$BUILD_DIR/tests/test_controller_trace" "$BUILD_DIR/trace-test-evidence"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_controller_edges.cpp" "${OBJECTS[@]}" "${FRAMEWORKS[@]}" -o "$BUILD_DIR/tests/test_controller_edges"
"$BUILD_DIR/tests/test_controller_edges" "$BUILD_DIR/test-evidence"
"$CXX" "${FLAGS[@]}" "$APP_ROOT/tests/test_download.cpp" "${OBJECTS[@]}" "${FRAMEWORKS[@]}" -o "$BUILD_DIR/tests/test_download"
"$BUILD_DIR/tests/test_download" "$BUILD_DIR/download-test-evidence"
"$CXX" "${FLAGS[@]}" -fobjc-arc "$APP_ROOT/tests/platform_smoke.mm" "$BUILD_DIR/tests/obj/core_image.o" "$BUILD_DIR/tests/obj/platform.o" "${FRAMEWORKS[@]}" -o "$BUILD_DIR/tests/platform_smoke"
"$BUILD_DIR/tests/platform_smoke"
for name in core delta priority; do
    "$CXX" "${FLAGS[@]}" "$CORE_ROOT/tests/test_$name.cpp" "$BUILD_DIR"/tests/obj/core_*.o -o "$BUILD_DIR/tests/test_$name"
    "$BUILD_DIR/tests/test_$name"
done
