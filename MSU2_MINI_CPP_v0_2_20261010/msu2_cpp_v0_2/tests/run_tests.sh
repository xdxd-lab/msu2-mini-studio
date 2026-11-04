#!/usr/bin/env bash
set -euo pipefail
project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${MSU2_TEST_BUILD:-${project}/tests/.build}"
cxx="${CXX:-c++}"
mkdir -p "$build"
common=(-std=c++17 -Wall -Wextra -Wpedantic -pthread -I "$project/include")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    common+=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
elif [[ "${DEBUG:-0}" == 1 ]]; then
    common+=(-O0 -g)
else
    common+=(-O3 -DNDEBUG)
fi
"$cxx" "${common[@]}" "$project/tests/test_core.cpp" "$project/src/protocol.cpp" \
    "$project/src/image.cpp" "$project/src/session.cpp" -o "$build/test_core"
"$build/test_core"
"$cxx" "${common[@]}" "$project/tests/test_delta.cpp" "$project/src/protocol.cpp" \
    "$project/src/delta.cpp" -o "$build/test_delta"
"$build/test_delta"
if [[ "${CORE_ONLY:-0}" != 1 ]]; then
    "$cxx" "${common[@]}" "$project/src/main.cpp" "$project/src/protocol.cpp" \
        "$project/src/delta.cpp" "$project/src/image.cpp" "$project/src/session.cpp" "$project/src/transport.cpp" \
        "$project/src/capture.cpp" -o "$build/msu2"
    python3 "$project/tests/integration_pty.py" "$build/msu2"
fi
