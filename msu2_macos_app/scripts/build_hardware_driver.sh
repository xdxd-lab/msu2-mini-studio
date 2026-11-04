#!/bin/bash
# Builds only. Hardware execution belongs to the appointed subagent.
set -euo pipefail
APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE_ROOT="$(cd "$APP_ROOT/../msu2_linux_v0_3" && pwd)"
BUILD_DIR="${BUILD_DIR:-$APP_ROOT/build}"
mkdir -p "$BUILD_DIR/tests"
clang++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -mmacosx-version-min=13.0 -pthread -I"$APP_ROOT/include" -I"$CORE_ROOT/include" \
  "$APP_ROOT/tests/hardware_session.cpp" "$BUILD_DIR"/obj/core_*.o "$BUILD_DIR/obj/controller.o" "$BUILD_DIR/obj/diagnostic_log.o" "$BUILD_DIR/obj/serial_guard.o" "$BUILD_DIR/obj/flash.o" "$BUILD_DIR/obj/platform.o" \
  -framework Cocoa -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo -framework ImageIO -framework IOKit -framework CoreGraphics \
  -o "$BUILD_DIR/tests/hardware_session"
shasum -a 256 "$BUILD_DIR/tests/hardware_session"
