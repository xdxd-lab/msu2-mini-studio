#!/usr/bin/env python3
"""Compare saved host-side manual runs without claiming LCD frame rate."""
import argparse
import csv
import json
from pathlib import Path
import sys


def option(command, name, fallback="-"):
    try:
        return command[command.index(name) + 1]
    except (ValueError, IndexError):
        return fallback


def collect(path):
    meta = json.loads((path / "metadata.json").read_text(encoding="utf-8"))
    command = meta.get("command", [])
    csv_file = path / "stream.csv"
    rows = []
    if csv_file.is_file():
        with csv_file.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
    stderr_file = path / "stderr.log"
    stderr = stderr_file.read_text(encoding="utf-8", errors="replace") if stderr_file.is_file() else ""
    return {
        "run": path.name,
        "source": meta.get("source", "-"),
        "fps": option(command, "--fps"),
        "encoding": option(command, "--mode"),
        "update": option(command, "--update"),
        "priority": option(command, "--priority-mode", "off"),
        "chunk": option(command, "--chunk"),
        "hold_open_ms": option(command, "--hold-open-ms", str(meta.get("hold_open_ms", 0))),
        "status": meta.get("status", "-"),
        "completed_rows": str(len(rows)) if csv_file.is_file() else "-",
        "replaced_at_last_row": rows[-1].get("dropped_total", "-") if rows else "-",
        "side_skips": str(sum(row.get("region_outcome") == "skipped" for row in rows)) if csv_file.is_file() else "-",
        "timeout_event": ("window_ack_timeout" if "window_ack_timeout" in stderr else
                          "response_timeout_unspecified" if "response timeout" in stderr else "none"),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="比較已保存的主機測試資料；不是 LCD FPS 量測。")
    parser.add_argument("runs", nargs="*", type=Path, help="指定結果資料夾；省略時列出預設 results")
    parser.add_argument("--results-dir", type=Path, default=Path(__file__).resolve().parent / "results")
    args = parser.parse_args(argv)
    paths = args.runs or sorted(p for p in args.results_dir.glob("*") if (p / "metadata.json").is_file())
    if not paths:
        print("沒有可比較的已保存測試。")
        return 0
    records = [collect(path) for path in paths]
    fields = list(records[0])
    writer = csv.DictWriter(sys.stdout, fieldnames=fields)
    writer.writeheader()
    writer.writerows(records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
