#!/usr/bin/env python3
"""Offline audit for the manual MSU2 test kit; never accesses serial hardware."""
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
KIT = ROOT / "manual_test_kit"
IMAGES = KIT / "images"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_ppm(data):
    header, payload = data.split(b"\n255\n", 1)
    fields = header.split()
    assert fields[0] == b"P6"
    width, height = map(int, fields[1:3])
    assert len(payload) == width * height * 3
    return width, height, payload


def decode_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, compressed, dimensions = 8, bytearray(), None
    while pos < len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        crc = struct.unpack_from(">I", data, pos + 8 + length)[0]
        assert zlib.crc32(kind + payload) == crc, f"bad PNG CRC: {kind!r}"
        if kind == b"IHDR":
            dimensions = struct.unpack(">IIBBBBB", payload)
            assert dimensions[2:] == (8, 2, 0, 0, 0)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
        pos += 12 + length
    width, height = dimensions[:2]
    raw = zlib.decompress(compressed)
    stride = width * 3
    assert len(raw) == height * (stride + 1)
    pixels = bytearray()
    for y in range(height):
        row = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        assert row[0] == 0, "decoder expects writer's filter-0 rows"
        pixels.extend(row[1:])
    return width, height, bytes(pixels)


def pixel(rgb, x, y):
    at = (y * 160 + x) * 3
    return tuple(rgb[at:at + 3])


def image_integrity(checks):
    entries = {}
    for line in (IMAGES / "SHA256SUMS.txt").read_text().splitlines():
        expected, name = line.split(None, 1)
        entries[name.strip()] = expected
    ppm_paths = sorted(IMAGES.glob("*.ppm"))
    assert len(ppm_paths) == 11, f"expected 11 PPMs, found {len(ppm_paths)}"
    assert len(entries) == 23, f"expected 23 manifest entries, found {len(entries)}"
    for ppm in ppm_paths:
        w, h, payload = parse_ppm(ppm.read_bytes())
        assert (w, h) == (160, 80), (ppm.name, w, h)
        assert len(payload) == 38400
        png = ppm.with_suffix(".png")
        pw, ph, png_pixels = decode_png(png.read_bytes())
        assert (pw, ph, png_pixels) == (w, h, payload), ppm.name
        assert entries[ppm.name] == sha(ppm)
        assert entries[png.name] == sha(png)
        checks.append(f"image {ppm.stem}: P6 160x80, 38400-byte payload, PNG exact, manifest OK")
    sheet = IMAGES / "contact_sheet.png"
    assert entries[sheet.name] == sha(sheet)
    assert decode_png(sheet.read_bytes())[:2] == (660, 256)

    samples = {
        "01_color_bars": {(10, 40): (255, 255, 255), (30, 40): (255, 255, 0),
                          (50, 40): (0, 255, 255), (150, 40): (0, 0, 0)},
        "02_geometry_grid": {(5, 5): (255, 0, 0), (150, 5): (0, 255, 0),
                             (5, 70): (0, 0, 255), (150, 70): (255, 0, 255)},
        "03_bands4": {(5, 5): (180, 24, 24), (5, 25): (180, 140, 0),
                      (5, 45): (0, 130, 180), (5, 65): (100, 0, 180)},
        "04_center3": {(10, 12): (0, 45, 180), (60, 12): (35, 35, 35),
                       (130, 12): (180, 35, 0)},
        "05_halves2": {(10, 10): (0, 110, 170), (100, 10): (175, 80, 0)},
        "06_boundary_targets": {(4, 4): (6, 10, 18), (39, 10): (255, 30, 30),
                                (78, 30): (30, 255, 30)},
        "07_checker1": {(0, 0): (0, 0, 0), (1, 0): (255, 255, 255)},
        "08_gradient": {(0, 0): (0, 0, 0), (159, 79): (255, 255, 104)},
        "10_black": {(80, 40): (0, 0, 0)},
        "11_white": {(80, 40): (255, 255, 255)},
    }
    for stem, points in samples.items():
        _, _, rgb = parse_ppm((IMAGES / (stem + ".ppm")).read_bytes())
        for xy, expected in points.items():
            assert pixel(rgb, *xy) == expected, (stem, xy, pixel(rgb, *xy), expected)
    checks.append(f"pixel samples: {sum(map(len, samples.values()))} coordinate/color assertions OK")

    with tempfile.TemporaryDirectory(prefix="msu2-images-") as td:
        after = {p.name: sha(p) for p in IMAGES.iterdir() if p.is_file()}
        assert all(entries.get(k) == v for k, v in after.items() if k in entries)
        # Run the generator against a fresh directory so the checked-in images stay untouched.
        spec = importlib.util.spec_from_file_location("manual_kit_make_images", KIT / "make_images.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        generated = module.generate(Path(td) / "images")
        fresh = {p.name: sha(p) for p in generated.iterdir() if p.is_file()}
        assert fresh == after
    checks.append("generation reproducibility: fresh directory SHA-256 set matches checked-in images")


def cli_checks(checks):
    def first_json(text):
        return json.JSONDecoder().raw_decode(text.lstrip())[0]
    script = KIT / "manual_test.py"
    version = subprocess.run([sys.executable, str(script), "--version"], cwd=ROOT,
                             capture_output=True, text=True)
    assert version.returncode == 0 and "0.2.0" in version.stdout
    listed = subprocess.run([sys.executable, str(script), "--list"], cwd=ROOT,
                             capture_output=True, text=True)
    assert listed.returncode == 0 and "01_color_bars" in listed.stdout and "moving" in listed.stdout
    image_stems = sorted(p.stem for p in IMAGES.glob("*.ppm"))
    for number, stem in enumerate(image_stems, 1):
        for spelling in (str(number), f"{number:02d}"):
            proc = subprocess.run([sys.executable, str(script), "--dry-run", "--source", spelling,
                                   "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT,
                                  capture_output=True, text=True)
            obj = first_json(proc.stdout)
            assert proc.returncode == 0 and obj["source"] == stem and obj["source_requested"] == spelling, (spelling, proc.stderr)
            assert obj["action"] == "send" and obj["hold_open_ms"] == 13
            assert obj["opens_serial_port"] is False and stem + ".ppm" in " ".join(obj["command"])
    for hold, expected in ((None, 13), ("0", 0), ("10000", 10000)):
        args = [sys.executable, str(script), "--dry-run", "--source", "11", "--port", "/dev/cu.OFFLINE_FAKE"]
        if hold is not None:
            args += ["--hold-open-ms", hold]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        obj = first_json(proc.stdout)
        assert proc.returncode == 0 and obj["action"] == "send" and obj["hold_open_ms"] == expected
        assert "11_white.ppm" in " ".join(obj["command"])
    for source in ("moving", "noise"):
        proc = subprocess.run([sys.executable, str(script), "--dry-run", "--source", source,
                               "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT,
                              capture_output=True, text=True)
        obj = first_json(proc.stdout)
        assert proc.returncode == 0 and obj["action"] == "stream" and obj["hold_open_ms"] == 0
        assert "--hold-open-ms" not in obj["command"]
        explicit = subprocess.run([sys.executable, str(script), "--dry-run", "--source", source,
                                   "--action", "stream", "--hold-open-ms", "10",
                                   "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT,
                                  capture_output=True, text=True)
        assert explicit.returncode == 2 and "only" in explicit.stderr
    explicit_stream = subprocess.run([sys.executable, str(script), "--dry-run", "--source", "moving",
                                      "--action", "stream", "--hold-open-ms", "0",
                                      "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT,
                                     capture_output=True, text=True)
    obj = first_json(explicit_stream.stdout)
    assert explicit_stream.returncode == 0 and "--hold-open-ms" not in obj["command"]
    dry_pattern = subprocess.run([sys.executable, str(script), "--dry-run", "--source", "moving",
                                  "--action", "stream", "--fps", "47", "--frames", "94",
                                  "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT,
                                 capture_output=True, text=True)
    obj = first_json(dry_pattern.stdout)
    assert dry_pattern.returncode == 0 and obj["action"] == "stream"
    assert "47" in obj["command"] and "94" in obj["command"]
    bad_cases = [("--fps", "0"), ("--frames", "3601"), ("--chunk", "7"),
                 ("--timeout-ms", "0"), ("--keyframe-ms", "-1"),
                 ("--label", "bad space"), ("--priority-mode", "center3")]
    for option, value in bad_cases:
        result = subprocess.run([sys.executable, str(script), "--dry-run", "--port", "/dev/cu.OFFLINE_FAKE",
                                 option, value], cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 2, (option, value, result.returncode)
    bad_numbers = []
    for value in ("0", "12", "99", "-1", "1.5"):
        proc = subprocess.run([sys.executable, str(script), "--dry-run", "--port", "/dev/cu.OFFLINE_FAKE",
                               "--source", value], cwd=ROOT, capture_output=True, text=True)
        assert proc.returncode == 2, (value, proc.returncode, proc.stderr)
        bad_numbers.append(value)
    with tempfile.TemporaryDirectory(prefix="msu2-custom-ppm-") as td:
        custom = Path(td) / "custom.ppm"
        custom.write_bytes((IMAGES / "01_color_bars.ppm").read_bytes())
        proc = subprocess.run([sys.executable, str(script), "--dry-run", "--source", str(custom),
                               "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT, capture_output=True, text=True)
        obj = first_json(proc.stdout)
        assert proc.returncode == 0 and obj["source"] == str(custom) and obj["source_type"] == "image"
    dry_named = subprocess.run([sys.executable, str(script), "--dry-run", "--source", "11_white",
                                "--port", "/dev/cu.OFFLINE_FAKE"], cwd=ROOT, capture_output=True, text=True)
    assert dry_named.returncode == 0 and first_json(dry_named.stdout)["source"] == "11_white"
    bad_source = subprocess.run([sys.executable, str(script), "--dry-run", "--port", "/dev/cu.OFFLINE_FAKE",
                                 "--source", "not-a-source"], cwd=ROOT, capture_output=True, text=True)
    assert bad_source.returncode == 2 and "unknown source" in bad_source.stderr
    checks.extend(["CLI --version/--list: kit version and sources available",
                   "CLI presets: all 1..11 and 01..09 resolve canonically; 11 stays static send",
                   "CLI hold: send defaults to 13ms and explicit 0/10000 override; stream omits hold and rejects nonzero",
                   "CLI sources: full name, patterns, custom PPM path preserved; invalid preset numbers rejected",
                   f"CLI rejection: {len(bad_cases) + len(bad_numbers) + 1} invalid value/source cases exit 2 before launcher"])


def launcher_checks(checks, evidence_dir):
    import time

    child_source = '''#!/usr/bin/env python3
import csv, os, signal, sys, time
from pathlib import Path
events = Path(os.environ["MSU2_TEST_EVENTS"])
def event(name):
    with events.open("a", encoding="utf-8") as stream: stream.write(name + "\\n")
def on_sigint(signum, frame):
    event("child_sigint")
    raise SystemExit(0)
signal.signal(signal.SIGINT, on_sigint)
event("child_started")
print("offline child started", flush=True)
print("offline child stderr", file=sys.stderr, flush=True)
try:
    if os.environ.get("MSU2_CHILD_HOLD") == "1":
        while True: time.sleep(0.02)
    if "--csv" in sys.argv:
        csv_path = Path(sys.argv[sys.argv.index("--csv") + 1])
        csv_path.write_text("frame_id,capture_ms,region_outcome,dropped_total\\n1,1,changed,2\\n2,1,unchanged,2\\n")
    event("child_exit")
    raise SystemExit(int(os.environ.get("MSU2_CHILD_EXIT", "0")))
except SystemExit:
    raise
finally:
    if "child_exit" not in events.read_text(encoding="utf-8").splitlines(): event("child_exit")
'''
    guard_source = '''import os, subprocess
def event(name):
    path = os.environ.get("MSU2_TEST_EVENTS")
    if path:
        with open(path, "a", encoding="utf-8") as stream: stream.write(name + "\\n")
class DeviceBusy(RuntimeError): pass
class SerialGuard:
    def __init__(self, port): self.port=port; self.evidence={"mock_guard": True}
    def __enter__(self):
        event("guard_enter")
        mode=os.environ.get("MOCK_GUARD_MODE", "")
        if mode == "busy": raise DeviceBusy("mock guard busy")
        if mode == "owner_timeout": raise subprocess.TimeoutExpired(["mock-owner-check"], 10)
        return self
    def __exit__(self, *args): event("guard_exit"); return False
'''

    cases = []

    def run_case(label, *, guard_mode="normal", child_hold=False, child_exit=0,
                 parent_interrupt=False, short_timeout=False, fallback_lock=False,
                 wait_fault=False, source="02", action="stream", send_hold=None):
        with tempfile.TemporaryDirectory(prefix="msu2-stop-offline-") as td:
            base = Path(td)
            workspace = base / "workspace"
            kit = workspace / "manual_test_kit"
            results = base / "results"
            kit.mkdir(parents=True)
            shutil.copy2(KIT / "manual_test.py", kit / "manual_test.py")
            shutil.copytree(IMAGES, kit / "images")
            if not fallback_lock:
                guard_dir = workspace / "scripts" / "macos"
                guard_dir.mkdir(parents=True)
                (guard_dir / "serial_guard.py").write_text(guard_source)
            source_copy = kit / "manual_test.py"
            instrumented = False
            if short_timeout:
                text = source_copy.read_text()
                target = "run_timeout = max(60.0, args.frames / args.fps * 4 + 60.0) if action == \"stream\" else 60.0 + hold_open_ms / 1000.0"
                assert target in text
                source_copy.write_text(text.replace(target, "run_timeout = 0.35", 1))
                instrumented = True
            if wait_fault:
                text = source_copy.read_text()
                target = "metadata[\"exit_code\"] = child.wait(timeout=run_timeout)"
                assert target in text
                replacement = ('while "child_started" not in Path(os.environ["MSU2_TEST_EVENTS"]).read_text(): time.sleep(0.01)\n'
                               '                        raise RuntimeError("injected wait fault")')
                source_copy.write_text(text.replace(target, replacement, 1))
            fake = base / "offline_fake_binary.py"
            fake.write_text(child_source)
            fake.chmod(0o755)
            events = base / "events.txt"
            events.write_text("")
            run_results = base / "run-results"
            env = os.environ.copy()
            env.update({"MSU2_TEST_EVENTS": str(events), "MOCK_GUARD_MODE": guard_mode,
                        "MSU2_CHILD_HOLD": "1" if child_hold else "0",
                        "MSU2_CHILD_EXIT": str(child_exit), "PYTHONUNBUFFERED": "1"})
            command = [sys.executable, str(source_copy), "--binary", str(fake),
                       "--port", "/dev/cu.OFFLINE_FAKE", "--source", source,
                       "--results-dir", str(run_results), "--label", label]
            if action is not None:
                command += ["--action", action]
            if action == "stream":
                command += ["--frames", "2", "--fps", "2"]
            if send_hold is not None:
                command += ["--hold-open-ms", str(send_hold)]
            held_lock = None
            if fallback_lock:
                import fcntl
                lock_root = kit / "results" / ".device-locks"
                lock_root.mkdir(parents=True)
                lock_key = hashlib.sha256(b"/dev/cu.OFFLINE_FAKE").hexdigest()[:20]
                held_lock = (lock_root / (lock_key + ".lock")).open("a+")
                fcntl.flock(held_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            parent_out = base / "parent.stdout"
            parent_err = base / "parent.stderr"
            with parent_out.open("w") as out, parent_err.open("w") as err:
                proc = subprocess.Popen(command, cwd=workspace, env=env, stdout=out, stderr=err)
                if parent_interrupt:
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        if "child_started" in events.read_text().splitlines(): break
                        if proc.poll() is not None: break
                        time.sleep(0.02)
                    assert "child_started" in events.read_text().splitlines(), events.read_text()
                    proc.send_signal(signal.SIGINT)
                try:
                    return_code = proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise AssertionError("offline launcher parent did not exit promptly")
            if held_lock is not None:
                import fcntl
                fcntl.flock(held_lock.fileno(), fcntl.LOCK_UN)
                held_lock.close()
            event_list = events.read_text().splitlines()
            run_dirs = list(run_results.glob("*")) if run_results.exists() else []
            assert len(run_dirs) == 1, (label, return_code, event_list, parent_err.read_text())
            run_dir = run_dirs[0]
            metadata = json.loads((run_dir / "metadata.json").read_text())
            expected = {"pass": ("passed", 0), "send_default": ("passed", 0),
                        "send_zero": ("passed", 0), "failed": ("failed", 1),
                        "interrupted": ("interrupted", 1), "timeout": ("launcher_timeout", 1),
                        "wait_fault": ("launcher_error", 1),
                        "owner_timeout": ("blocked", 2), "fallback_lock_busy": ("blocked", 2)}[label]
            assert (metadata["status"], return_code) == expected, (label, metadata, return_code)
            if label == "pass":
                assert metadata["kit_version"] == "0.2.0"
                assert metadata["source_requested"] == "02" and metadata["source"] == "02_geometry_grid"
                assert metadata["hold_open_ms"] == 0 and metadata["hold_open_ms_explicit"] is False
            if label in ("send_default", "send_zero"):
                expected_hold = 13 if label == "send_default" else 0
                assert metadata["kit_version"] == "0.2.0"
                assert metadata["source"] == "11_white" and metadata["source_requested"] == "11"
                assert metadata["hold_open_ms"] == expected_hold
                assert metadata["hold_open_ms_explicit"] is (label == "send_zero")
                assert metadata["image_sha256"] == sha(IMAGES / "11_white.ppm")
                assert "--hold-open-ms" in metadata["command"]
                hold_index = metadata["command"].index("--hold-open-ms")
                assert metadata["command"][hold_index + 1] == str(expected_hold)
                assert metadata["csv_summary"] is None
                assert "--csv" not in metadata["command"]
                assert event_list.index("child_exit") < event_list.index("guard_exit")
            if label in ("interrupted", "timeout"):
                assert metadata["termination_method"] == "sigint" and metadata["exit_code"] == 0, metadata
                assert "child_sigint" in event_list and "child_exit" in event_list
                assert event_list.index("child_sigint") < event_list.index("child_exit") < event_list.index("guard_exit"), event_list
            if label == "interrupted":
                assert metadata["error"].startswith("user interrupted")
            if label == "timeout":
                assert instrumented and metadata["error"].startswith("child exceeded 0.3 seconds")
            if label == "wait_fault":
                assert wait_fault and metadata["error"] == "injected wait fault", metadata
                assert metadata["termination_method"] == "sigint" and metadata["exit_code"] == 0
                assert "child_sigint" in event_list and "child_exit" in event_list
                assert event_list.index("child_exit") < event_list.index("guard_exit"), event_list
            if label in ("pass", "send_default", "send_zero", "failed"):
                assert metadata["exit_code"] == (7 if label == "failed" else 0)
                assert event_list.index("child_exit") < event_list.index("guard_exit")
                assert "offline child started" in (run_dir / "stdout.log").read_text()
                assert "offline child stderr" in (run_dir / "stderr.log").read_text()
                if label in ("pass", "failed"):
                    assert metadata["csv_summary"]["completed_rows"] == 2
            if label == "owner_timeout":
                assert metadata["error"] == "serial owner check timed out before the child was started"
                assert "child_started" not in event_list and not (run_dir / "stdout.log").exists()
            if label == "fallback_lock_busy":
                assert metadata["error"] == "another test holds the shared serial lock"
                assert "child_started" not in event_list and not (run_dir / "stdout.log").exists()
            assert (run_dir / "SHA256SUMS.txt").is_file()
            target = evidence_dir / "launcher" / label
            target.parent.mkdir(exist_ok=True)
            shutil.copytree(run_dir, target)
            shutil.copy2(events, target / "events.txt")
            shutil.copy2(parent_out, target / "launcher.stdout")
            shutil.copy2(parent_err, target / "launcher.stderr")
            cases.append({"case": label, "command": command, "launcher_exit_code": return_code,
                          "metadata": metadata, "events": event_list,
                          "instrumented_timeout_threshold": instrumented,
                          "parent_stdout": parent_out.read_text(), "parent_stderr": parent_err.read_text(),
                          "run_dir": str(target), "files": sorted(p.name for p in target.iterdir())})

    run_case("pass")
    run_case("send_default", source="11", action=None)
    run_case("send_zero", source="11", action=None, send_hold=0)
    run_case("failed", child_exit=7)
    run_case("interrupted", child_hold=True, parent_interrupt=True)
    run_case("timeout", child_hold=True, short_timeout=True)
    run_case("wait_fault", child_hold=True, wait_fault=True)
    run_case("owner_timeout", guard_mode="owner_timeout")
    run_case("fallback_lock_busy", fallback_lock=True)
    (evidence_dir / "launcher_stop_cases.json").write_text(
        json.dumps(cases, ensure_ascii=False, indent=2) + "\n")
    checks.append("mock launcher: stream and send-default/send-zero metadata verified; child exited before guard release; pass/nonzero exit, Ctrl+C, timeout, wait exception, owner timeout, and flock-busy lifecycle checks passed")


def binary_checks(checks, evidence_dir):
    binary = ROOT / "msu2_linux_v0_3" / "build" / "msu2"
    if not binary.is_file():
        checks.append("candidate executable: unavailable; preview/encode skipped")
        return
    inputs = [IMAGES / "01_color_bars.ppm", IMAGES / "02_geometry_grid.ppm"]
    records = []
    for source in inputs:
        for action, suffix, flags in (("preview", "preview.ppm", []),
                                      ("encode", "encoded.bin", ["--mode", "pair"])):
            output = evidence_dir / (source.stem + "_" + suffix)
            command = [str(binary), action, "--input", str(source), *flags, "--output", str(output)]
            proc = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
            record = {"command": command, "exit_code": proc.returncode,
                      "stdout": proc.stdout, "stderr": proc.stderr,
                      "input_sha256": sha(source), "output_sha256": sha(output) if output.exists() else None,
                      "binary_sha256": sha(binary)}
            records.append(record)
            assert proc.returncode == 0, record
            assert output.is_file() and output.stat().st_size > 0
            if action == "preview":
                assert parse_ppm(output.read_bytes()) == parse_ppm(source.read_bytes())
    (evidence_dir / "candidate_binary_offline.json").write_text(
        json.dumps(records, ensure_ascii=False, indent=2) + "\n")
    checks.append("candidate preview/encode: 2 prebuilt PPM inputs accepted offline; preview pixel-identical; encode produced files")


def compare_checks(checks, evidence_dir):
    runs_root = evidence_dir / "compare_fixture"
    run_a = runs_root / "a"
    run_b = runs_root / "b"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    for path, fps, status, csv_text, stderr in (
            (run_a, "45", "passed", "frame_id,region_outcome,dropped_total\n1,changed,0\n2,skipped,1\n",
             ""),
            (run_b, "47", "failed", "frame_id,region_outcome,dropped_total\n1,changed,0\n",
             "response timeout")):
        (path / "metadata.json").write_text(json.dumps({
            "source": "moving", "status": status,
            "command": ["fake-msu2", "stream", "--fps", fps, "--mode", "pair",
                        "--update", "diff", "--priority-mode", "bands4", "--chunk", "60"]
        }))
        (path / "stream.csv").write_text(csv_text)
        (path / "stderr.log").write_text(stderr)
    proc = subprocess.run([sys.executable, str(KIT / "compare_runs.py"), str(run_a), str(run_b)],
                          cwd=ROOT, capture_output=True, text=True, check=True)
    rows = list(csv.DictReader(proc.stdout.splitlines()))
    assert len(rows) == 2
    assert rows[0]["fps"] == "45" and rows[0]["completed_rows"] == "2"
    assert rows[0]["replaced_at_last_row"] == "1" and rows[0]["side_skips"] == "1"
    assert rows[0]["timeout_event"] == "none"
    assert rows[1]["timeout_event"] == "response_timeout_unspecified"
    assert rows[1]["status"] == "failed" and rows[1]["fps"] == "47"
    checks.append("compare_runs.py: saved mock runs summarized source/FPS/mode/status/completed rows/replaced/skips/timeout classification")


def main():
    stamp = __import__("datetime").datetime.now().astimezone().strftime("%Y%m%dT%H%M%S%z")
    evidence = KIT / "results" / ("offline_" + stamp)
    evidence.mkdir(parents=True, exist_ok=False)
    checks = []
    image_integrity(checks)
    cli_checks(checks)
    launcher_checks(checks, evidence)
    binary_checks(checks, evidence)
    compare_checks(checks, evidence)
    (evidence / "CHECKS.txt").write_text("\n".join(checks) + "\n", encoding="utf-8")
    artifact_lines = []
    for p in sorted(evidence.rglob("*")):
        if p.is_file() and p.name != "SHA256SUMS.txt":
            artifact_lines.append(f"{sha(p)}  {p.relative_to(evidence)}\n")
    (evidence / "SHA256SUMS.txt").write_text("".join(artifact_lines), encoding="utf-8")
    print(evidence)
    print("\n".join(checks))


if __name__ == "__main__":
    main()
