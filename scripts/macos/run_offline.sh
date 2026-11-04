#!/usr/bin/env bash
# Native build, upstream core/PTY regression, sanitizer core, and byte benchmark.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
source_dir="${1:-$root/msu2_cpp_v0_2}"
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'This entry point verifies macOS. Run the upstream scripts on other OSes.' >&2
  exit 2
fi
if [[ ! -f "$source_dir/src/main.cpp" || ! -f "$source_dir/tests/run_tests.sh" ]]; then
  echo "Missing v0.2 sources: $source_dir" >&2
  echo 'Put the original msu2_cpp_v0_2 folder in this project, or pass its path as argument 1.' >&2
  exit 2
fi
source_dir="$(cd "$source_dir" && pwd)"
mkdir -p "$root/results/macos"
run_dir="$(mktemp -d "$root/results/macos/$(TZ=Asia/Taipei date +%Y%m%dT%H%M%S)_offline_XXXXXX")"
echo "Results: $run_dir"
printf '%s\n' "$source_dir" > "$run_dir/source_path.txt"
cd "$source_dir"
bash scripts/build.sh 2>&1 | tee "$run_dir/build.log"
file build/msu2 | tee "$run_dir/binary_type.txt"
shasum -a 256 build/msu2 > "$run_dir/binary.sha256"
./build/msu2 --help > "$run_dir/help.txt"
# PTY emulation never opens the USB device. A denied PTY requires the host's approval.
MSU2_TEST_BUILD="$run_dir/release" bash tests/run_tests.sh 2>&1 | tee "$run_dir/release_tests.log"
# Apple ASan has no supported leak detector here; this is memory/UB checking only.
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 SANITIZE=1 CORE_ONLY=1 \
  MSU2_TEST_BUILD="$run_dir/sanitizer" bash tests/run_tests.sh 2>&1 | tee "$run_dir/sanitizer_tests.log"
./build/msu2 diff-bench --frames 300 --csv "$run_dir/diff_benchmark_macos.csv" \
  2>&1 | tee "$run_dir/diff_benchmark_macos.log"
echo 'Offline checks completed. This does not verify the physical LCD.' | tee "$run_dir/completed.txt"
