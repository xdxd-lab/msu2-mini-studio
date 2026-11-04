#!/usr/bin/env python3
"""Validate and summarize MSU2 v0.2 CSV/log recordings using only stdlib.

PASS means recording consistency, not visual correctness or measured LCD FPS.
The v0.2 writer records every processed frame, including unchanged frames.
An unchanged row has no bytes, writes, rectangles, or transmitted pixels.
host_latency_ms excludes capture_ms; encode_ms includes diff planning.
"""

import argparse
import csv
import io
import json
import math
from pathlib import Path
import re
import statistics
import sys


CSV_FIELDS = [
    "frame_id", "capture_ms", "convert_ms", "encode_ms", "window_ack_ms",
    "payload_write_ms", "host_latency_ms", "wire_bytes", "write_calls",
    "dropped_total", "changed_pixels", "transmitted_pixels", "rectangles",
    "update_kind", "unchanged_total",
]
TIME_FIELDS = [name for name in CSV_FIELDS if name.endswith("_ms")]
KINDS = {"full", "bounding_box", "tiles", "runs", "unchanged"}
FINAL_INT_FIELDS = [
    "captured", "host_frames_sent", "waiting_frames_replaced", "bytes",
    "processed", "unchanged_frames", "full_frames", "partial_frames",
    "rectangles_sent",
]
FINAL_FLOAT_FIELDS = ["elapsed_s", "host_send_fps"]
METRICS = TIME_FIELDS + [
    "wire_bytes", "write_calls", "changed_pixels", "transmitted_pixels", "rectangles",
]
PAIR_RE = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s;]+)")


def read_text(path):
    """PowerShell Tee-Object logs can be UTF-16; macOS logs are UTF-8."""
    data = Path(path).read_bytes()
    if data.startswith((b"\xff\xfe\x00\x00", b"\x00\x00\xfe\xff")):
        return data.decode("utf-32")
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    return data.decode("utf-8-sig")


def numeric(value, name, integer=False):
    if integer:
        if not re.fullmatch(r"[0-9]+", value):
            raise ValueError("{} must be a nonnegative integer: {!r}".format(name, value))
        return int(value)
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ValueError("{} must be finite and nonnegative: {!r}".format(name, value))
    return result


def percentile(values, fraction):
    """Linear interpolation at index (n - 1) * fraction, including n=1."""
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(rows):
    result = {"count": len(rows), "metrics": {}}
    if not rows:
        return result
    for field in METRICS:
        values = [row[field] for row in rows]
        result["metrics"][field] = {
            "mean": statistics.mean(values),
            "p50": percentile(values, 0.50),
            "p95": percentile(values, 0.95),
            "min": min(values),
            "max": max(values),
        }
    return result


def read_csv(path):
    reader = csv.DictReader(io.StringIO(read_text(path)))
    if reader.fieldnames != CSV_FIELDS:
        raise ValueError("CSV header does not match the v0.2 schema in its original order")
    rows = []
    for line_number, raw in enumerate(reader, start=2):
        if None in raw or any(value is None for value in raw.values()):
            raise ValueError("CSV line {} has an incorrect field count".format(line_number))
        row = {}
        for name in CSV_FIELDS:
            value = raw[name]
            if name == "update_kind":
                if value not in KINDS:
                    raise ValueError("CSV line {}: unknown update_kind {!r}".format(line_number, value))
                row[name] = value
            else:
                row[name] = numeric(value, "CSV line {} {}".format(line_number, name), name not in TIME_FIELDS)
        rows.append(row)
    return rows


def read_log(path):
    contents = read_text(path)
    final_lines = []
    settings = {}
    for line in contents.splitlines():
        pairs = dict(PAIR_RE.findall(line))
        if "update" in pairs and "encoding" in pairs:
            settings.update(pairs)
        if "bytes" in pairs or "host_send_fps" in pairs:
            final_lines.append(pairs)
    if len(final_lines) != 1:
        raise ValueError("Expected exactly one final log summary; found {} (truncated or combined runs cannot PASS)".format(len(final_lines)))
    pairs = final_lines[0]
    final = {}
    for name in FINAL_INT_FIELDS + FINAL_FLOAT_FIELDS:
        if name not in pairs:
            raise ValueError("Final log summary is missing {}".format(name))
        final[name] = numeric(pairs[name], "log " + name, name in FINAL_INT_FIELDS)
    return final, settings, "MSN handshake verified" in contents


def find_manifest_case(csv_path, log_path, manifest_paths):
    """Return matching case; absolute paths or paths relative to manifest work."""
    csv_path, log_path = Path(csv_path).resolve(), Path(log_path).resolve()
    for manifest_path in manifest_paths:
        data = json.loads(read_text(manifest_path))
        if not isinstance(data, dict) or not isinstance(data.get("cases"), list):
            raise ValueError("Invalid manifest cases in {}".format(manifest_path))
        for case in data["cases"]:
            if not isinstance(case, dict):
                raise ValueError("Invalid case entry in {}".format(manifest_path))
            recorded = case.get("csv_path")
            if not isinstance(recorded, str):
                continue
            recorded_path = Path(recorded)
            if not recorded_path.is_absolute():
                recorded_path = manifest_path.parent / recorded_path
            if recorded_path.resolve() != csv_path:
                continue
            recorded_log = Path(case.get("log_path", ""))
            if not recorded_log.is_absolute():
                recorded_log = manifest_path.parent / recorded_log
            if recorded_log.resolve() != log_path:
                raise ValueError("Manifest log_path does not match the selected log")
            return {"path": str(manifest_path.resolve()), "run_status": data.get("status"), "case": case}
    return None


def analyze_case(csv_path, log_path, manifest_paths=()):
    csv_path, log_path = Path(csv_path), Path(log_path)
    result = {
        "name": csv_path.stem,
        "csv_path": str(csv_path.resolve()),
        "log_path": str(log_path.resolve()),
        "status": "FAIL",
        "errors": [],
        "warnings": [],
    }
    errors = result["errors"]
    try:
        rows = read_csv(csv_path)
    except (OSError, UnicodeError, ValueError, TypeError, csv.Error) as error:
        errors.append(str(error))
        return result
    # A failed stream may still contain useful completed updates. Retain those
    # statistics without inferring final totals, duration, or a successful run.
    final = None
    try:
        final, settings, handshake_verified = read_log(log_path)
        result["settings"] = settings
        result["final_log"] = final
        result["handshake_verified"] = handshake_verified
        if not handshake_verified:
            errors.append("Log does not confirm MSN handshake verification")
    except (OSError, UnicodeError, ValueError, TypeError) as error:
        errors.append(str(error))
        result["warnings"].append("CSV statistics cover recorded completed updates only; final totals and host FPS are unavailable")
    result["statistics_scope"] = "completed CSV rows only"
    if not rows:
        errors.append("CSV has no processed frames")
    sent = [row for row in rows if row["update_kind"] != "unchanged"]
    unchanged = [row for row in rows if row["update_kind"] == "unchanged"]
    full = [row for row in sent if row["update_kind"] == "full"]
    partial = [row for row in sent if row["update_kind"] != "full"]
    initial_full = sent[:1] if sent and sent[0]["update_kind"] == "full" else []
    if sent and not initial_full:
        errors.append("First sent frame is not a full frame")
    steady_sent = sent[1:] if initial_full else sent
    result["csv"] = {
        "processed_rows": len(rows), "sent_rows": len(sent), "unchanged_rows": len(unchanged),
        "first_frame_id": rows[0]["frame_id"] if rows else None,
        "last_frame_id": rows[-1]["frame_id"] if rows else None,
        "last_observed_dropped_total": rows[-1]["dropped_total"] if rows else None,
        "wire_bytes": sum(row["wire_bytes"] for row in rows),
        "write_calls": sum(row["write_calls"] for row in rows),
        "rectangles": sum(row["rectangles"] for row in rows),
        "update_kinds": {kind: sum(row["update_kind"] == kind for row in rows) for kind in sorted(KINDS)},
    }
    result["statistics"] = {
        "initial_full": summarize(initial_full),
        "steady_sent_including_later_full": summarize(steady_sent),
        "later_full": summarize([row for row in steady_sent if row["update_kind"] == "full"]),
        "partial_sent": summarize(partial),
        "unchanged": summarize(unchanged),
        "all_sent": summarize(sent),
    }
    if initial_full:
        result["initial_full_frame_id"] = initial_full[0]["frame_id"]
    previous = None
    unchanged_so_far = 0
    for row in rows:
        label = "frame_id {}".format(row["frame_id"])
        if previous is not None:
            if row["frame_id"] <= previous["frame_id"]:
                errors.append(label + ": frame_id is not strictly increasing")
            if row["dropped_total"] < previous["dropped_total"]:
                errors.append(label + ": dropped_total decreased")
        if row["update_kind"] == "unchanged":
            unchanged_so_far += 1
            for field in ("wire_bytes", "write_calls", "rectangles", "transmitted_pixels", "changed_pixels"):
                if row[field] != 0:
                    errors.append(label + ": unchanged row has nonzero " + field)
        else:
            for field in ("wire_bytes", "write_calls", "rectangles", "transmitted_pixels"):
                if row[field] == 0:
                    errors.append(label + ": sent row has zero " + field)
        if row["unchanged_total"] != unchanged_so_far:
            errors.append(label + ": unchanged_total does not match preceding unchanged rows")
        if row["changed_pixels"] > row["transmitted_pixels"]:
            errors.append(label + ": changed_pixels exceeds transmitted_pixels")
        previous = row

    if final is not None:
        expected = {
            "processed": len(rows), "host_frames_sent": len(sent), "unchanged_frames": len(unchanged),
            "full_frames": len(full), "partial_frames": len(partial),
            "bytes": result["csv"]["wire_bytes"], "rectangles_sent": result["csv"]["rectangles"],
        }
        if rows:
            expected["waiting_frames_replaced"] = rows[-1]["dropped_total"]
        for key, value in expected.items():
            if final[key] != value:
                errors.append("Final {}={} disagrees with CSV {}".format(key, final[key], value))
        accounted = final["host_frames_sent"] + final["unchanged_frames"] + final["waiting_frames_replaced"]
        if final["captured"] != accounted:
            errors.append("Final captured={} != sent + unchanged + dropped ({})".format(final["captured"], accounted))
        if final["elapsed_s"] <= 0:
            errors.append("Final elapsed_s must be positive")
        else:
            calculated_fps = final["host_frames_sent"] / final["elapsed_s"]
            result["calculated_host_send_fps"] = calculated_fps
            result["wire_bytes_per_second"] = final["bytes"] / final["elapsed_s"]
            if not math.isclose(calculated_fps, final["host_send_fps"], rel_tol=0.001, abs_tol=0.01):
                errors.append("Final host_send_fps disagrees with sent / elapsed_s beyond rounding tolerance")

    try:
        manifest = find_manifest_case(csv_path, log_path, manifest_paths)
        result["manifest"] = manifest
        if manifest is None:
            result["warnings"].append("No matching manifest: requested frame count and process exit status are unverified")
        else:
            case = manifest["case"]
            if manifest["run_status"] != "completed":
                errors.append("Manifest run did not finish with status=completed")
            if case.get("status") != "passed" or case.get("exit_code") != 0:
                errors.append("Manifest case did not finish with status=passed and exit_code=0")
            if type(case.get("expected_frames")) is not int or case["expected_frames"] <= 0:
                errors.append("Manifest expected_frames must be a positive integer")
            elif final is not None and case["expected_frames"] != final["captured"]:
                errors.append("Manifest expected_frames={} disagrees with captured={}".format(case["expected_frames"], final["captured"]))
    except (OSError, UnicodeError, ValueError, TypeError, KeyError) as error:
        errors.append("Manifest: " + str(error))
    result["status"] = "FAIL" if errors else "PASS"
    return result


def manifest_candidates(csv_path, root=None):
    parent = Path(csv_path).resolve().parent
    limit = Path(root).resolve().parent if root else parent.parent
    paths = []
    while True:
        candidate = parent / "manifest.json"
        if candidate.is_file():
            paths.append(candidate)
        if parent == limit or parent == parent.parent:
            break
        parent = parent.parent
    return paths


def missing_manifest_recordings(root, known_csv_paths):
    """Do not overlook a requested case whose CSV and log are both absent."""
    cases = []
    known = {path.resolve() for path in known_csv_paths}
    for manifest_path in sorted(root.rglob("manifest.json")):
        try:
            data = json.loads(read_text(manifest_path))
            if not isinstance(data, dict) or not isinstance(data.get("cases"), list):
                raise ValueError("Manifest has no cases array")
            for case in data["cases"]:
                if not isinstance(case, dict) or not isinstance(case.get("csv_path"), str) or not isinstance(case.get("log_path"), str):
                    raise ValueError("Manifest case must contain csv_path and log_path")
                csv_path, log_path = Path(case["csv_path"]), Path(case["log_path"])
                if not csv_path.is_absolute():
                    csv_path = manifest_path.parent / csv_path
                if not log_path.is_absolute():
                    log_path = manifest_path.parent / log_path
                if csv_path.resolve() not in known:
                    cases.append(analyze_case(csv_path, log_path, [manifest_path]))
                    known.add(csv_path.resolve())
        except (OSError, UnicodeError, ValueError, TypeError) as error:
            cases.append({"name": str(manifest_path), "status": "FAIL", "errors": ["Manifest: " + str(error)], "warnings": []})
    return cases


def report_table(report):
    lines = ["MSU2 v0.2 recording consistency (host send FPS is NOT LCD refresh FPS)",
             "Case                         Status  Sent  Unchanged  Bytes       Host FPS"]
    for case in report["cases"]:
        final = case.get("final_log", {})
        lines.append("{:<28} {:<6} {:>5} {:>10} {:>10} {:>10}".format(
            case["name"], case["status"], final.get("host_frames_sent", "-"),
            final.get("unchanged_frames", "-"), final.get("bytes", "-"), final.get("host_send_fps", "-")))
        if not final and "csv" in case:
            observed = case["csv"]
            lines.append("  Incomplete run: CSV contains {} completed updates, frame IDs {}..{}, {} bytes; no final host FPS".format(
                observed["processed_rows"], observed["first_frame_id"], observed["last_frame_id"], observed["wire_bytes"]))
        for label, key in (("initial full", "initial_full"), ("steady sent (includes later full)", "steady_sent_including_later_full"), ("later full", "later_full")):
            group = case.get("statistics", {}).get(key, {})
            timing = group.get("metrics", {}).get("host_latency_ms")
            if timing:
                lines.append("  {}: n={} host latency ms mean={:.4f} P50={:.4f} P95={:.4f}".format(
                    label, group["count"], timing["mean"], timing["p50"], timing["p95"]))
        for error in case["errors"]:
            lines.append("  ERROR: " + error)
        for warning in case["warnings"]:
            lines.append("  NOTE: " + warning)
    if not report["cases"]:
        lines.append("ERROR: No CSV/log recordings found")
    lines.append("Overall: {} (recording consistency only; visual/hardware acceptance remains separate)".format(report["status"]))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_dir", nargs="?", type=Path, help="Recursively inspect paired *.csv / *.log recordings")
    parser.add_argument("--csv", type=Path, help="CSV recording for one case")
    parser.add_argument("--log", type=Path, help="Log recording for one case")
    parser.add_argument("--output", type=Path, help="Also save the complete JSON report to this file")
    args = parser.parse_args(argv)
    if args.result_dir and (args.csv or args.log):
        parser.error("Use either result_dir or --csv with --log")
    if not args.result_dir and not (args.csv and args.log):
        parser.error("Provide result_dir or both --csv and --log")
    cases = []
    if args.result_dir:
        if not args.result_dir.is_dir():
            parser.error("result_dir is not a directory")
        csv_paths = sorted(args.result_dir.rglob("*.csv"))
        log_paths = sorted(args.result_dir.rglob("*.log"))
        for csv_path in csv_paths:
            cases.append(analyze_case(csv_path, csv_path.with_suffix(".log"), manifest_candidates(csv_path, args.result_dir)))
        known_csv_paths = list(csv_paths)
        for log_path in log_paths:
            if not log_path.with_suffix(".csv").is_file():
                cases.append(analyze_case(log_path.with_suffix(".csv"), log_path, manifest_candidates(log_path, args.result_dir)))
                known_csv_paths.append(log_path.with_suffix(".csv"))
        cases.extend(missing_manifest_recordings(args.result_dir, known_csv_paths))
    else:
        cases.append(analyze_case(args.csv, args.log, manifest_candidates(args.csv)))
    report = {
        "schema_version": 1, "csv_schema": "v0.2", "percentile_method": "linear interpolation at (n-1)*p",
        "validation_scope": "recording consistency; not visual acceptance or LCD refresh FPS",
        "status": "PASS" if cases and all(case["status"] == "PASS" for case in cases) else "FAIL",
        "cases": cases,
    }
    rendered = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
    if args.output:
        try:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered + "\n", encoding="utf-8")
        except OSError as error:
            print("Cannot save JSON report: {}".format(error), file=sys.stderr)
            return 1
    print(report_table(report))
    print("\nJSON report:")
    print(rendered)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
