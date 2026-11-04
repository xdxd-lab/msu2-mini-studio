#!/usr/bin/env python3
"""Record one bounded LCD stream on the already identified MSU2 Mini port."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import uuid
from serial_guard import SerialGuard

ROOT = Path(__file__).resolve().parents[2]
PORT = "/dev/cu.usbmodem01234567891"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "msu2_cpp_v0_2/build/msu2")
    parser.add_argument("--pattern", choices=("moving", "bars", "solid"), default="moving")
    parser.add_argument("--update", choices=("full", "diff"), default="diff")
    parser.add_argument("--fps", type=int, required=True)
    parser.add_argument("--frames", type=int, required=True)
    parser.add_argument("--chunk", type=int, required=True)
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument("--label", required=True)
    args = parser.parse_args()
    if not 1 <= args.fps <= 60 or not 1 <= args.frames <= 1800:
        parser.error("fps and frames must be bounded (1..60, 1..1800)")
    if not 6 <= args.chunk <= 3846 or args.chunk % 6:
        parser.error("chunk must be 6..3846 and divisible by 6")
    if not 100 <= args.timeout_ms <= 5000:
        parser.error("timeout must be 100..5000 ms")
    if not args.label.replace("_", "").isalnum():
        parser.error("label must be alphanumeric or underscore")
    binary = args.binary.resolve(strict=True)
    stamp = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).strftime("%Y%m%dT%H%M%S_%f%z")
    directory = ROOT / "results/macos" / (stamp + "_diag_" + args.label + "_" + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True)
    csv = directory / "stream.csv"
    command = [str(binary), "stream", "--port", PORT, "--pattern", args.pattern,
               "--mode", "pair", "--update", args.update, "--fps", str(args.fps),
               "--frames", str(args.frames), "--csv", str(csv),
               "--timeout-ms", str(args.timeout_ms), "--chunk", str(args.chunk)]
    if args.update == "diff":
        command.extend(["--keyframe-ms", "0"])
    metadata = {"time": dt.datetime.now(dt.timezone.utc).isoformat(), "command": command,
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "exit_code": None}
    (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    try:
        with SerialGuard(PORT) as lease, (directory / "stream.log").open("x") as log:
            metadata["serial_guard"] = lease.evidence
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.frames / args.fps + 30)
        metadata["exit_code"] = result.returncode
    except subprocess.TimeoutExpired:
        metadata["exit_code"] = "launcher_timeout"
    finally:
        metadata["finished_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
        (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(directory)
    print("exit_code=" + str(metadata["exit_code"]))
    print((directory / "stream.log").read_text()[-1500:])
    return 0 if metadata["exit_code"] == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
