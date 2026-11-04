#!/usr/bin/env python3
"""Build a new isolated observation tool and run fake-transport checks only."""
from pathlib import Path
import csv
import hashlib
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/macos/session_state_tools'

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    src = ROOT / 'msu2_cpp_v0_2/src'
    sources = [src / name for name in ('protocol.cpp', 'image.cpp', 'session.cpp', 'transport.cpp')]
    sources.append(ROOT / 'tests/macos/session_state_probe.cpp')
    binary = OUT / 'session_state_probe'
    command = ['c++', '-std=c++17', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic',
               '-pthread', '-I', str(ROOT / 'msu2_cpp_v0_2/include')]
    command += [str(path) for path in sources] + ['-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary), '--self-test'], check=True)
    with tempfile.TemporaryDirectory(prefix='msu2_session_fake_') as directory:
        csv_path = Path(directory) / 'fake.csv'
        subprocess.run([str(binary), '--self-test-csv', str(csv_path)], check=True)
        with csv_path.open(newline='') as handle:
            rows = list(csv.DictReader(handle))
        if len(rows) != 3 or any(len(row) != 17 or None in row for row in rows):
            raise RuntimeError('Actual C++ CSV schema is invalid')
        if rows[0]['wire_bytes'] != '32970' or rows[0]['write_calls'] != '551' or rows[0]['rx_bytes'] != '':
            raise RuntimeError('Actual send fields shifted')
        if rows[1]['rx_bytes'] != '24' or rows[1]['banners'] != '4' or rows[1]['wire_bytes'] != '':
            raise RuntimeError('Actual monitor fields shifted')
        if rows[2]['failure_phase'] != 'window_ack_wait' or rows[2]['error'] != 'quoted, "error"':
            raise RuntimeError('Actual failure CSV quoting is invalid')
    metadata = dict(command=command, self_test='passed_fake_transport_and_actual_csv_only',
                    sha256={str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in sources + [binary]})
    (OUT / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata['sha256'], indent=2))

if __name__ == '__main__':
    main()
