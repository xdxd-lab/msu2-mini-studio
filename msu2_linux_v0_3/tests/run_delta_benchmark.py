#!/usr/bin/env python3
"""Build and run the fixed corpus against one or two source trees (no hardware)."""
import argparse
import csv
import statistics
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('--baseline', type=Path, required=True, help='baseline core source directory')
parser.add_argument('--candidate', type=Path, help='candidate core source directory')
parser.add_argument('--output', type=Path, required=True, help='new output directory')
parser.add_argument('--rounds', type=int, default=30)
args = parser.parse_args()
if args.rounds < 1:
    parser.error('rounds must be positive')
args.output.mkdir(parents=True, exist_ok=False)
out = args.output.resolve()
harness = Path(__file__).resolve().with_name('benchmark_delta.cpp')
cxx = os.environ.get('CXX', 'clang++')
log = (out / 'commands.log').open('w')
def run(cmd):
    log.write('\nCOMMAND: ' + json.dumps([str(x) for x in cmd]) + '\nSTART: ' + datetime.datetime.now().astimezone().isoformat() + '\n')
    log.flush()
    start = time.monotonic()
    p = subprocess.run([str(x) for x in cmd], stdout=log, stderr=subprocess.STDOUT)
    log.write(f'EXIT: {p.returncode}\nELAPSED_SECONDS: {time.monotonic()-start:.3f}\n')
    log.flush()
    if p.returncode:
        raise RuntimeError('command failed; see commands.log')
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
try:
    run([cxx, '--version'])
    sources = [('baseline', args.baseline)] + ([('candidate', args.candidate)] if args.candidate else [])
    for label, root in sources:
        root = root.resolve()
        exe = out / (label + '_benchmark')
        run([cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-I' + str(root / 'include'), harness, root / 'src/delta.cpp', root / 'src/protocol.cpp', '-o', exe])
        log.write(f'SHA256 {label} executable: {sha(exe)}\nSHA256 harness: {sha(harness)}\n')
        for p in [root / 'src/delta.cpp', root / 'src/protocol.cpp', root / 'include/msu2/delta.hpp', root / 'include/msu2/protocol.hpp']:
            log.write(f'SHA256 {p}: {sha(p)}\n')
        log.flush()
    for sample in range(5):
        order = sources if sample % 2 == 0 else list(reversed(sources))
        for label, _ in order:
            run([out / (label + '_benchmark'), out / f'{label}_{sample}', str(args.rounds)])
    reference = (out / 'baseline_0.plans.bin').read_bytes()
    for label, _ in sources:
        for sample in range(5):
            equal = (out / f'{label}_{sample}.plans.bin').read_bytes() == reference
            log.write(f'BYTE COMPARISON {label}_{sample}: {"identical" if equal else "DIFFERENT"}\n')
            if not equal:
                raise RuntimeError('serialized plans differ')
        samples = []
        for sample in range(5):
            with (out / f'{label}_{sample}.csv').open() as f:
                samples.append(list(csv.DictReader(f)))
        with (out / f'{label}_median.csv').open('w', newline='') as f:
            fields = list(samples[0][0]) + ['median_ns_per_plan']
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            for rows in zip(*samples):
                row = dict(rows[0])
                row['median_ns_per_plan'] = statistics.median(int(r['total_ns']) / args.rounds for r in rows)
                writer.writerow(row)
except (OSError, RuntimeError) as exc:
    log.write('ERROR: ' + str(exc) + '\n')
    print(exc, file=sys.stderr)
    sys.exit(1)
finally:
    log.close()
