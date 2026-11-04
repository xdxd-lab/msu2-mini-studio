#!/usr/bin/env python3
"""Offline PTY checks for send --hold-open-ms; never opens a physical port."""
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import os
import shutil
import time

import integration_pty as pty_test


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def command(binary, port, hold_ms):
    args = [str(binary), "send", "--port", port, "--pattern", "bars", "--mode", "pair"]
    if hold_ms is not None:
        args += ["--hold-open-ms", str(hold_ms)]
    return args


def wait_stdout_marker(child, marker, timeout):
    collected = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and marker not in collected:
        ready, _, _ = __import__("select").select([child.stdout], [], [], 0.05)
        if ready:
            chunk = os.read(child.stdout.fileno(), 4096)
            if not chunk:
                break
            collected.extend(chunk)
        check(child.poll() is None or marker in collected,
              f"child exited before stdout marker {marker!r}")
    check(marker in collected, f"stdout marker missing: {marker!r}")
    return bytes(collected)


def require_child_holds_port(child, port):
    lsof = shutil.which("lsof") or "/usr/sbin/lsof"
    check(pathlib.Path(lsof).is_file(), "lsof is required to inspect the child PTY fd")
    result = subprocess.run([lsof, "-Fn", "-a", "-p", str(child.pid)],
                            capture_output=True, text=True, timeout=3)
    check(result.returncode == 0, f"lsof failed for child PID {child.pid}: {result.stderr}")
    check("\nn" + port + "\n" in "\n" + result.stdout,
          f"child PID {child.pid} does not hold PTY {port}: {result.stdout}")
    return [line for line in result.stdout.splitlines() if line == "n" + port]


def run(binary, hold_ms):
    import pty
    import tty

    master, slave = pty.openpty()
    tty.setraw(slave)
    peer = pty_test.MiniPeer(master)
    peer.start()
    start = time.monotonic()
    port = os.ttyname(slave)
    child = subprocess.Popen(command(binary, port, hold_ms),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    fd_evidence = []
    requested_ms = 13 if hold_ms is None else hold_ms
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and pty_test.WAKE not in peer.raw_received:
            check(child.poll() is None, "child exited before opening PTY")
            time.sleep(0.005)
        check(pty_test.WAKE in peer.raw_received, "child never opened PTY")
        requested = wait_stdout_marker(child,
                                       f"post_send_hold_requested_ms={requested_ms}".encode(), 8)
        frame_deadline = time.monotonic() + 2
        while not peer.decoder.frames and time.monotonic() < frame_deadline:
            time.sleep(0.005)
        check(len(peer.decoder.frames) == 1,
              f"send marker appeared before PTY decoded the frame: raw={len(peer.raw_received)} "
              f"buffer={len(peer.buffer)} active={peer.decoder.active} "
              f"framebytes={len(peer.decoder.frame)} windows={peer.decoder.windows} error={peer.error!r}")
        # Do not add lsof's 150ms observation delay to the short default case.
        if hold_ms is not None and hold_ms >= 100:
            fd_evidence.extend(require_child_holds_port(child, port))
            time.sleep(0.15)
            check(child.poll() is None, "child exited before hold interval elapsed")
            fd_evidence.extend(require_child_holds_port(child, port))
        rest_stdout, result_stderr = child.communicate(timeout=12)
        elapsed = time.monotonic() - start
        result = subprocess.CompletedProcess(child.args, child.returncode,
                                             (requested + rest_stdout).decode(), result_stderr.decode())
    finally:
        if slave >= 0:
            os.close(slave)
        if child.poll() is None:
            child.kill()
            child.wait()
        peer.finish()
    check(result.returncode == 0, f"hold {hold_ms} failed: {result.stderr}")
    check(len(peer.decoder.frames) == 1, "send did not complete exactly one LCD frame")
    check(not peer.buffer, "send left a partial serial command")
    check("post_send_hold_requested_ms=" + str(requested_ms) in result.stdout,
          "hold duration was not reported")
    check("post_send_hold_elapsed_ms=" in result.stdout, "hold elapsed time was not reported")
    check(len(peer.raw_received) > 6, "PTY did not receive send payload")
    hold_elapsed = float(re.search(r"post_send_hold_elapsed_ms=([0-9.]+)", result.stdout).group(1))
    return bytes(peer.raw_received), elapsed, hold_elapsed, peer.decoder.frames[0], fd_evidence


def interrupt_during_hold(binary):
    import os
    import pty
    import tty

    master, slave = pty.openpty()
    tty.setraw(slave)
    peer = pty_test.MiniPeer(master)
    peer.start()
    port = os.ttyname(slave)
    child = subprocess.Popen(command(binary, port, 10000),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and pty_test.WAKE not in peer.raw_received:
            check(child.poll() is None, "child exited before opening PTY")
            time.sleep(0.005)
        check(pty_test.WAKE in peer.raw_received, "child never opened PTY")
        before_signal = wait_stdout_marker(child, b"post_send_hold_requested_ms=10000", 8)
        check(len(peer.decoder.frames) == 1, "hold marker preceded completed bars payload")
        fd_first = require_child_holds_port(child, port)
        time.sleep(0.1)
        fd_second = require_child_holds_port(child, port)
        child.send_signal(signal.SIGINT)
        rest_stdout, stderr = child.communicate(timeout=3)
        stdout = (before_signal + rest_stdout).decode()
        check(child.returncode == 130, f"SIGINT exit was {child.returncode}: {stderr.decode()}")
        check("interrupted=1" in stdout, "SIGINT was not reported as hold interruption")
        check("post_send_hold_requested_ms=10000" in stdout,
              "SIGINT was sent before hold request marker")
        check(len(peer.decoder.frames) == 1, "SIGINT hold emitted another frame")
        return fd_first + fd_second
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        if slave >= 0:
            os.close(slave)
        peer.finish()


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    check(binary.is_file(), "candidate binary is missing")
    with tempfile.TemporaryDirectory(prefix="msu2-hold-pty-"):
        payload0, elapsed0, hold0, frame0, _ = run(binary, 0)
        payload_default, elapsed_default, hold_default, frame_default, _ = run(binary, None)
        payload13, elapsed13, hold13, frame13, _ = run(binary, 13)
        payload1, elapsed1, hold1, frame1, fds = run(binary, 1000)
        check(payload0 == payload_default == payload13 == payload1,
              "hold duration changed the complete serial byte stream")
        check(frame0 == frame_default == frame13 == frame1,
              "hold duration changed decoded bars pixels")
        check(hold0 < 50, f"hold0 lasted {hold0:.1f}ms")
        check(hold_default >= 13, f"default hold reported only {hold_default:.1f}ms")
        check(hold13 >= 13, f"explicit 13ms hold reported only {hold13:.1f}ms")
        check(900 <= hold1 < 3000, f"1000ms hold lasted {hold1:.1f}ms")
        check(elapsed1 - elapsed0 >= 0.8, f"1000ms hold did not delay transport close: {elapsed0:.3f}s vs {elapsed1:.3f}s")
        # lsof checks the child's own descriptor, so the parent's slave handle
        # cannot mask an early transport close.
        check(elapsed1 < 5, "1000ms hold exceeded bounded close time")
        interrupt_fds = interrupt_during_hold(binary)
    print(f"PASS hold-open PTY: identical payloads; run0={elapsed0:.3f}s/hold={hold0:.1f}ms "
          f"default13={elapsed_default:.3f}s/hold={hold_default:.1f}ms "
          f"explicit13={elapsed13:.3f}s/hold={hold13:.1f}ms "
          f"run1000={elapsed1:.3f}s/hold={hold1:.1f}ms; SIGINT exit=130")
    print("child_fd_during_1000ms_hold=" + repr(fds))
    print("child_fd_before_sigint=" + repr(interrupt_fds))


if __name__ == "__main__":
    main()
