#!/usr/bin/env python3
"""One guarded, recorded MSU2 display run from a preset/custom PPM or pattern."""
import argparse
import csv
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid

KIT = Path(__file__).resolve().parent
WORKSPACE = KIT.parent
IMAGES = KIT / "images"
DEFAULT_BINARY = WORKSPACE / "msu2_linux_v0_3" / "build" / "msu2"
PATTERNS = ("bars", "solid", "checker", "gradient", "noise", "moving")
MODES = ("off", "bands4", "center3", "halves2")
KIT_VERSION = "0.2.0"
DEFAULT_SEND_HOLD_MS = 13


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def available_images():
    return {p.stem: p for p in sorted(IMAGES.glob("*.ppm"))}


def resolve_preset_source(source):
    if not re.fullmatch(r"[0-9]{1,2}", source):
        return source
    prefix = f"{int(source):02d}_"
    matches = [name for name in available_images() if name.startswith(prefix)]
    if len(matches) != 1:
        raise ValueError("unknown or ambiguous preset number; use --list to choose an image")
    return matches[0]


def effective_hold_open_ms(args, action):
    if action == "send":
        return DEFAULT_SEND_HOLD_MS if args.hold_open_ms is None else args.hold_open_ms
    if args.hold_open_ms not in (None, 0):
        raise ValueError("--hold-open-ms applies only to --action send")
    return 0


def load_guard():
    # In this workspace use the exact guard and lock root of existing macOS
    # experiments. This prevents two cooperative launchers from owning the MINI.
    shared = WORKSPACE / "scripts" / "macos" / "serial_guard.py"
    if shared.is_file():
        spec = importlib.util.spec_from_file_location("msu2_shared_serial_guard", shared)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.SerialGuard, module.DeviceBusy

    # Standalone kit on Linux/macOS: same cu/tty alias rule and advisory lock.
    import fcntl

    class DeviceBusy(RuntimeError):
        pass

    class SerialGuard:
        def __init__(self, port):
            self.port = port
            self.handle = None
            self.evidence = None

        def __enter__(self):
            path = Path(self.port)
            name = path.name
            if name.startswith(("cu.", "tty.")):
                suffix = name.split(".", 1)[1]
                aliases = [str(path.parent / ("cu." + suffix)),
                           str(path.parent / ("tty." + suffix))]
            else:
                aliases = [str(path)]
            key = hashlib.sha256(aliases[0].encode()).hexdigest()[:20]
            lock_root = KIT / "results" / ".device-locks"
            lock_root.mkdir(parents=True, exist_ok=True)
            self.handle = (lock_root / (key + ".lock")).open("a+")
            try:
                try:
                    fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError as exc:
                    raise DeviceBusy("another test holds the shared serial lock") from exc
                paths = [p for p in aliases if Path(p).exists()]
                if not paths or not path.exists():
                    raise DeviceBusy("serial device is absent: " + self.port)
                lsof = shutil.which("lsof") or ("/usr/sbin/lsof" if Path("/usr/sbin/lsof").is_file() else None)
                if not lsof:
                    raise DeviceBusy("lsof is required to verify that the serial port is idle")
                check = subprocess.run([lsof, "-nP", "--", *paths], capture_output=True,
                                       text=True, timeout=10)
                if check.returncode == 0 and check.stdout.strip():
                    raise DeviceBusy("serial port is already owned:\n" + check.stdout.strip())
                if check.returncode != 1 or check.stdout.strip() or check.stderr.strip():
                    raise DeviceBusy("cannot verify idle serial port: " + check.stderr.strip())
                self.evidence = {"aliases_checked": aliases, "lsof_exit_code": 1,
                                 "checked_at_unix": time.time(), "lock_file": str(lock_root / (key + ".lock"))}
                self.handle.seek(0)
                self.handle.truncate()
                json.dump({"pid": os.getpid(), "port": self.port,
                           "acquired_at_unix": time.time()}, self.handle)
                self.handle.flush()
                return self
            except BaseException:
                self.__exit__(None, None, None)
                raise

        def __exit__(self, *_):
            if self.handle is not None:
                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
                self.handle.close()
                self.handle = None

    return SerialGuard, DeviceBusy


def parse(argv=None):
    parser = argparse.ArgumentParser(description="MSU2 Mini 手動顯示測試；每次只執行一組參數。")
    parser.add_argument("--version", action="version", version="MSU2 manual test kit " + KIT_VERSION)
    parser.add_argument("--list", action="store_true", help="列出預製影像與內建動態圖樣")
    parser.add_argument("--dry-run", action="store_true", help="只檢查參數、列出命令，不開序列埠")
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--port", help="實際裝置埠，如 /dev/cu.usbmodem... 或 /dev/ttyACM0")
    parser.add_argument("--source", default="01_color_bars", help="預製影像編號（1..11）或名稱、PPM 路徑、內建 pattern")
    parser.add_argument("--action", choices=("send", "stream"), help="靜態單張或連續來源；預設依來源決定")
    parser.add_argument("--priority-mode", choices=MODES, default="off")
    parser.add_argument("--fps", type=int, default=45)
    parser.add_argument("--frames", type=int, default=450)
    parser.add_argument("--mode", choices=("raw", "pair"), default="pair")
    parser.add_argument("--update", choices=("full", "diff"), default="diff")
    parser.add_argument("--keyframe-ms", type=int, default=0)
    parser.add_argument("--chunk", type=int, default=60)
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument("--hold-open-ms", type=int, default=None,
                        help="send 寫完 payload 後保持開埠的毫秒數（0..60000；send 預設 13）")
    parser.add_argument("--ack", choices=("prefix", "echo"), default="prefix")
    parser.add_argument("--orientation", type=int, choices=(0, 1))
    parser.add_argument("--label", default="manual")
    parser.add_argument("--results-dir", type=Path, default=KIT / "results")
    args = parser.parse_args(argv)
    if args.list:
        return args
    if not args.port and not args.dry_run:
        parser.error("--port is required unless --dry-run or --list is used")
    if not 1 <= args.fps <= 240:
        parser.error("--fps must be 1..240")
    if not 1 <= args.frames <= 3600:
        parser.error("--frames must be 1..3600")
    if not 6 <= args.chunk <= 65532 or args.chunk % 6:
        parser.error("--chunk must be 6..65532 and divisible by 6")
    if not 1 <= args.timeout_ms <= 60000:
        parser.error("--timeout-ms must be 1..60000")
    if args.hold_open_ms is not None and not 0 <= args.hold_open_ms <= 60000:
        parser.error("--hold-open-ms must be 0..60000")
    if not 0 <= args.keyframe_ms <= 60000:
        parser.error("--keyframe-ms must be 0..60000")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,48}", args.label):
        parser.error("--label must be 1..48 ASCII letters, digits, underscores or hyphens")
    if args.update == "full" and args.keyframe_ms:
        parser.error("--keyframe-ms requires --update diff")
    return args


def source_option(source):
    source = resolve_preset_source(source)
    images = available_images()
    if source in images:
        return "image", ["--input", str(images[source].resolve())], images[source]
    if source in PATTERNS:
        return "pattern", ["--pattern", source], None
    path = Path(source).expanduser()
    if path.is_file() and path.suffix.lower() == ".ppm":
        return "image", ["--input", str(path.resolve())], path.resolve()
    raise ValueError("unknown source; use --list, or supply an existing P6/P3 .ppm path")


def build_command(args, binary, csv_path=None):
    kind, source_args, image = source_option(args.source)
    action = args.action or ("stream" if args.source in ("moving", "noise", "gradient") else "send")
    if action == "send" and args.priority_mode != "off":
        raise ValueError("--priority-mode applies only to --action stream")
    if action == "send" and args.keyframe_ms:
        raise ValueError("--keyframe-ms applies only to --action stream")
    if action == "stream" and csv_path is None:
        raise ValueError("stream needs a CSV output path")
    hold_open_ms = effective_hold_open_ms(args, action)
    cmd = [str(binary), action, "--port", args.port or "<PORT>", *source_args,
           "--mode", args.mode, "--chunk", str(args.chunk),
           "--timeout-ms", str(args.timeout_ms), "--ack", args.ack]
    if args.orientation is not None:
        cmd += ["--orientation", str(args.orientation)]
    if action == "send":
        cmd += ["--hold-open-ms", str(hold_open_ms)]
    if action == "stream":
        cmd += ["--fps", str(args.fps), "--frames", str(args.frames),
                "--update", args.update, "--priority-mode", args.priority_mode,
                "--csv", str(csv_path)]
        if args.update == "diff":
            cmd += ["--keyframe-ms", str(args.keyframe_ms)]
    return action, kind, image, cmd


def summarize_csv(path):
    if not path.is_file():
        return None
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        return {"completed_rows": 0}
    outcomes = {}
    for row in rows:
        outcome = row.get("region_outcome") or row.get("update_kind") or "unknown"
        outcomes[outcome] = outcomes.get(outcome, 0) + 1
    return {"completed_rows": len(rows), "last_frame_id": rows[-1].get("frame_id"),
            "waiting_frames_replaced": rows[-1].get("dropped_total"), "outcomes": outcomes}


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def reap_after_stop(child, grace_seconds):
    """Finish our child while the serial lease is still held.

    SIGINT lets the C++ stream complete its in-flight update and flush CSV.
    Escalation is bounded for a stalled driver; every path waits for the child.
    """
    prior = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        if child.poll() is not None:
            return "already_exited", child.returncode
        child.send_signal(signal.SIGINT)
        try:
            return "sigint", child.wait(timeout=grace_seconds)
        except subprocess.TimeoutExpired:
            child.terminate()
            try:
                return "sigterm", child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                return "sigkill", child.wait()
    finally:
        signal.signal(signal.SIGINT, prior)


def main(argv=None):
    args = parse(argv)
    if args.list:
        print("預製 PPM 影像（160×80）：")
        for name in available_images():
            prefix = name.split("_", 1)[0]
            number = str(int(prefix)) + " / " if re.fullmatch(r"[0-9]{2}", prefix) else ""
            print("  " + number + name)
        print("--source 可用編號或完整名稱，例如 11 或 11_white；亦支援 01..09。")
        print("內建圖樣：" + ", ".join(PATTERNS))
        print("動態更新請用 --source moving 或 noise，並選 --action stream。")
        return 0
    try:
        requested_source = args.source
        args.source = resolve_preset_source(args.source)
        binary = args.binary.expanduser().resolve()
        placeholder_csv = Path("<RESULTS>/stream.csv")
        action, kind, image, preview_cmd = build_command(args, binary, placeholder_csv)
        hold_open_ms = effective_hold_open_ms(args, action)
    except ValueError as exc:
        print("error: " + str(exc), file=sys.stderr)
        return 2
    if args.dry_run:
        print(json.dumps({"kit_version": KIT_VERSION, "action": action,
                          "source": args.source, "source_requested": requested_source,
                          "source_type": kind, "hold_open_ms": hold_open_ms, "command": preview_cmd,
                          "opens_serial_port": False}, ensure_ascii=False, indent=2))
        if action == "stream" and kind == "image" and args.update == "diff":
            print("注意：靜態 PPM 的後續來源相同，diff 可能只送首張；動態測試選 moving/noise。")
        return 0
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print("error: build the executable first: " + str(binary), file=sys.stderr)
        return 2

    stamp = dt.datetime.now().astimezone().strftime("%Y%m%dT%H%M%S%z")
    result_dir = args.results_dir.expanduser().resolve() / (stamp + "_" + args.label + "_" + uuid.uuid4().hex[:8])
    result_dir.mkdir(parents=True, exist_ok=False)
    (result_dir / "OBSERVATION_zh-TW.txt").write_text(
        "請在測試當下或結束後填寫，不要只依主機 exit code 判斷 LCD。\n"
        "螢幕畫面是否完整、方向與顏色是否正確：\n"
        "是否有殘影、缺塊、錯位或卡頓；約在第幾秒出現：\n"
        "重新插拔／前一次測試距今多久（若知道）：\n"
        "其他觀察：\n", encoding="utf-8")
    csv_path = result_dir / "stream.csv"
    action, kind, image, command = build_command(args, binary, csv_path)
    metadata = {
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "kit_version": KIT_VERSION,
        "platform": platform.platform(), "action": action, "source": args.source,
        "source_requested": requested_source,
        "source_type": kind, "image_sha256": digest(image) if image else None,
        "binary": str(binary), "binary_sha256": digest(binary),
        "command": command, "status": "prepared", "exit_code": None,
        "hold_open_ms": hold_open_ms,
        "hold_open_ms_explicit": args.hold_open_ms is not None,
    }
    save_json(result_dir / "metadata.json", metadata)
    (result_dir / "command.txt").write_text(shlex.join(command) + "\n", encoding="utf-8")
    try:
        Guard, Busy = load_guard()
    except Exception as exc:
        metadata["status"] = "launcher_error"
        metadata["error"] = "cannot initialize serial guard: " + str(exc)
        save_json(result_dir / "metadata.json", metadata)
        print("結果：" + str(result_dir))
        print("原因：" + metadata["error"], file=sys.stderr)
        return 1
    run_timeout = max(60.0, args.frames / args.fps * 4 + 60.0) if action == "stream" else 60.0 + hold_open_ms / 1000.0
    stop_grace = max(30.0, min(120.0, args.timeout_ms / 1000.0 * 32 + 5.0))
    started = time.monotonic()
    child = None
    try:
        with Guard(args.port) as lease:
            metadata["serial_guard"] = lease.evidence
            metadata["status"] = "running"
            save_json(result_dir / "metadata.json", metadata)
            with (result_dir / "stdout.log").open("x") as out, (result_dir / "stderr.log").open("x") as err:
                try:
                    child = subprocess.Popen(command, stdout=out, stderr=err)
                    try:
                        metadata["exit_code"] = child.wait(timeout=run_timeout)
                        metadata["status"] = "passed" if child.returncode == 0 else "failed"
                    except subprocess.TimeoutExpired:
                        metadata["status"] = "launcher_timeout"
                        metadata["termination_method"], metadata["exit_code"] = reap_after_stop(child, stop_grace)
                        metadata["error"] = f"child exceeded {run_timeout:.1f} seconds; stop requested and child reaped"
                    except KeyboardInterrupt:
                        metadata["status"] = "interrupted"
                        metadata["termination_method"], metadata["exit_code"] = reap_after_stop(child, stop_grace)
                        metadata["error"] = "user interrupted; stop requested and child reaped before guard release"
                finally:
                    # Keep the lease until a live child is reaped even if an
                    # unexpected launcher error interrupted the normal wait.
                    if child is not None and child.poll() is None:
                        metadata["termination_method"], metadata["exit_code"] = reap_after_stop(child, stop_grace)
                        if metadata["status"] == "running":
                            metadata["status"] = "launcher_error"
                            metadata["error"] = "child stopped after an unexpected launcher error"
    except Busy as exc:
        metadata["status"] = "blocked"
        metadata["error"] = str(exc)
    except subprocess.TimeoutExpired:
        metadata["status"] = "blocked"
        metadata["error"] = "serial owner check timed out before the child was started"
    except KeyboardInterrupt:
        metadata["status"] = "interrupted"
        metadata["error"] = "user interrupted before the child was started"
    except Exception as exc:
        metadata["status"] = "launcher_error"
        metadata["error"] = str(exc)
    metadata["elapsed_seconds"] = round(time.monotonic() - started, 3)
    try:
        metadata["csv_summary"] = summarize_csv(csv_path)
    except (OSError, csv.Error, ValueError) as exc:
        metadata["csv_summary_error"] = str(exc)
    save_json(result_dir / "metadata.json", metadata)
    output_files = [p for p in (result_dir / "command.txt", result_dir / "stdout.log",
                                 result_dir / "stderr.log", csv_path) if p.is_file()]
    (result_dir / "SHA256SUMS.txt").write_text(
        "".join(f"{digest(p)}  {p.name}\n" for p in output_files), encoding="utf-8")
    print("結果：" + str(result_dir))
    print("狀態：" + metadata["status"] + "; exit_code=" + str(metadata["exit_code"]))
    if metadata.get("csv_summary") is not None:
        print("CSV 摘要：" + json.dumps(metadata["csv_summary"], ensure_ascii=False))
    if metadata.get("error"):
        print("原因：" + metadata["error"], file=sys.stderr)
    return 0 if metadata["status"] == "passed" else (2 if metadata["status"] == "blocked" else 1)


if __name__ == "__main__":
    sys.exit(main())
