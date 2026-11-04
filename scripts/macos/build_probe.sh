#!/usr/bin/env bash
# Build only; the resulting executable requires an explicitly supplied port.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
source_dir="${1:-$root/msu2_cpp_v0_2}"
mkdir -p "$root/results/macos"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -pthread -I"$source_dir/include" \
  "$root/tests/macos/serial_probe.cpp" "$source_dir/src/transport.cpp" \
  "$source_dir/src/session.cpp" "$source_dir/src/protocol.cpp" "$source_dir/src/image.cpp" \
  -o "$root/results/macos/serial_probe"
echo "Built $root/results/macos/serial_probe"
echo 'Usage: serial_probe PORT [--bars settle_ms [hold_ms]]'
echo 'Default: handshake only. --bars: one LCD color pattern, no flash writes.'
