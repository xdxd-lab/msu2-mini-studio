#!/usr/bin/env python3
"""POSIX CLI integration against an independently decoded, fragmented MINI mock.

The mock verifies wire commands and reconstructed pixels, without a legacy EXE
oracle or third-party modules. It is a PTY peer, not evidence of real hardware.
"""
import argparse
from collections import Counter
import csv
import errno
import os
import pathlib
import platform
import pty
import re
import select
import subprocess
import sys
import tempfile
import threading
import time
import tty

HELLO = b"\x00MSN16"
WAKE = b"\x00MSNCN"
START = b"\x02\x03\x07\x00\x00\x00"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def u16(two):
    return (two[0] << 8) | two[1]


class MiniDecoder:
    """Interprets firmware staging RAM and emits complete window-sized frames."""
    def __init__(self):
        self.xy = None
        self.size = None
        self.active = False
        self.ram = bytearray(256)
        self.initialized = set()
        self.page_commands = []
        self.frame = bytearray()
        self.frames = []
        self.frame_bytes = []
        self.rectangles = []
        self.canvas = bytearray(25600)
        self.canvases = []
        self.current_wire_bytes = 0
        self.orientations = []
        self.windows = 0
        self.start_geometries = []

    def command(self, command):
        require(len(command) == 6, "command must be six bytes")
        c = command
        if c[:2] == b"\x02\x00":
            require(not self.active, "a second window interrupted an unfinished frame")
            self.xy = (u16(c[2:4]), u16(c[4:6]))
            self.current_wire_bytes = 6
            return None
        if c[:2] == b"\x02\x01":
            self.size = (u16(c[2:4]), u16(c[4:6]))
            self.current_wire_bytes += 6
            return None
        if c == START:
            require(self.xy is not None and self.size is not None, "start before window dimensions")
            x, y = self.xy
            w, h = self.size
            require(w > 0 and h > 0 and x + w <= 160 and y + h <= 80, "invalid window bounds")
            self.active = True
            self.frame.clear()
            self.initialized.clear()
            self.page_commands.clear()
            self.current_wire_bytes += 6
            self.windows += 1
            self.start_geometries.append((self.windows, self.xy, self.size))
            return "ack"
        if c[:3] == b"\x02\x03\x0a":
            require(c[3] in (0, 1) and c[4:] == b"\x00\x00", "orientation byte format")
            self.orientations.append(c[3])
            return "ack"
        require(self.active, "pixel command outside active window")
        self.current_wire_bytes += 6
        self.page_commands.append(c)
        if c[0] == 4:
            require(c[1] < 64, "RAM index outside 0..63")
            self.ram[c[1] * 4:c[1] * 4 + 4] = c[2:]
            self.initialized.add(c[1])
            return None
        if c[:2] == b"\x02\x04":
            self.ram[:] = c[2:] * 64
            self.initialized = set(range(64))
            return None
        if c[:3] == b"\x02\x03\x08":
            count = u16(c[3:5])
            require(0 < count <= 256 and count % 2 == 0 and c[5] == 0, "invalid page commit")
            require(len(self.initialized) == 64, "page staging RAM was not fully initialized")
            if count < 256:
                require(len(self.page_commands) == 65, "tail must stage 64 groups and commit")
                require(all(cmd[0] == 4 for cmd in self.page_commands[:-1]), "tail must be raw")
                require(self.ram[count:] == b"\xff" * (256 - count), "tail FF padding")
            self.frame.extend(self.ram[:count])
            total = self.size[0] * self.size[1] * 2
            require(len(self.frame) <= total, "commits overflow active window")
            self.initialized.clear()
            self.page_commands.clear()
            if len(self.frame) == total:
                self.frames.append(bytes(self.frame))
                self.frame_bytes.append(self.current_wire_bytes)
                x, y = self.xy
                w, h = self.size
                self.rectangles.append((x, y, w, h))
                for row in range(h):
                    destination = ((y + row) * 160 + x) * 2
                    self.canvas[destination:destination + w * 2] = self.frame[row * w * 2:(row + 1) * w * 2]
                self.canvases.append(bytes(self.canvas))
                self.active = False
                self.xy = self.size = None
            return None
        raise AssertionError("unknown command / forbidden extra tail command: " + c.hex())


class MiniPeer:
    def __init__(self, master, ack="prefix", greeting=True, no_ack=False,
                 corrupt_echo=False, disconnect=False, ack_delay=0.0, fail_window=None,
                 ack_delay_first=0.0, fail_second_wrap_patch=False):
        self.master = master
        self.ack = ack
        self.greeting = greeting
        self.no_ack = no_ack
        self.corrupt_echo = corrupt_echo
        self.disconnect = disconnect
        self.ack_delay = ack_delay
        self.ack_delay_first = ack_delay_first
        self.fail_window = fail_window
        self.fail_second_wrap_patch = fail_second_wrap_patch
        self.wrap_piece_completed = False
        self.decoder = MiniDecoder()
        self.buffer = bytearray()
        self.raw_received = bytearray()
        self.handshakes = 0
        self.error = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def fragmented(self, data):
        # Deliberately split patterns over OS reads, with unrelated noise first.
        for byte in data:
            if self.stop.is_set():
                return
            os.write(self.master, bytes([byte]))
            time.sleep(0.0008)

    def run(self):
        last_hello = 0.0
        try:
            while not self.stop.is_set():
                now = time.monotonic()
                if self.greeting and not self.handshakes and now - last_hello > 0.035:
                    self.fragmented(b"noise\x00MSNx!" + HELLO)
                    last_hello = time.monotonic()
                ready, _, _ = select.select([self.master], [], [], 0.01)
                if not ready:
                    continue
                try:
                    data = os.read(self.master, 65536)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        time.sleep(0.002)
                        continue
                    raise
                if not data:
                    continue
                self.raw_received.extend(data)
                self.buffer.extend(data)
                while len(self.buffer) >= 6:
                    command = bytes(self.buffer[:6])
                    del self.buffer[:6]
                    if not self.handshakes:
                        require(command == WAKE, "host wake differs from exact NUL MSNCN: " + command.hex())
                        self.handshakes += 1
                        if self.corrupt_echo:
                            self.fragmented(b"\x00MSNCX")
                        else:
                            self.fragmented(b"junk\x00MSNXN" + WAKE)
                        continue
                    action = self.decoder.command(command)
                    if action == "ack":
                        if command == START and self.fail_second_wrap_patch:
                            if self.wrap_piece_completed:
                                self.fail_window = self.decoder.windows
                            elif self.decoder.windows >= 8 and self.decoder.xy[0] == 0 and self.decoder.size[0] <= 32:
                                # The left side of a wrap is the first piece in
                                # row-major patch order; reject its next sibling.
                                self.wrap_piece_completed = True
                        if self.disconnect:
                            os.close(self.master)
                            self.master = -1
                            return
                        if not self.no_ack and self.decoder.windows != self.fail_window:
                            delay = self.ack_delay_first if self.decoder.windows == 1 else self.ack_delay
                            if delay:
                                self.stop.wait(delay)
                            reply = command if self.ack == "echo" else b"\x02\x03\x91\x92\x93\x94"
                            self.fragmented(b"\xa5\x02\x01\xff" + reply)
        except Exception as exc:
            if not self.stop.is_set():
                self.error = exc

    def start(self):
        self.thread.start()

    def finish(self):
        self.stop.set()
        self.thread.join(timeout=1.0)
        require(not self.thread.is_alive(), "mock peer failed to stop")
        if self.master >= 0:
            os.close(self.master)
            self.master = -1
        if self.error:
            raise self.error


def run_cli(binary, arguments, *, peer_options=None, success=True, bound=5.0):
    master, slave = pty.openpty()
    tty.setraw(slave)
    port = os.ttyname(slave)
    peer = MiniPeer(master, **(peer_options or {}))
    # Keep the slave open until the CLI exits, allowing the mock to start before open.
    start = time.monotonic()
    peer.start()
    try:
        result = subprocess.run([str(binary)] + arguments + ["--port", port],
                                capture_output=True, text=True, timeout=bound)
        elapsed = time.monotonic() - start
        # Give the decoder time to consume bytes accepted by the OS before exit.
        deadline = time.monotonic() + 0.25
        while time.monotonic() < deadline and peer.buffer:
            time.sleep(0.005)
        time.sleep(0.02)
    finally:
        os.close(slave)
        peer.finish()
    require((result.returncode == 0) == success,
            f"unexpected exit {result.returncode}: {' '.join(arguments)}\n{result.stdout}\n{result.stderr}\n"
            f"mock START geometries: {peer.decoder.start_geometries}")
    require(elapsed < bound, "CLI operation was not bounded")
    require(not peer.buffer, "CLI left a partial six-byte command")
    return result, peer, elapsed


def decode_file(path):
    wire = path.read_bytes()
    require(len(wire) % 6 == 0, "encoded file is not six-byte aligned")
    decoder = MiniDecoder()
    for pos in range(0, len(wire), 6):
        decoder.command(wire[pos:pos + 6])
    require(len(decoder.frames) == 1 and not decoder.active, "encoded file must contain exactly one complete frame")
    return wire, decoder


def make_fixture(path):
    # Exact 160x80 source removes resizing ambiguity. Unique, asymmetric RGB samples
    # catch channel/order/endian errors without relying on a production pattern.
    rgb = bytearray()
    expected = bytearray()
    for y in range(80):
        for x in range(160):
            r, g, b = (x * 19 + y * 7) % 256, (x * 3 + y * 29) % 256, (x * 13 + y * 11) % 256
            rgb.extend((r, g, b))
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            expected.extend((value >> 8, value & 255))
    path.write_bytes(b"P6\n160 80\n255\n" + rgb)
    return bytes(expected)


FIRST_STREAM_FIELDS = ["frame_id", "capture_ms", "convert_ms", "encode_ms", "window_ack_ms",
                       "payload_write_ms", "host_latency_ms", "wire_bytes", "write_calls", "dropped_total"]
DELTA_STREAM_FIELDS = ["changed_pixels", "transmitted_pixels", "rectangles", "update_kind", "unchanged_total"]
PRIORITY_STREAM_FIELDS = [
    "priority_mode", "active_layout", "region", "region_slot", "region_outcome", "mode_event", "trigger",
    "elapsed_ms", "mode_duration_ms", "transitions_total", "predicted_full_ms", "predicted_selected_ms",
    "source_gap", "region_due", "region_sent", "region_unchanged",
    "region_skipped", "region_missed_queue", "region_max_miss_streak", "region_max_stale_ms",
    "region_last_synced_frame_id", "center_due", "center_sent", "center_missed_queue",
    "center_last_synced_frame_id", "left_due", "left_sent", "left_skipped", "left_missed_queue",
    "left_max_streak", "left_last_synced_frame_id", "right_due", "right_sent", "right_skipped",
    "right_missed_queue", "right_max_streak", "right_last_synced_frame_id",
]

PRIORITY_REGIONS = {
    "bands4": [("top0", (0, 0, 160, 20)), ("top1", (0, 20, 160, 20)),
               ("bottom0", (0, 40, 160, 20)), ("bottom1", (0, 60, 160, 20))],
    "center3": [("center", (40, 0, 80, 80)), ("left", (0, 0, 40, 80)),
                ("right", (120, 0, 40, 80))],
    "halves2": [("left", (0, 0, 80, 80)), ("right", (80, 0, 80, 80))],
}


def csv_rows(path):
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames
        rows = list(reader)
    return fields, rows


def pattern_pixels(pattern, tick):
    # Independently produce the specified deterministic source patterns and pack
    # RGB565. No C++ image, planner, encoder or vendor software is used as oracle.
    colors = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
              (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    rng = (0x6d737532 ^ ((tick * 0x9e3779b9) & 0xffffffff)) or 1
    output = bytearray()
    for y in range(80):
        for x in range(160):
            if pattern == "noise":
                channels = []
                for _ in range(3):
                    rng ^= (rng << 13) & 0xffffffff
                    rng ^= rng >> 17
                    rng ^= (rng << 5) & 0xffffffff
                    channels.append(rng & 255)
                r, g, b = channels
            elif pattern == "gradient":
                r, g, b = x * 255 // 159, y * 255 // 79, (x + y + tick) % 256
            elif pattern == "solid":
                r, g, b = 255, 0, 0
            elif pattern == "checker":
                r = g = b = 255 if (x // 8 + y // 8) % 2 else 0
            else:
                r, g, b = colors[x // 20]
                if pattern == "moving" and (x + 160 - tick % 160) % 160 < 12 and 28 <= y < 52:
                    r, g, b = 255 - r, 255 - g, 255 - b
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            output.extend((value >> 8, value & 255))
    return bytes(output)


def changed_pixels(before, current):
    return sum(before[p:p + 2] != current[p:p + 2] for p in range(0, 25600, 2))


def encoded_wire_size(pixels, mode):
    # Firmware pages are 64 four-byte staging groups, followed by a commit.
    size = 18
    for start in range(0, len(pixels), 256):
        page = pixels[start:start + 256]
        if mode == "raw" or len(page) < 256:
            size += 390
        else:
            frequency = Counter(page[p:p + 4] for p in range(0, 256, 4))
            size += 12 + (64 - max(frequency.values())) * 6
    return size


def stream_rows(path, peer, expected_for_id, captured):
    fields, rows = csv_rows(path)
    require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS, "stream CSV must preserve first ten fields and append delta fields")
    require(rows, "stream CSV has no processed-frame rows")
    ids = [int(row["frame_id"]) for row in rows]
    require(ids == sorted(set(ids)) and ids[-1] == captured - 1, "stream must process last capture and never duplicate/reorder source ids")
    cursor, unchanged, previous = 0, 0, None
    for row in rows:
        current = expected_for_id(int(row["frame_id"]))
        count = int(row["rectangles"])
        require(count <= 32, "processed update exceeds window cap")
        frames = peer.decoder.frames[cursor:cursor + count]
        rects = peer.decoder.rectangles[cursor:cursor + count]
        sizes = peer.decoder.frame_bytes[cursor:cursor + count]
        require(len(frames) == count, "CSV claims a window that did not complete")
        require(int(row["wire_bytes"]) == sum(sizes), "CSV wire bytes differ from independently decoded command bytes")
        require(int(row["transmitted_pixels"]) == sum(w * h for _, _, w, h in rects), "CSV transmitted pixels disagree with LCD windows")
        require(int(row["changed_pixels"]) == (12800 if previous is None else changed_pixels(previous, current)),
                "diff must compare against last fully sent RGB565 source, including skipped captures")
        if count == 0:
            unchanged += 1
            require(row["update_kind"] == "unchanged" and current == previous, "zero-window row is not unchanged")
            require(all(float(row[field]) == 0 for field in ("wire_bytes", "write_calls", "window_ack_ms", "payload_write_ms")),
                    "unchanged row must account for zero serial work")
        else:
            require(row["update_kind"] in ("full", "bounding_box", "tiles", "runs"), "unknown update kind")
            cursor += count
            require(peer.decoder.canvases[cursor - 1] == current, "complete patch group differs from independent full source canvas")
            if row["update_kind"] == "full":
                require(rects == [(0, 0, 160, 80)], "full row must contain exactly a full-screen window")
        require(int(row["unchanged_total"]) == unchanged, "CSV unchanged cumulative accounting")
        previous = current
    require(cursor == len(peer.decoder.frames) and not peer.decoder.active, "completed CSV updates must account for all noninterleaved LCD windows")
    require(int(rows[-1]["dropped_total"]) == captured - len(rows), "CSV processed/dropped capture accounting")
    require(rows[0]["update_kind"] == "full", "first stream frame must be full")
    return rows


def summary_value(result, name):
    values = re.findall(r"\b" + re.escape(name) + r"=(\d+)", result.stdout)
    require(values, "stream console missing " + name)
    return int(values[-1])


def stream_checks(binary, directory, fixture, expected):
    # A detailed static fixture exercises unchanged detection beyond a solid fill.
    for label, options, sent in (("default-full", [], 5), ("explicit-full", ["--update", "full"], 5),
                                 ("static-diff", ["--update", "diff", "--keyframe-ms", "0"], 1)):
        path = directory / (label + ".csv")
        result, peer, _ = run_cli(binary, ["stream", "--input", str(fixture), "--frames", "5", "--fps", "20",
                                          "--timeout-ms", "500", "--csv", str(path)] + options)
        rows = stream_rows(path, peer, lambda _: expected, 5)
        require(len(rows) == 5 and len(peer.decoder.frames) == sent, "default/full/static-diff frame counts")
        require(summary_value(result, "captured") == 5 and summary_value(result, "host_frames_sent") == sent,
                "console retains captured and sent frame counts")
        require(summary_value(result, "processed") == 5 and summary_value(result, "unchanged_frames") == 5 - sent,
                "console distinguishes processed/unchanged from host sent FPS")
        require(summary_value(result, "waiting_frames_replaced") == 0, "unpaced static fixture unexpectedly dropped captures")
    print("PASS PTY static diff: detailed image sends once, every unchanged row has zero wire; full default preserved")
    for label, option in (("default", []), ("explicit-off", ["--priority-mode", "off"])):
        path = directory / ("keyframes-" + label + ".csv")
        result, peer, _ = run_cli(binary, ["stream", "--input", str(fixture), "--frames", "12", "--fps", "20",
                                          "--update", "diff", "--keyframe-ms", "140", "--timeout-ms", "500",
                                          "--csv", str(path)] + option)
        fields, rows = csv_rows(path)
        require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS,
                "priority off keyframe path changed the baseline CSV schema")
        require(len(rows) == 12 and 3 <= len(peer.decoder.frames) <= 5,
                "static diff did not produce bounded periodic full keyframes with " + label)
        require(any(row["update_kind"] == "unchanged" for row in rows),
                "periodic keyframes stopped unchanged skipping with " + label)
        require(all(row["update_kind"] in ("full", "unchanged") for row in rows),
                "static source produced partial patches with " + label)
        require(summary_value(result, "full_frames") == len(peer.decoder.frames), "keyframe console full count")
    print("PASS PTY static periodic keyframes and unchanged CSV accounting")
    # At 240 captures/sec a slow ACK replaces queued frames. More than one 160px
    # movement period guarantees edge-wrap patches as well as multiwindow updates.
    path = directory / "moving-diff.csv"
    result, peer, _ = run_cli(binary, ["stream", "--pattern", "moving", "--mode", "raw", "--frames", "220", "--fps", "240",
                                      "--update", "diff", "--keyframe-ms", "0", "--timeout-ms", "750", "--csv", str(path)],
                             peer_options={"ack_delay": 0.035}, bound=8.0)
    rows = stream_rows(path, peer, lambda tick: pattern_pixels("moving", tick), 220)
    require(1 < len(rows) < 220, "slow diff stream did not replace waiting captures")
    require(any(int(row["rectangles"]) > 1 for row in rows), "moving wrap did not exercise multipatch updates")
    require(summary_value(result, "processed") == len(rows), "dynamic processed count")
    require(summary_value(result, "rectangles_sent") == len(peer.decoder.frames), "dynamic completed window count")
    require(summary_value(result, "waiting_frames_replaced") == 220 - len(rows), "dynamic dropped captures count")
    require(bytes(peer.decoder.canvas) == pattern_pixels("moving", 219), "last captured moving frame was not retained on LCD")
    print(f"PASS PTY moving diff: {len(rows)} processed captures, {len(peer.decoder.frames)} windows, dropped captures and wrap reconstruct exactly")


def priority_off_compatibility_checks(binary, directory, fixture, expected):
    # Priority off is the compatibility path. With identical deterministic
    # input and pacing, omitting the option and spelling `off` must produce the
    # same host byte stream, frame rows, and CSV fields.
    captures = []
    for label, option in (("priority-default", []), ("priority-off", ["--priority-mode", "off"])):
        path = directory / (label + ".csv")
        result, peer, _ = run_cli(binary, ["stream", "--input", str(fixture), "--frames", "3", "--fps", "30",
                                          "--timeout-ms", "500", "--csv", str(path)] + option)
        fields, rows = csv_rows(path)
        require(len(rows) == 3 and peer.decoder.frames == [expected] * 3,
                "priority off should retain the baseline full-frame stream")
        captures.append((bytes(peer.raw_received), fields, rows, result.stdout))
    require(captures[0][0] == captures[1][0], "omitted priority mode and explicit off differ on the wire")
    require(captures[0][1] == captures[1][1], "omitted priority mode and explicit off differ in CSV schema")
    stable = ("frame_id", "wire_bytes", "write_calls", "dropped_total", "changed_pixels",
              "transmitted_pixels", "rectangles", "update_kind", "unchanged_total")
    require([[{key: row[key] for key in stable} for row in capture[2]] for capture in captures][0] ==
            [[{key: row[key] for key in stable} for row in capture[2]] for capture in captures][1],
            "omitted priority mode and explicit off differ in stable CSV fields")
    for name in ("captured", "host_frames_sent", "waiting_frames_replaced", "processed", "unchanged_frames",
                 "full_frames", "partial_frames", "rectangles_sent"):
        counts = [re.findall(r"\b" + re.escape(name) + r"=(\d+)", capture[3]) for capture in captures]
        require(counts[0] and counts[0] == counts[1],
                "omitted priority mode and explicit off differ in summary count " + name)
    print("PASS PTY priority off: omitted and explicit off preserve wire bytes, CSV, and baseline stream counts")


def adaptive_keyframe_checks(binary, directory, fixture, expected):
    path = directory / "adaptive-keyframe.csv"
    result, peer, _ = run_cli(
        binary,
        ["stream", "--input", str(fixture), "--frames", "50", "--fps", "20", "--update", "diff",
         "--keyframe-ms", "10", "--priority-mode", "center3", "--timeout-ms", "350", "--csv", str(path)],
        peer_options={"ack_delay_first": 0.080}, bound=7.0)
    fields, rows = csv_rows(path)
    require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS + PRIORITY_STREAM_FIELDS,
            "adaptive maintenance keyframe must preserve full telemetry schema")
    require(rows and rows[0]["update_kind"] == "full" and rows[0]["active_layout"] == "off",
            "adaptive keyframe test must start from a committed baseline full frame")

    epoch = None
    previous = None
    cursor = 0
    keyframes = []
    exits = []
    for row in rows:
        frame_id = int(row["frame_id"])
        if row["mode_event"] == "enter":
            require(epoch is None and row["active_layout"] == "center3",
                    "center3 maintenance test must enter only once")
            epoch = frame_id
        if epoch is not None and row["active_layout"] == "center3":
            require(int(row["region_slot"]) == (frame_id - epoch) % 3,
                    "maintenance test region cadence must remain tied to source IDs")

        count = int(row["rectangles"])
        rects = peer.decoder.rectangles[cursor:cursor + count]
        canvases = peer.decoder.canvases[cursor:cursor + count]
        require(len(rects) == count, "adaptive keyframe telemetry claims incomplete PTY windows")
        require(all(canvas == expected for canvas in canvases),
                "adaptive maintenance keyframe or region update diverged from static source canvas")

        if row["update_kind"] == "priority_keyframe_full":
            keyframes.append(row)
            require(row["active_layout"] == "center3" and row["mode_event"] != "exit" and
                    int(row["transitions_total"]) == 1,
                    "maintenance full must keep adaptive mode active")
            require(row["region_outcome"] == "sent" and count == 1 and rects == [(0, 0, 160, 80)],
                    "adaptive maintenance update must send a complete full-screen window")
            require(epoch is not None and int(row["region_slot"]) == (frame_id - epoch) % 3,
                    "maintenance full must retain the currently scheduled region slot")
            if previous is not None:
                slot = int(row["region_slot"])
                counter_fields = [("center_due", "center_sent"),
                                  ("left_due", "left_sent"), ("right_due", "right_sent")]
                for i, (due_field, sent_field) in enumerate(counter_fields):
                    missed_due = 0
                    if previous["active_layout"] == "center3":
                        for missed_id in range(int(previous["frame_id"]) + 1, frame_id):
                            missed_due += 1 if (missed_id - epoch) % 3 == i else 0
                    expected_due_delta = missed_due + (1 if i == slot else 0)
                    require(int(row[due_field]) - int(previous[due_field]) == expected_due_delta,
                            "maintenance full must increment due only for its scheduled slot")
                    expected_sent_delta = 1 if i == slot else 0
                    require(int(row[sent_field]) - int(previous[sent_field]) == expected_sent_delta,
                            "maintenance full must increment sent only for its scheduled slot")
            require(all(int(row[field]) == frame_id for field in
                        ("center_last_synced_frame_id", "left_last_synced_frame_id", "right_last_synced_frame_id")),
                    "successful maintenance full must synchronize every region to its source frame")
        if row["mode_event"] == "exit":
            exits.append(row)
        cursor += count
        previous = row

    require(epoch is not None and keyframes,
            "test duration did not produce a periodic full while adaptive scheduling was active")
    require(exits and float(exits[-1]["elapsed_ms"]) >= 1000 and
            int(exits[-1]["transitions_total"]) == 2,
            "scheduler should exit normally after maintenance once safe hysteresis and dwell complete; exits="+
            repr([(row["frame_id"], row["elapsed_ms"], row["mode_duration_ms"], row["trigger"])
                  for row in exits])+" tail="+
            repr([(row["frame_id"], row["active_layout"], row["transitions_total"],
                   row["predicted_full_ms"], row["region_outcome"], row["dropped_total"],
                   row["capture_ms"], row["window_ack_ms"], row["mode_duration_ms"])
                  for row in rows[-5:]]))
    require(cursor == len(peer.decoder.frames) and not peer.decoder.active,
            "adaptive keyframe rows must account for all complete PTY windows")
    require(bytes(peer.decoder.canvas) == expected,
            "retained PTY canvas must remain equal to source after maintenance and later exit")
    print(f"PASS PTY adaptive maintenance keyframe: {len(keyframes)} full refreshes while active; normal safe exit observed")

    fail_path = directory / "adaptive-keyframe-failed.csv"
    failed, failed_peer, _ = run_cli(
        binary,
        ["stream", "--input", str(fixture), "--frames", "30", "--fps", "20", "--update", "diff",
         "--keyframe-ms", "10", "--priority-mode", "center3", "--timeout-ms", "120", "--csv", str(fail_path)],
        peer_options={"ack_delay_first": 0.080, "fail_window": 3}, success=False, bound=3.0)
    error = failed.stderr
    require("adaptive_failure frame_id=" in error and "class=window_ack_timeout" in error and
            "phase=send" in error,
            "failed adaptive keyframe must report its ACK-timeout class and send phase")
    fields, fail_rows = csv_rows(fail_path)
    require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS + PRIORITY_STREAM_FIELDS,
            "failed adaptive keyframe should keep the complete CSV schema")
    require(fail_rows and any(row["mode_event"] == "enter" and row["active_layout"] == "center3" for row in fail_rows),
            "failure injection must happen after adaptive mode entry")
    failure_match = re.search(r"adaptive_failure frame_id=(\d+)", error)
    require(failure_match is not None and not any(int(row["frame_id"]) == int(failure_match.group(1))
                                                  for row in fail_rows),
            "failed maintenance source frame must not produce a success CSV row")
    require(any(row["update_kind"] == "priority_keyframe_full" for row in fail_rows),
            "failure injection must follow one committed adaptive maintenance keyframe")
    require(failed_peer.decoder.windows == 3 and failed_peer.decoder.frames == [expected, expected] and
            bytes(failed_peer.decoder.canvas) == expected,
            "failed maintenance keyframe must not change the last committed LCD canvas")
    print("PASS PTY adaptive keyframe failure: ACK timeout reported, no successful maintenance row or canvas commit")


def center_probe_checks(binary, directory):
    path = directory / "center3-bounded-probe.csv"
    captured = 29  # at 20 FPS this exceeds the 1s probe age; moving stripe stays left of center through ID 28
    _, peer, _ = run_cli(
        binary,
        ["stream", "--pattern", "moving", "--frames", str(captured), "--fps", "20", "--priority-mode",
         "center3", "--timeout-ms", "300", "--csv", str(path)],
        peer_options={"ack_delay_first": 0.120}, bound=5.0)
    fields, rows = csv_rows(path)
    require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS + PRIORITY_STREAM_FIELDS,
            "center probe must retain full adaptive telemetry schema")
    require(rows and rows[0]["update_kind"] == "full" and rows[0]["active_layout"] == "off",
            "center probe must begin from a full baseline canvas")
    canvas = bytearray(pattern_pixels("moving", int(rows[0]["frame_id"])))
    cursor = int(rows[0]["rectangles"])
    require(cursor == 1 and peer.decoder.rectangles[0] == (0, 0, 160, 80),
            "center probe must initialize its reference with one full-screen window")
    require(peer.decoder.canvases[0] == bytes(canvas), "center probe baseline differs from independent source pattern")
    epoch = None
    saw_center_unchanged = False
    saw_left_skip = False
    probe_rows = []
    for row in rows[1:]:
        frame_id = int(row["frame_id"])
        current = pattern_pixels("moving", frame_id)
        if row["mode_event"] == "enter":
            epoch = frame_id
        if row["active_layout"] == "center3":
            require(epoch is not None and int(row["region_slot"]) == (frame_id - epoch) % 3,
                    "center3 probe cadence must follow source frame IDs")
            name = row["region"]
            rect = PRIORITY_REGIONS["center3"][int(row["region_slot"])][1]
            if name == "center":
                saw_center_unchanged = True
                require(row["region_outcome"] == "unchanged" and int(row["rectangles"]) == 0 and
                        int(row["changed_pixels"]) == 0,
                        "the central region must remain unchanged while the moving stripe stays on the left")
            if name == "left" and row["region_outcome"] == "skipped":
                saw_left_skip = True
                require(float(row["predicted_selected_ms"]) >= 0.80 * 50.0,
                        "the left side skip must be due to its slow host-cost prediction")
            if name == "left" and row["region_outcome"] == "sent" and float(row["elapsed_ms"]) >= 1000.0:
                probe_rows.append(row)
            count = int(row["rectangles"])
            rects = peer.decoder.rectangles[cursor:cursor + count]
            require(len(rects) == count, "center probe CSV claims incomplete windows")
            if row["region_outcome"] == "sent":
                require(count == 1 and rects == [rect], "sent center3 region must match selected side bounds")
                x, y, width, height = rect
                for y0 in range(y, y + height):
                    start = (y0 * 160 + x) * 2
                    canvas[start:start + width * 2] = current[start:start + width * 2]
            else:
                require(count == 0 and row["region_outcome"] in ("unchanged", "skipped"),
                        "unsent center3 opportunity must have no completed PTY window")
            if count:
                require(peer.decoder.canvases[cursor] == bytes(canvas),
                        "bounded side probe failed to reconstruct the retained LCD canvas")
                cursor += count
            else:
                observed = peer.decoder.canvases[cursor - 1] if cursor else b""
                require(observed == bytes(canvas),
                        "unchanged/skipped side opportunity altered the retained LCD canvas at source " +
                        str(frame_id) + " region=" + name)
        else:
            count = int(row["rectangles"])
            rects = peer.decoder.rectangles[cursor:cursor + count]
            current = pattern_pixels("moving", int(row["frame_id"]))
            require(count == 1 and rects == [(0, 0, 160, 80)],
                    "post-exit full-mode update should retain its ordinary full-frame behavior")
            canvas[:] = current
            require(peer.decoder.canvases[cursor] == bytes(canvas),
                    "post-exit full update did not reconstruct the latest source canvas")
            cursor += count
    require(saw_center_unchanged and saw_left_skip,
            "scenario must include an unchanged center and a predicted-risk side release")
    require(probe_rows,
            "a changing left side must receive a bounded probe after its sync age exceeds one second")
    require(int(probe_rows[0]["left_sent"]) > 0 and int(probe_rows[0]["left_last_synced_frame_id"]) ==
            int(probe_rows[0]["frame_id"]),
            "left-side probe must be reflected in sent and last-synced telemetry")
    require(cursor == len(peer.decoder.frames) and bytes(peer.decoder.canvas) == bytes(canvas),
            "center probe traffic must account for all PTY windows and preserve the expected LCD canvas")
    print(f"PASS PTY center3 bounded probe: {len(probe_rows)} stale left-side probe(s), center unchanged, canvas reconstructed")


def region_changes(before, current, rect):
    x, y, width, height = rect
    count = 0
    for row in range(y, y + height):
        start = (row * 160 + x) * 2
        end = start + width * 2
        count += changed_pixels(before[start:end], current[start:end])
    return count


def priority_stream_checks(binary, directory):
    for mode, regions in PRIORITY_REGIONS.items():
        path = directory / ("priority-" + mode + ".csv")
        result, peer, _ = run_cli(
            binary,
            ["stream", "--pattern", "noise", "--frames", "12", "--fps", "30", "--mode", "pair",
             "--priority-mode", mode, "--timeout-ms", "350", "--csv", str(path)],
            peer_options={"ack_delay_first": 0.075}, bound=5.0)
        fields, rows = csv_rows(path)
        require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS + PRIORITY_STREAM_FIELDS,
                mode + " CSV must append the complete priority telemetry schema")
        require(rows and rows[0]["active_layout"] == "off" and rows[0]["region"] == "full",
                mode + " must begin with the baseline full update")
        require(int(rows[0]["frame_id"]) == 0 and int(rows[0]["rectangles"]) == 1 and
                peer.decoder.rectangles[0] == (0, 0, 160, 80),
                mode + " first frame must fully establish the retained canvas")
        require(rows[0]["region_outcome"] == "sent" and int(rows[0]["transitions_total"]) == 0,
                mode + " first full update is a normal send before scheduler entry")
        require(all(int(rows[0][key]) == 0 for key in
                    ("center_last_synced_frame_id", "left_last_synced_frame_id", "right_last_synced_frame_id")),
                mode + " full baseline should mark every region synced to source frame zero")

        canvas = bytearray(pattern_pixels("noise", 0))
        cursor = 0
        epoch = None
        last_id = 0
        seen_regions = set()
        due = [0] * len(regions)
        sent = [0] * len(regions)
        unchanged = [0] * len(regions)
        skipped = [0] * len(regions)
        missed = [0] * len(regions)
        streak = [0] * len(regions)
        max_streak = [0] * len(regions)
        last_synced = [0] * len(regions)
        pre_entry_gap = 0

        for row in rows:
            frame_id = int(row["frame_id"])
            current = pattern_pixels("noise", frame_id)
            expected_gap = max(0, frame_id - last_id - 1)
            require(int(row["source_gap"]) == expected_gap, mode + " source-gap telemetry differs from source IDs")
            count = int(row["rectangles"])
            group_frames = peer.decoder.frames[cursor:cursor + count]
            group_rects = peer.decoder.rectangles[cursor:cursor + count]
            group_sizes = peer.decoder.frame_bytes[cursor:cursor + count]
            require(len(group_frames) == count, mode + " CSV rectangle count lacks completed wire windows")
            require(int(row["wire_bytes"]) == sum(group_sizes), mode + " wire-byte telemetry differs from decoded commands")
            require(int(row["transmitted_pixels"]) == sum(w * h for _, _, w, h in group_rects),
                    mode + " transmitted-pixel telemetry differs from decoded windows")
            require(int(row["changed_pixels"]) == (12800 if frame_id == 0 else
                    (changed_pixels(canvas, current) if row["active_layout"] == "off" else
                     region_changes(canvas, current, regions[int(row["region_slot"])][1]))),
                    mode + " changed-pixel count must compare against the committed LCD canvas")
            require(float(row["elapsed_ms"]) >= 0 and float(row["mode_duration_ms"]) >= 0,
                    mode + " elapsed/dwell telemetry must be nonnegative")
            require(row["priority_mode"] == mode, mode + " CSV selected mode value")

            if row["mode_event"] == "enter":
                require(epoch is None and row["active_layout"] == mode and int(row["region_slot"]) == 0,
                        mode + " entry must begin at phase zero")
                epoch = frame_id
                pre_entry_gap = int(row["source_gap"])
            if row["active_layout"] == "off":
                require(row["region_outcome"] == "sent" and count == 1 and group_rects == [(0, 0, 160, 80)],
                        mode + " off slot must send the full canvas")
                canvas[:] = current
            else:
                require(epoch is not None and row["active_layout"] == mode,
                        mode + " active layout must follow a recorded entry")
                expected_index = (frame_id - epoch) % len(regions)
                index = int(row["region_slot"])
                name, rect = regions[expected_index]
                require(index == expected_index and row["region"] == name,
                        mode + " region order must follow source frame IDs")
                seen_regions.add(name)

                if row["mode_event"] != "enter" and frame_id > last_id + 1:
                    for missed_id in range(last_id + 1, frame_id):
                        missed_index = (missed_id - epoch) % len(regions)
                        due[missed_index] += 1
                        missed[missed_index] += 1
                        streak[missed_index] += 1
                        max_streak[missed_index] = max(max_streak[missed_index], streak[missed_index])
                due[index] += 1
                outcome = row["region_outcome"]
                if outcome == "sent":
                    sent[index] += 1
                    streak[index] = 0
                    x, y, width, height = rect
                    require(count == 1 and group_rects == [rect],
                            mode + " sent region window must match its declared partition bounds")
                    for y0 in range(y, y + height):
                        start = (y0 * 160 + x) * 2
                        canvas[start:start + width * 2] = current[start:start + width * 2]
                elif outcome == "unchanged":
                    unchanged[index] += 1
                    streak[index] = 0
                    require(count == 0 and row["update_kind"] == "priority_unchanged",
                            mode + " unchanged region must not send a window")
                elif outcome == "skipped":
                    skipped[index] += 1
                    streak[index] += 1
                    max_streak[index] = max(max_streak[index], streak[index])
                    require(mode == "center3" and index != 0 and count == 0 and
                            row["update_kind"] == "priority_skipped",
                            "only center3 side regions may be intentionally released")
                else:
                    raise AssertionError(mode + " unexpected region outcome " + outcome)

                require(int(row["region_due"]) == due[index] and int(row["region_sent"]) == sent[index] and
                        int(row["region_unchanged"]) == unchanged[index] and
                        int(row["region_skipped"]) == skipped[index] and
                        int(row["region_missed_queue"]) == missed[index] and
                        int(row["region_max_miss_streak"]) == max_streak[index],
                        mode + " selected-region telemetry counters differ from source-slot model")
                if outcome in ("sent", "unchanged"):
                    last_synced[index] = frame_id
                require(int(row["region_last_synced_frame_id"]) == last_synced[index],
                        mode + " region sync age must advance only after a completed/unchanged slot")
                if mode == "center3":
                    fields_for_regions = [
                        ("center_due", "center_sent", "center_missed_queue", 0),
                        ("left_due", "left_sent", "left_skipped", "left_missed_queue", 1),
                        ("right_due", "right_sent", "right_skipped", "right_missed_queue", 2),
                    ]
                    for item in fields_for_regions:
                        region_index = item[-1]
                        names = item[:-1]
                        values = {"center_due": due[region_index], "center_sent": sent[region_index],
                                  "center_missed_queue": missed[region_index],
                                  "left_due": due[region_index], "left_sent": sent[region_index],
                                  "left_skipped": skipped[region_index], "left_missed_queue": missed[region_index],
                                  "right_due": due[region_index], "right_sent": sent[region_index],
                                  "right_skipped": skipped[region_index], "right_missed_queue": missed[region_index]}
                        require(all(int(row[field]) == values[field] for field in names),
                                "center3 side telemetry counters differ from source-slot model")
                    sync_fields = ("center_last_synced_frame_id", "left_last_synced_frame_id",
                                   "right_last_synced_frame_id")
                    require(all(int(row[field]) == last_synced[i] for i, field in enumerate(sync_fields)),
                            "center3 per-region last-synced frame IDs differ from the canvas model")

            cursor += count
            observed_canvas = peer.decoder.canvases[cursor - 1] if cursor else b""
            same_canvas = observed_canvas == bytes(canvas)
            mismatch = next((i for i, (a, b) in enumerate(zip(observed_canvas, canvas)) if a != b), -1)
            require(same_canvas,
                    mode + " decoded PTY LCD canvas differs from independently retained canvas at source " +
                    str(frame_id) + " outcome=" + row["region_outcome"] + " window_cursor=" + str(cursor) +
                    " actual_byte=" + str(observed_canvas[mismatch] if mismatch >= 0 else None) +
                    " expected_byte=" + str(canvas[mismatch] if mismatch >= 0 else None) +
                    " actual_size=" + str(len(observed_canvas)) + " expected_size=" + str(len(canvas)))
            last_id = frame_id

        require(epoch is not None and any(row["mode_event"] == "enter" for row in rows),
                mode + " adaptive stream never entered its requested layout")
        require(seen_regions == {name for name, _ in regions}, mode + " did not exercise every scheduled region")
        require(cursor == len(peer.decoder.frames), mode + " CSV rows do not account for every completed window")
        require(int(rows[-1]["dropped_total"]) == 12 - len(rows) and int(rows[-1]["frame_id"]) == 11,
                mode + " source queue drops must be distinct from scheduler outcomes")
        require(int(rows[-1]["dropped_total"]) == pre_entry_gap + sum(missed),
                mode + " queue drops should split into pre-entry source gaps and scheduled region misses")
        require(int(rows[-1]["transitions_total"]) == 1, mode + " should remain in one entered mode during the short run")
        require(int(rows[-1]["left_last_synced_frame_id"]) >= 0 and
                int(rows[-1]["center_last_synced_frame_id"]) >= 0 and
                int(rows[-1]["right_last_synced_frame_id"]) >= 0,
                mode + " successful initial full frame must initialize all region ages")
        if mode == "center3":
            require(any(row["region_outcome"] == "skipped" for row in rows),
                    "center3 stress must report side release distinctly from source queue drops")
            require(all(row["region_outcome"] != "skipped" for row in rows if row["region"] == "center"),
                    "center3 must preserve every central cadence slot")
        print(f"PASS PTY priority {mode}: {len(rows)} rows, {cursor} windows, retained LCD canvas and telemetry verified")


def multipart_failure_checks(binary, directory):
    # Reject a window during a multipart update. CSV can contain only completed
    # updates, and no subsequent update can interleave with the failed transaction.
    path = directory / "failed-diff.csv"
    # Use enough source time for the moving patch to cross the left edge even
    # when ACK latency replaces queued captures; 240 FPS ended before the wrap
    # source positions were necessarily processed.
    _, peer, _ = run_cli(binary, ["stream", "--pattern", "moving", "--mode", "raw", "--frames", "220", "--fps", "60",
                                  "--update", "diff", "--keyframe-ms", "0", "--timeout-ms", "150", "--csv", str(path)],
                         peer_options={"ack_delay": 0.035, "fail_second_wrap_patch": True}, success=False, bound=6.0)
    _, rows = csv_rows(path)
    require(peer.wrap_piece_completed and peer.decoder.windows == len(peer.decoder.frames) + 1,
            "failed multipart window must stop stream before sending its payload or any later window")
    require(sum(int(row["rectangles"]) for row in rows) == len(peer.decoder.frames) - 1,
            "CSV must omit the whole incomplete update even after its first patch completed")
    print("PASS PTY second multipart window fails: first sibling completed, incomplete update omitted from CSV, no later interleave")


def priority_failure_check(binary, directory):
    path = directory / "priority-failed-region.csv"
    failed, peer, _ = run_cli(
        binary,
        ["stream", "--pattern", "noise", "--frames", "12", "--fps", "30", "--priority-mode", "halves2",
         "--timeout-ms", "250", "--csv", str(path)],
        peer_options={"ack_delay_first": 0.075, "fail_window": 2}, success=False, bound=3.0)
    require("adaptive_failure frame_id=" in failed.stderr and "class=window_ack_timeout" in failed.stderr and
            "phase=send" in failed.stderr,
            "adaptive region ACK failure should identify frame, timeout class, and send phase")
    fields, rows = csv_rows(path)
    require(fields == FIRST_STREAM_FIELDS + DELTA_STREAM_FIELDS + PRIORITY_STREAM_FIELDS,
            "failed priority run should retain complete adaptive CSV schema")
    require(len(rows) == 1 and rows[0]["active_layout"] == "off" and rows[0]["region_outcome"] == "sent",
            "failed partition slot must not be recorded as a completed processed row")
    require("completed_wire_bytes=0" in failed.stderr,
            "incomplete region failure must not report acknowledged wire bytes as completed")
    require(peer.decoder.windows == 2 and peer.decoder.frames == [pattern_pixels("noise", 0)],
            "failed priority window must not append pixels or complete a second canvas update")
    require(bytes(peer.decoder.canvas) == pattern_pixels("noise", 0),
            "failed region must leave the independently decoded LCD canvas at its last committed full frame")
    print("PASS PTY priority failure: incomplete region has no CSV commit and cannot alter retained LCD canvas")


def csv_and_argument_checks(binary, directory, fixture):
    sentinel = directory / "existing.csv"
    sentinel.write_bytes(b"PRESERVE EXISTING CSV\n")
    for command, extra in (("bench", ["--frames", "10"]), ("diff-bench", ["--frames", "1"]),
                           ("stream", ["--input", str(fixture), "--frames", "1"])):
        arguments = [command, "--csv", str(sentinel)] + extra
        if command == "stream":
            result, peer, _ = run_cli(binary, arguments, success=False)
            require(peer.handshakes == 0, "existing stream CSV should be rejected before device handshake")
        else:
            result = subprocess.run([str(binary)] + arguments, capture_output=True, text=True, timeout=3)
            require(result.returncode != 0, "existing benchmark CSV accepted")
        require("exist" in result.stderr.lower() and sentinel.read_bytes() == b"PRESERVE EXISTING CSV\n", "CSV refusal must preserve existing bytes")
    path = directory / "failed-handshake.csv"
    _, peer, _ = run_cli(binary, ["stream", "--input", str(fixture), "--frames", "1", "--update", "diff",
                                  "--csv", str(path), "--timeout-ms", "100"],
                         peer_options={"greeting": False}, success=False, bound=2.0)
    require(not path.exists() and peer.handshakes == 0, "stream CSV must not open before successful handshake")
    bad = [
        ["stream", "--update", "bogus"],
        ["stream", "--update", "full", "--keyframe-ms", "0"],
        ["stream", "--keyframe-ms", "10"],
        ["stream", "--update", "diff", "--keyframe-ms", "60001"],
        ["stream", "--update", "diff", "--keyframe-ms", "-1"],
        ["stream", "--priority-mode", "invalid"],
        ["encode", "--priority-mode", "bands4"],
        ["diff-bench", "--frames", "0"],
        ["diff-bench", "--frames", "100001"],
        ["diff-bench", "--frames", "-1"],
    ]
    for arguments in bad:
        result = subprocess.run([str(binary)] + arguments + (["--port", "/does/not/exist"] if arguments[0] == "stream" else []),
                                capture_output=True, text=True, timeout=3)
        require(result.returncode != 0, "invalid delta CLI option accepted")
        require("serial" not in result.stderr.lower() and "/does/not/exist" not in result.stderr, "delta CLI option was not validated before opening serial")
    _, peer, _ = run_cli(binary, ["stream", "--input", str(fixture), "--frames", "1", "--update", "diff",
                                  "--keyframe-ms", "60000", "--timeout-ms", "500"])
    require(len(peer.decoder.frames) == 1, "maximum valid keyframe interval rejected")
    print("PASS CSV existing-path preservation, post-handshake creation, update/keyframe/benchmark argument ranges")


def diff_bench_checks(binary, directory):
    outputs = []
    frames = 9
    for name in ("diff-bench-a.csv", "diff-bench-b.csv"):
        path = directory / name
        result = subprocess.run([str(binary), "diff-bench", "--frames", str(frames), "--csv", str(path)],
                                capture_output=True, text=True, timeout=8)
        require(result.returncode == 0, "diff benchmark failed: " + result.stderr)
        fields, rows = csv_rows(path)
        require(len(rows) == 12, "diff benchmark must cover six patterns times raw/pair")
        require("display" not in fields and "fps" not in ",".join(fields), "diff benchmark must report byte savings, not display FPS")
        outputs.append(rows)
    numeric = ["pattern", "mode", "frames", "full_wire_bytes", "diff_wire_bytes", "saved_percent",
               "changed_pixels", "transmitted_pixels", "rectangles", "unchanged_frames"]
    require([[row[k] for k in numeric] for row in outputs[0]] == [[row[k] for k in numeric] for row in outputs[1]],
            "diff benchmark deterministic sequence totals changed across runs")
    expected_keys = {(p, m) for p in ("solid", "bars", "checker", "gradient", "noise", "moving") for m in ("raw", "pair")}
    require({(row["pattern"], row["mode"]) for row in outputs[0]} == expected_keys, "diff benchmark pattern/mode coverage")
    for row in outputs[0]:
        sources = [pattern_pixels(row["pattern"], tick) for tick in range(frames)]
        full = sum(encoded_wire_size(source, row["mode"]) for source in sources)
        changed = 12800 + sum(changed_pixels(a, b) for a, b in zip(sources, sources[1:]))
        require(int(row["frames"]) == frames and int(row["full_wire_bytes"]) == full, "benchmark full baseline differs from actual page encoding")
        require(int(row["changed_pixels"]) == changed, "benchmark changed pixels differ from RGB565 deterministic sequence")
        diff = int(row["diff_wire_bytes"])
        require(0 < diff <= full and abs(float(row["saved_percent"]) - 100 * (1 - diff / full)) < 0.00011,
                "benchmark savings must be computed from encoded command bytes")
        if row["pattern"] in ("solid", "bars", "checker"):
            require(diff == encoded_wire_size(sources[0], row["mode"]) and int(row["unchanged_frames"]) == frames - 1,
                    "offline static benchmark must send only the first full frame with no periodic refresh")
            require(int(row["rectangles"]) == 1 and int(row["transmitted_pixels"]) == 12800, "offline static first-full accounting")
    path = directory / "one-frame.csv"
    result = subprocess.run([str(binary), "diff-bench", "--frames", "1", "--csv", str(path)], capture_output=True, text=True, timeout=4)
    require(result.returncode == 0, "diff benchmark minimum one frame rejected")
    _, rows = csv_rows(path)
    require(all(row["full_wire_bytes"] == row["diff_wire_bytes"] and int(row["rectangles"]) == 1
                and int(row["changed_pixels"]) == 12800 for row in rows), "one-frame benchmark must start every pattern/mode with one full frame")
    print("PASS offline diff-bench: 12 deterministic sequences, RGB565 change counts and independent actual encoded-byte savings")


def offline_checks(binary, directory, fixture, expected):
    for mode in ("raw", "pair"):
        output = directory / (mode + ".bin")
        result = subprocess.run([str(binary), "encode", "--input", str(fixture), "--mode", mode,
                                 "--output", str(output)], capture_output=True, text=True, timeout=4)
        require(result.returncode == 0, "offline encode failed: " + result.stderr)
        wire, decoder = decode_file(output)
        require(decoder.frames[0] == expected, "offline CLI RGB565 pixels differ from independent fixture")
        if mode == "raw":
            require(len(wire) == 39018, "offline full raw golden 39018B")
    # Argument/input errors must fail without needing a serial connection.
    malformed = directory / "malformed.ppm"
    malformed.write_bytes(b"P6\n160 80\n255\nshort")
    cases = [
        ["encode", "--input", str(malformed), "--output", str(directory / "bad.bin")],
        ["encode", "--pattern", "does-not-exist", "--output", str(directory / "bad.bin")],
        ["encode", "--pattern", "solid", "--mode", "invalid", "--output", str(directory / "bad.bin")],
    ]
    for arguments in cases:
        result = subprocess.run([str(binary)] + arguments, capture_output=True, text=True, timeout=3)
        require(result.returncode != 0, "invalid CLI input unexpectedly succeeded: " + " ".join(arguments))
    print("PASS offline CLI encode/raw/pair and malformed input")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    require(binary.is_file(), "CLI binary missing")
    require(os.name == "posix", "PTY tests require POSIX")
    with tempfile.TemporaryDirectory(prefix="msu2-pty-") as temporary:
        directory = pathlib.Path(temporary)
        fixture = directory / "fixture.ppm"
        expected = make_fixture(fixture)
        offline_checks(binary, directory, fixture, expected)
        result, peer, _ = run_cli(binary, ["probe", "--timeout-ms", "500"])
        require(peer.handshakes == 1 and not peer.decoder.frames, "probe must only handshake")
        require("16" in result.stdout + result.stderr, "probe should report version 16")
        print("PASS PTY probe: fragmented noisy ASCII16 greeting and exact echo")
        for mode, chunk, ack in (("raw", "60", "prefix"), ("pair", "3840", "prefix"), ("pair", "60", "echo")):
            _, peer, _ = run_cli(binary, ["send", "--input", str(fixture), "--mode", mode,
                                          "--chunk", chunk, "--ack", ack, "--timeout-ms", "500"],
                                 peer_options={"ack": ack})
            require(peer.decoder.frames == [expected], "send must reconstruct exactly one independent fixture frame")
            require(peer.decoder.windows == 1 and not peer.decoder.orientations, "send starts one window without implicit orientation")
            if mode == "raw":
                require(peer.decoder.frame_bytes == [39018], "PTY full raw wire golden")
        print("PASS PTY send: raw/pair, 60/3840B chunks, prefix/experimental echo ACK")
        _, peer, _ = run_cli(binary, ["stream", "--pattern", "moving", "--frames", "8", "--fps", "200",
                                      "--timeout-ms", "750", "--csv", str(directory / "stream.csv")],
                             peer_options={"ack_delay": 0.035}, bound=8.0)
        require(1 <= len(peer.decoder.frames) < 8, "slow ACK did not exercise waiting-frame replacement")
        require(peer.decoder.windows == len(peer.decoder.frames) and not peer.decoder.active,
                "stream interrupted a frame or failed to complete a window")
        require(all(len(frame) == 25600 for frame in peer.decoder.frames), "stream frame sizes")
        require((directory / "stream.csv").is_file(), "stream CSV output missing")
        with (directory / "stream.csv").open(newline="") as csv_file:
            rows = list(csv.DictReader(csv_file))
        require(len(rows) == len(peer.decoder.frames), "CSV frame rows disagree with completed wire frames")
        ids = [int(row["frame_id"]) for row in rows]
        require(ids == sorted(set(ids)) and ids[-1] == 7, "latest-frame stream must retain the last captured frame")
        require(int(rows[-1]["dropped_total"]) == 8 - len(rows), "CSV dropped-frame accounting")
        print(f"PASS PTY slow-ACK stream: {len(peer.decoder.frames)} completed frames, no overlapping windows")
        stream_checks(binary, directory, fixture, expected)
        priority_off_compatibility_checks(binary, directory, fixture, expected)
        adaptive_keyframe_checks(binary, directory, fixture, expected)
        center_probe_checks(binary, directory)
        priority_stream_checks(binary, directory)
        priority_failure_check(binary, directory)
        multipart_failure_checks(binary, directory)
        csv_and_argument_checks(binary, directory, fixture)
        diff_bench_checks(binary, directory)
        negative = [
            (["probe", "--timeout-ms", "100"], {"greeting": False}, "silent handshake"),
            (["probe", "--timeout-ms", "100"], {"corrupt_echo": True}, "incorrect exact handshake echo"),
            (["send", "--input", str(fixture), "--timeout-ms", "100"], {"no_ack": True}, "missing window ACK"),
            (["send", "--input", str(fixture), "--timeout-ms", "100"], {"disconnect": True}, "disconnect at window ACK"),
            (["send", "--input", str(fixture), "--ack", "echo", "--timeout-ms", "100"], {"ack": "prefix"}, "echo mode prefix-only rejection"),
        ]
        for arguments, options, name in negative:
            result, peer, elapsed = run_cli(binary, arguments, peer_options=options, success=False, bound=2.0)
            require(not peer.decoder.frames, "failed operation completed an unexpected frame")
            diagnostic = (result.stdout + result.stderr).lower()
            if name == "silent handshake":
                require("no msn version banner" in diagnostic,
                        "silent peer should be classified as a missing handshake banner")
            elif name == "disconnect at window ACK":
                require("serial read:" in diagnostic or "response timeout" in diagnostic,
                        "disconnect should be distinguishable as transport EOF/I/O or bounded ACK timeout")
            else:
                require("response timeout" in diagnostic,
                        name + " should be classified as a bounded response/ACK timeout")
            print(f"PASS PTY negative {name}: nonzero exit in {elapsed:.3f}s")
    host = platform.system() + " " + platform.machine()
    print(f"PASS POSIX CLI/PTY integration on {host}; physical hardware was not exercised")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print("FAIL PTY integration:", exc, file=sys.stderr)
        sys.exit(1)
