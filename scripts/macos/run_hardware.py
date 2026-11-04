#!/usr/bin/env python3
"""Run a bounded macOS MSU2 Mini display test suite (Python 3.9+, stdlib).

This runner sends display streams only. A human must inspect the connected
display and record observations; process success does not establish visual
correctness. Use --dry-run to inspect commands without opening a serial port.
"""

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import time
import uuid
from typing import Any, Dict, List, Optional, Sequence


PROJECT_ROOT = Path(__file__).resolve().parents[2]
TAIPEI_TIMEZONE = dt.timezone(dt.timedelta(hours=8), "Asia/Taipei")
_ANALYZER = None


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def nonnegative_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be a finite nonnegative number")
    return number


def chunk_size(value: str) -> int:
    number = int(value)
    if number < 6 or number > 65532 or number % 6:
        raise argparse.ArgumentTypeError("must be 6..65532 and divisible by 6")
    return number


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./msu2_cpp_v0_2/build/msu2",
                        help="path to the compiled v0.2 msu2 executable")
    parser.add_argument("--port", required=True,
                        help="explicit serial port, e.g. /dev/cu.usbserial-XXXX")
    parser.add_argument("--suite", choices=("visual", "smoke", "baseline", "extended", "soak"),
                        default="smoke")
    parser.add_argument("--dry-run", action="store_true",
                        help="save and print commands; do not launch the binary")
    parser.add_argument("--repeats", type=positive_int, default=None,
                        help="suite repetitions (baseline: 3; other suites: 1)")
    parser.add_argument("--case", dest="case_name",
                        help="run only this named case from the selected suite (use --dry-run to list names)")
    parser.add_argument("--seconds", type=positive_int, default=300,
                        help="duration of each soak case in seconds (default: 300)")
    parser.add_argument("--settle-seconds", type=nonnegative_float, default=3.0,
                        help="wait between cases for device discovery (default: 3)")
    parser.add_argument("--chunk", type=chunk_size, default=3840,
                        help="payload write chunk, multiple of 6 (default: 3840; diagnostic: 60)")
    parser.add_argument("--results-root", "--output-root", dest="results_root",
                        type=Path, default=PROJECT_ROOT / "results" / "macos",
                        help="parent directory for unique run directories")
    args = parser.parse_args(argv)
    if not args.port.strip():
        parser.error("--port must not be blank")
    if args.repeats is None:
        args.repeats = 3 if args.suite == "baseline" else 1
    return args


def make_case(name: str, pattern: str, update: str, fps: int, frames: int,
              mode: str = "pair", keyframe_ms: Optional[int] = None) -> Dict[str, Any]:
    return {"name": name, "pattern": pattern, "update": update, "fps": fps,
            "expected_frames": frames, "mode": mode, "keyframe_ms": keyframe_ms}


def build_cases(suite: str, repeats: int, seconds: int) -> List[Dict[str, Any]]:
    if suite == "visual":
        base = [make_case("bars_visual_30s", "bars", "full", 15, 450)]
    elif suite == "smoke":
        base = [make_case("bars_full_15", "bars", "full", 15, 45),
                make_case("moving_diff_30", "moving", "diff", 30, 90,
                          keyframe_ms=0)]
    elif suite == "baseline":
        base = [make_case("moving_full_30", "moving", "full", 30, 300),
                make_case("moving_diff_30", "moving", "diff", 30, 300,
                          keyframe_ms=0),
                make_case("moving_diff_45", "moving", "diff", 45, 450,
                          keyframe_ms=0),
                make_case("moving_diff_60", "moving", "diff", 60, 600,
                          keyframe_ms=0)]
    elif suite == "extended":
        base = [make_case("bars_diff_kf0", "bars", "diff", 30, 300,
                          keyframe_ms=0),
                make_case("bars_diff_kf1000", "bars", "diff", 30, 300,
                          keyframe_ms=1000),
                make_case("moving_diff_60_kf1000", "moving", "diff", 60, 600,
                          keyframe_ms=1000),
                make_case("gradient_diff", "gradient", "diff", 30, 300,
                          keyframe_ms=0),
                make_case("noise_diff", "noise", "diff", 30, 300,
                          keyframe_ms=0),
                make_case("moving_raw_diff", "moving", "diff", 30, 300,
                          mode="raw", keyframe_ms=0)]
    elif suite == "soak":
        base = [make_case("moving_diff_soak_kf1000", "moving", "diff", 60,
                          seconds * 60, keyframe_ms=1000),
                make_case("bars_diff_soak_kf0", "bars", "diff", 60,
                          seconds * 60, keyframe_ms=0)]
    else:
        raise ValueError("unknown suite: " + suite)

    cases = []
    for repeat in range(1, repeats + 1):
        # Reverse alternate baseline repetitions to reduce order bias.
        ordered = list(reversed(base)) if suite == "baseline" and repeat % 2 == 0 else base
        for case in ordered:
            cases.append(dict(case, repeat=repeat))
    return cases


def build_command(binary: Path, port: str, case: Dict[str, Any], csv_path: Path) -> List[str]:
    command = [str(binary), "stream", "--port", port, "--pattern", case["pattern"],
               "--mode", case["mode"], "--update", case["update"],
               "--fps", str(case["fps"]), "--frames", str(case["expected_frames"]),
               "--csv", str(csv_path), "--timeout-ms", "2000"]
    command.extend(["--chunk", str(case.get("chunk", 3840))])
    if case["update"] == "diff" and case["keyframe_ms"] is not None:
        command.extend(["--keyframe-ms", str(case["keyframe_ms"])])
    return command


def sha256_file(path: Path) -> Optional[str]:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def create_run_directory(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(TAIPEI_TIMEZONE).strftime("%Y%m%dT%H%M%S_%f%z")
    run_root = root / (stamp + "_" + uuid.uuid4().hex[:8])
    run_root.mkdir(exist_ok=False)
    hardware = run_root / "hardware"
    hardware.mkdir()
    return hardware


def save_manifest(path: Path, manifest: Dict[str, Any]) -> None:
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def write_visual_checklist(path: Path, cases: List[Dict[str, Any]]) -> None:
    lines = ["MSU2 Mini — visual observations / 人工畫面觀察",
             "Process success alone does not establish visual correctness.",
             "命令成功不代表畫面正確；請實際觀察後填寫，未填一律視為未驗證。",
             "Observer / 觀察者：", "Device / 設備、韌體：", "Date / 日期：", ""]
    for case in cases:
        lines.extend([case["stem"],
                      "[ ] 圖案與預期一致（bars / moving / gradient / noise）",
                      "[ ] 局部更新沒有殘影、撕裂、錯位或漏更新",
                      "[ ] 靜態畫面穩定／動態畫面持續更新",
                      "Outcome / 結果（pass / fail / not observed）：",
                      "Notes / 異常時間、現象：", ""])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def stop_process(process: subprocess.Popen) -> None:
    """Bounded cleanup on an interrupted runner; preserve any CSV already written."""
    if process.poll() is None:
        process.terminate()
    try:
        process.communicate(timeout=5)
    except (subprocess.TimeoutExpired, OSError, KeyboardInterrupt):
        process.kill()
        # Output goes directly to a file, so wait can reap the child even when
        # communicate itself failed. Keep the device lease during cleanup.
        while True:
            try:
                process.wait()
                break
            except KeyboardInterrupt:
                continue


def analyze_recording(csv_path: str, log_path: str) -> Dict[str, Any]:
    """Load the sibling validator even when this runner is imported by file path."""
    global _ANALYZER
    if _ANALYZER is None:
        path = Path(__file__).resolve().with_name("analyze_results.py")
        spec = importlib.util.spec_from_file_location("msu2_recording_analyzer", path)
        if spec is None or spec.loader is None:
            raise ImportError("Cannot load recording validator: " + str(path))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _ANALYZER = module
    # The current run is still in progress. Validate its recordings here, then
    # check the requested frame count below rather than validating that manifest.
    return _ANALYZER.analyze_case(csv_path, log_path, manifest_paths=())


def validate_case(case: Dict[str, Any]) -> bool:
    case["status"] = "validating"
    try:
        result = analyze_recording(case["csv_path"], case["log_path"])
        errors = list(result.get("errors", []))
        captured = result.get("final_log", {}).get("captured")
        if captured != case["expected_frames"]:
            errors.append("Requested {} frames but final captured={}".format(
                case["expected_frames"], captured))
        if result.get("status") != "PASS" and not errors:
            errors.append("Recording validator did not return PASS")
    except KeyboardInterrupt:
        case["status"] = "interrupted"
        case["error"] = "user interrupted recording validation"
        raise
    except Exception as error:
        errors = ["Recording validation error: {}: {}".format(type(error).__name__, error)]
    # Keep the manifest compact, even if a damaged CSV generates many errors.
    case["validation_error_count"] = len(errors)
    case["validation_errors"] = [str(error)[:300] for error in errors[:8]]
    if errors:
        case["status"] = "failed"
        case["error"] = "Recording validation failed: " + str(errors[0])[:300]
        return False
    case["status"] = "passed"
    return True


def guard_command(command):
    path = Path(__file__).resolve().with_name("serial_guard.py")
    spec = importlib.util.spec_from_file_location("msu2_serial_guard", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.guard_command(command)


def execute_case(case: Dict[str, Any]) -> bool:
    # Import by path too: unit tests load this runner without adding its directory.
    with guard_command(case["command"]) as lease:
        if lease is not None:
            case["serial_guard"] = lease.evidence
        return _execute_case_unlocked(case)


def _execute_case_unlocked(case: Dict[str, Any]) -> bool:
    started = time.monotonic()
    case["started_at"] = utc_now()
    case["status"] = "running"
    case["exit_code"] = None
    process = None
    # Child stdout/stderr go straight to disk, including output before failure.
    try:
        if "binary_sha256_expected" in case:
            case["binary_sha256_before"] = sha256_file(Path(case["command"][0]))
            if case["binary_sha256_before"] != case["binary_sha256_expected"]:
                case["status"] = "failed"
                case["error"] = "binary changed since run preparation; case was not launched"
                return False
        with Path(case["log_path"]).open("x", encoding="utf-8") as log:
            log.write("Command: " + shlex.join(case["command"]) + "\n\n")
            log.flush()
            process = subprocess.Popen(case["command"], stdout=log, stderr=subprocess.STDOUT)
            try:
                process.communicate(timeout=case["expected_frames"] / case["fps"] + 60)
                case["exit_code"] = process.returncode
                case["status"] = "process_completed" if process.returncode == 0 else "failed"
                if process.returncode != 0:
                    case["error"] = "process returned a nonzero exit code"
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                case["exit_code"] = process.returncode
                case["status"] = "failed"
                case["error"] = "process exceeded frames/fps + 60 seconds"
            except KeyboardInterrupt:
                stop_process(process)
                case["exit_code"] = process.returncode
                case["status"] = "interrupted"
                case["error"] = "user interrupted; partial output must not be treated as passing"
                raise
    except OSError as error:
        case["status"] = "failed"
        case["error"] = str(error)
    finally:
        if process is not None and process.returncode is None:
            stop_process(process)
            case["exit_code"] = process.returncode
        if "binary_sha256_before" in case:
            try:
                case["binary_sha256_after"] = sha256_file(Path(case["command"][0]))
                if (case["binary_sha256_before"] == case["binary_sha256_expected"]
                        and case["binary_sha256_after"] != case["binary_sha256_expected"]):
                    case["status"] = "failed"
                    case["error"] = "binary changed during the run; results cannot be accepted"
            except OSError as error:
                case["status"] = "failed"
                case["error"] = "Cannot verify binary after run: " + str(error)
        case["completed_at"] = utc_now()
        case["duration_seconds"] = round(time.monotonic() - started, 3)
        case["csv_exists"] = Path(case["csv_path"]).is_file()
        case["csv_size_bytes"] = Path(case["csv_path"]).stat().st_size if case["csv_exists"] else 0
    if case["status"] == "process_completed":
        if not case["csv_size_bytes"]:
            case["status"] = "failed"
            case["error"] = "process exited successfully but produced no nonempty CSV"
        else:
            return validate_case(case)
    return case["status"] == "passed"


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    binary = Path(args.binary).expanduser().resolve()
    if not args.dry_run and (not binary.is_file() or not os.access(str(binary), os.X_OK)):
        print("Executable not found or not executable: " + str(binary), file=sys.stderr)
        print("Build the v0.2 source first, or use --dry-run to inspect the suite.", file=sys.stderr)
        return 2

    cases = build_cases(args.suite, args.repeats, args.seconds)
    if args.case_name:
        cases = [case for case in cases if case["name"] == args.case_name]
        if not cases:
            print("No case named " + args.case_name + " in suite " + args.suite, file=sys.stderr)
            return 2
    hardware = create_run_directory(args.results_root.expanduser().resolve())
    for index, case in enumerate(cases, 1):
        case["chunk"] = args.chunk
        stem = "{:02d}_r{:02d}_{}".format(index, case["repeat"], case["name"])
        csv_path = hardware / (stem + ".csv")
        case.update({"stem": stem, "csv_path": str(csv_path),
                     "log_path": str(hardware / (stem + ".log")),
                     "command": build_command(binary, args.port, case, csv_path),
                     "status": "planned", "exit_code": None})
    arguments = vars(args).copy()
    arguments["results_root"] = str(arguments["results_root"])
    manifest = {"schema_version": 1, "suite": args.suite,
                "status": "dry_run" if args.dry_run else "running",
                "started_at": utc_now(), "completed_at": None,
                "binary": {"path": str(binary), "sha256": sha256_file(binary)},
                "platform": {"system": platform.system(), "release": platform.release(),
                             "machine": platform.machine(), "python": platform.python_version(),
                             "macos": platform.mac_ver()[0]},
                "arguments": arguments, "visual_verification": "not_observed",
                "cases": cases}
    manifest_path = hardware / "manifest.json"
    save_manifest(manifest_path, manifest)
    write_visual_checklist(hardware / "visual_observations.txt", cases)
    print("Results: " + str(hardware), flush=True)
    exit_code = 0
    try:
        if args.dry_run:
            for case in cases:
                print(shlex.join(case["command"]), flush=True)
                case["status"] = "dry_run"
        else:
            for index, case in enumerate(cases):
                if index and args.settle_seconds:
                    time.sleep(args.settle_seconds)
                print("[{}/{}] {}".format(index + 1, len(cases), case["stem"]), flush=True)
                case["status"] = "running"
                case["binary_sha256_expected"] = manifest["binary"]["sha256"]
                save_manifest(manifest_path, manifest)
                success = execute_case(case)
                save_manifest(manifest_path, manifest)
                print("  " + case["status"] + " — " + case["log_path"], flush=True)
                if not success:
                    manifest["status"] = "failed"
                    exit_code = 1
                    break
            else:
                manifest["status"] = "completed"
    except RuntimeError as error:
        manifest["status"] = "blocked"
        manifest["stop_reason"] = str(error)
        exit_code = 1
        print("No concurrent test launched: " + str(error), file=sys.stderr)
    except KeyboardInterrupt:
        manifest["status"] = "interrupted"
        exit_code = 130
        print("Interrupted; partial results saved. Remaining cases were not run.", file=sys.stderr)
    finally:
        manifest["completed_at"] = utc_now()
        for case in cases:
            if case["status"] == "planned":
                case["status"] = "not_run"
        save_manifest(manifest_path, manifest)
    if args.dry_run:
        print("Dry run only; no binary was launched and no device was opened.")
    else:
        print("Record screen observations in: " + str(hardware / "visual_observations.txt"))
    print("Analysis command: " + shlex.join([sys.executable,
          str(PROJECT_ROOT / "scripts" / "macos" / "analyze_results.py"), str(hardware)]))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
