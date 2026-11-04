#!/usr/bin/env python3
"""Collect macOS build and USB evidence without opening a serial port."""
import argparse
import datetime as dt
import glob
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def command(args):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=30)
        return {"command": args, "exit_code": p.returncode,
                "stdout": p.stdout.strip(), "stderr": p.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"command": args, "exit_code": None, "error": str(exc)}


def usb_devices():
    try:
        p = subprocess.run(["ioreg", "-a", "-r", "-c", "IOUSBHostDevice"],
                           capture_output=True, timeout=30, check=True)
        devices = []

        def walk(nodes):
            for node in nodes:
                if "idVendor" in node and "idProduct" in node:
                    devices.append({"name": node.get("USB Product Name", node.get("IORegistryEntryName")),
                                    "vendor_id": f"0x{node['idVendor']:04x}",
                                    "product_id": f"0x{node['idProduct']:04x}",
                                    "manufacturer": node.get("USB Vendor Name")})
                walk(node.get("IORegistryEntryChildren", []))
        walk(plistlib.loads(p.stdout))
        # Children may also occur as roots in ioreg's recursive output.
        return list({json.dumps(d, sort_keys=True): d for d in devices}.values())
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        detail = getattr(exc, "stderr", b"")
        if isinstance(detail, bytes):
            detail = detail.decode("utf-8", errors="replace")
        return {"error": str(exc), "stderr": detail}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=ROOT / "msu2_cpp_v0_2")
    ap.add_argument("--output", type=Path, help="New output directory; never overwrite an existing run")
    ap.add_argument("--usb-details", action="store_true", help="Opt in to ioreg USB metadata; requires host permission")
    args = ap.parse_args()
    stamp = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).strftime("%Y%m%dT%H%M%S%z")
    parent = ROOT / "results" / "macos"
    parent.mkdir(parents=True, exist_ok=True)
    if args.output:
        out = args.output.resolve()
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix=stamp + "_preflight_", dir=parent))
    report = {"timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
              "platform": platform.platform(), "architecture": platform.machine(),
              "python": platform.python_version(), "serial_ports": sorted(glob.glob("/dev/cu.*")),
              "source": str(args.source.resolve()),
              "source_present": (args.source / "src/main.cpp").is_file(),
              "cmake": shutil.which("cmake"), "serial_port_opened": False}
    report["checks"] = [command(["sw_vers"]), command(["xcode-select", "-p"]),
                        command([os.environ.get("CXX", "c++"), "--version"])]
    report["usb_devices"] = (usb_devices() if args.usb_details and platform.system() == "Darwin"
                             else {"status": "not_requested"})
    src = out / "compiler_probe.cpp"
    src.write_text('#include <iostream>\n#include <filesystem>\nint main(){std::cout << "C++17 ready\\n";}\n')
    binary = out / "compiler_probe"
    build = command([os.environ.get("CXX", "c++"), "-std=c++17", str(src), "-o", str(binary)])
    report["compiler_probe"] = build
    if build["exit_code"] == 0:
        report["binary_type"] = command(["file", str(binary)])
        report["compiler_probe_run"] = command([str(binary)])
    report["build_environment_ready"] = (platform.system() == "Darwin" and build["exit_code"] == 0
                                          and report.get("compiler_probe_run", {}).get("exit_code") == 0)
    (out / "environment.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"Report: {out / 'environment.json'}")
    return 0 if report["build_environment_ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
