#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
mkdir -p results/macos
"${CXX:-c++}" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -pthread \
  -I msu2_cpp_v0_2/include \
  msu2_cpp_v0_2/src/protocol.cpp \
  msu2_cpp_v0_2/src/delta.cpp \
  msu2_cpp_v0_2/src/image.cpp \
  msu2_cpp_v0_2/src/session.cpp \
  tests/macos/transport_timed.cpp \
  msu2_cpp_v0_2/src/capture.cpp \
  msu2_cpp_v0_2/src/main.cpp \
  -o results/macos/msu2_timed
shasum -a 256 results/macos/msu2_timed
