#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
"${CXX:-c++}" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -pthread -Iinclude \
  src/protocol.cpp src/delta.cpp src/image.cpp src/session.cpp src/transport.cpp src/capture.cpp src/main.cpp -o build/msu2
echo "Built build/msu2"
