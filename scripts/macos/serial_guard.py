"""Cooperative single-device lease and read-only cu/tty owner check; no serial I/O."""
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


class DeviceBusy(RuntimeError):
    pass


def aliases(port):
    path = Path(port)
    name = path.name
    if name.startswith(('cu.', 'tty.')):
        suffix = name.split('.', 1)[1]
        return [str(path.parent / ('cu.' + suffix)), str(path.parent / ('tty.' + suffix))]
    return [str(path)]


def check_idle(port):
    paths = [p for p in aliases(port) if Path(p).exists()]
    if not paths:
        raise DeviceBusy('Serial device is absent: ' + port)
    result = subprocess.run(['/usr/sbin/lsof', '-nP', '--'] + paths,
                            capture_output=True, text=True, timeout=10)
    if result.returncode == 0 and result.stdout.strip():
        raise DeviceBusy('Serial device already has an owner; no new test launched:\n' + result.stdout.strip())
    if result.returncode != 1 or result.stdout.strip() or result.stderr.strip():
        raise DeviceBusy('Cannot establish that serial device is idle: ' + result.stderr.strip())
    return {'aliases_checked': paths, 'lsof_exit_code': result.returncode,
            'checked_at_unix': time.time()}


class SerialGuard:
    def __init__(self, port, lock_root=None, idle_check=None):
        self.port = port
        self.lock_root = Path(lock_root) if lock_root else ROOT / 'results/macos/.device-locks'
        self.idle_check = idle_check or check_idle
        self.handle = None
        self.evidence = None

    def __enter__(self):
        key = hashlib.sha256(aliases(self.port)[0].encode()).hexdigest()[:20]
        self.lock_root.mkdir(parents=True, exist_ok=True)
        self.handle = (self.lock_root / (key + '.lock')).open('a+')
        try:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.handle.close()
            self.handle = None
            raise DeviceBusy('Another test holds the shared device lock; no new test launched')
        try:
            self.evidence = self.idle_check(self.port)
            self.handle.seek(0)
            self.handle.truncate()
            json.dump({'pid': os.getpid(), 'port': self.port, 'acquired_at_unix': time.time()}, self.handle)
            self.handle.flush()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        if self.handle is not None:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
            self.handle.close()
            self.handle = None


@contextlib.contextmanager
def guard_command(command):
    if '--port' not in command:
        yield None
    else:
        with SerialGuard(command[command.index('--port') + 1]) as lease:
            yield lease
