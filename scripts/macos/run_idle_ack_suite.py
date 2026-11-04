#!/usr/bin/env python3
"""Bounded, sequential idle/reopen comparisons on the already identified MINI."""
import argparse
import csv
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

import run_hardware as runner
from serial_guard import SerialGuard
from run_fps_sweep import ORIGINAL_SHA, PORT, classify

ROOT = runner.PROJECT_ROOT

def read_probe(path):
    with Path(path).open(newline='') as handle:
        return list(csv.DictReader(handle))

def validate_probe(case):
    rows = read_probe(case['csv_path'])
    delays = [0,0,5000,0,30000,0,60000,0]
    if case['order'] == 'descending':
        delays = [0,0,60000,0,30000,0,5000,0]
    errors = []
    if len(rows) != 8:
        errors.append('Expected exactly eight completed full sends')
    calls = 1 + (32952 + case['chunk'] - 1) // case['chunk']
    for index, row in enumerate(rows):
        if row['status'] != 'completed':
            errors.append('Failed send at index ' + row['index'])
            continue
        if int(row['index']) != index or int(row['idle_requested_ms']) != delays[index]:
            errors.append('Unexpected idle schedule or row order')
        if int(row['wire_bytes']) != 32970 or int(row['write_calls']) != calls:
            errors.append('Unexpected full-frame wire bytes or chunk count')
        if float(row['idle_actual_ms']) < delays[index]:
            errors.append('Idle duration shorter than requested')
        for metric in ('window_ack_ms','payload_write_ms','send_host_ms'):
            if not 0 <= float(row[metric]) < 2000:
                errors.append('Invalid successful timing: ' + metric)
    log = Path(case['log_path']).read_text()
    if 'MSN handshake verified' not in log or 'COMPLETE sends=8' not in log:
        errors.append('Missing complete handshake or final summary')
    case['validation_errors'] = errors
    case['status'] = 'failed' if errors else 'passed'
    return not errors

def execute_probe(case):
    process = None
    with SerialGuard(PORT) as lease:
        case.update(serial_guard=lease.evidence, status='running', started_at=runner.utc_now())
        try:
            case['binary_sha256_before'] = runner.sha256_file(Path(case['command'][0]))
            if case['binary_sha256_before'] != case['binary_sha256_expected']:
                case.update(status='failed', error='diagnostic binary changed; not launched')
                return False
            with Path(case['log_path']).open('x') as log:
                log.write('Command: ' + shlex.join(case['command']) + '\n\n')
                log.flush()
                process = subprocess.Popen(case['command'], stdout=log, stderr=subprocess.STDOUT)
                try:
                    process.communicate(timeout=125)
                    case['exit_code'] = process.returncode
                    case['status'] = 'process_completed' if process.returncode == 0 else 'failed'
                except subprocess.TimeoutExpired:
                    case.update(status='failed', error='probe launcher timeout')
        except (OSError, KeyboardInterrupt) as error:
            case.update(status='failed', error=str(error) or 'Interrupted')
            if isinstance(error, KeyboardInterrupt):
                raise
        finally:
            if process is not None and process.returncode is None:
                runner.stop_process(process)
            if process is not None:
                case['exit_code'] = process.returncode
            case['binary_sha256_after'] = runner.sha256_file(Path(case['command'][0]))
            if case.get('binary_sha256_before') != case['binary_sha256_after']:
                case.update(status='failed', error='diagnostic binary changed during run')
            case['completed_at'] = runner.utc_now()
    if case['status'] == 'process_completed':
        return validate_probe(case)
    return False

def summarize(case):
    log = Path(case['log_path']).read_text() if Path(case['log_path']).exists() else ''
    if case['kind'] == 'stream':
        case['analysis'] = runner.analyze_recording(case['csv_path'], case['log_path'])
        case['stage'] = classify(case)
        return
    try:
        case['probe_rows'] = read_probe(case['csv_path'])
    except OSError:
        case['probe_rows'] = []
    if case['status'] == 'passed':
        case['stage'] = 'completed'
    elif 'Operation not permitted (errno 1)' in log:
        case['stage'] = 'sandbox_open_denied'
    elif case['probe_rows'] and case['probe_rows'][-1]['failure_phase']:
        case['stage'] = case['probe_rows'][-1]['failure_phase']
    else:
        found = re.search(r'FAIL phase=(\S+)', log)
        case['stage'] = found.group(1) if found else 'other_failure'
    opening = re.search(r'OPEN_MS=([\d.]+)', log)
    handshake = re.search(r'HANDSHAKE_MS=([\d.]+)', log)
    case['open_ms'] = float(opening.group(1)) if opening else None
    case['handshake_ms'] = float(handshake.group(1)) if handshake else None

def make_cases(directory, build):
    original = (ROOT / 'msu2_cpp_v0_2/build/msu2').resolve()
    cases=[]
    def stream(name,fps,binary=original,sha=ORIGINAL_SHA,conditional=False):
        c=runner.make_case(name,'moving','diff',fps,fps*30,keyframe_ms=0)
        c.update(kind='stream', chunk=60, conditional=conditional, binary_sha256_expected=sha)
        cases.append(c)
    def probe(name,order,chunk=60):
        binary=ROOT/'results/macos/idle_ack_tools/idle_full_probe'
        cases.append(dict(name=name,kind='idle_probe',order=order,chunk=chunk,
                          binary=str(binary), binary_sha256_expected=build['sha256']['results/macos/idle_ack_tools/idle_full_probe']))
    # Fixed 60 before any lower-FPS control, rather than an ascending warmup.
    stream('original_60_first',60)
    stream('original_45_control',45)
    stream('original_60_repeat',60)
    probe('same_connection_idle_ascending','ascending')
    probe('same_connection_idle_descending','descending')
    stream('original_60_after_idle_suite',60)
    # Established failing chunk: probe stops at the first failed send.
    probe('full_chunk3840_contrast','ascending',3840)
    trace=ROOT/'results/macos/idle_ack_tools/msu2_patch_trace'
    stream('patch_trace_60_if_ack_failed',60,trace,
           build['sha256']['results/macos/idle_ack_tools/msu2_patch_trace'],True)
    for index,c in enumerate(cases,1):
        c.update(status='planned',exit_code=None,stem='{:02d}_{}'.format(index,c['name']))
        c['csv_path']=str(directory/(c['stem']+'.csv'))
        c['log_path']=str(directory/(c['stem']+'.log'))
        if c['kind']=='stream':
            binary=trace if c.get('conditional') else original
            c['command']=runner.build_command(binary,PORT,c,Path(c['csv_path']))
        else:
            c['command']=[c['binary'],PORT,c['csv_path'],c['order'],str(c['chunk'])]
    return cases

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label',default='idle_reopen')
    parser.add_argument('--dry-run',action='store_true')
    args=parser.parse_args(argv)
    if not args.label.replace('_','').isalnum():
        parser.error('label must be alphanumeric or underscore')
    if runner.sha256_file(ROOT/'msu2_cpp_v0_2/build/msu2')!=ORIGINAL_SHA:
        parser.error('Original executable changed; no hardware opened')
    build=json.loads((ROOT/'results/macos/idle_ack_tools/build.json').read_text())
    for path,sha in build['sha256'].items():
        if runner.sha256_file(ROOT/path)!=sha:
            parser.error('Diagnostic build mismatch: '+path)
    directory=runner.create_run_directory(ROOT/'results/macos/idle_ack')
    cases=make_cases(directory,build)
    manifest=dict(schema_version=1,suite='idle_ack',label=args.label,status='dry_run' if args.dry_run else 'running',
                  started_at=runner.utc_now(),cases=cases,diagnostic_build=build,
                  original_binary_sha256=ORIGINAL_SHA,visual_verification='not_observed',
                  method='Sequential independent processes, 3s gaps, no automatic retries; same-connection idle probes; original stream comparisons; conditional trace distinct from original')
    path=directory/'manifest.json'
    runner.save_manifest(path,manifest)
    print('SUITE_RESULTS='+str(directory),flush=True)
    if args.dry_run:
        for c in cases:
            print(('CONDITIONAL ' if c.get('conditional') else '')+shlex.join(c['command']))
        return 0
    aborted=False
    try:
        for index,c in enumerate(cases):
            if c.get('conditional') and not any(x.get('stage')=='lcd_ack_timeout' for x in cases[:index] if x['kind']=='stream'):
                c.update(status='skipped',reason='No original stream ACK failure in this suite');continue
            if index: time.sleep(3)
            c['previous_case_stage']=cases[index-1].get('stage') if index else None
            c['status']='running';runner.save_manifest(path,manifest)
            print('START '+c['name'],flush=True)
            if c['kind']=='stream': runner.execute_case(c)
            else: execute_probe(c)
            summarize(c);runner.save_manifest(path,manifest)
            print('END '+c['name']+' status='+c['status']+' stage='+c['stage'],flush=True)
            if c['stage'] in ('sandbox_open_denied','other_failure'):
                aborted=True;break
    except (RuntimeError,OSError,subprocess.SubprocessError,KeyboardInterrupt) as error:
        aborted=True;manifest['stop_reason']=str(error) or 'Interrupted'
        print('STOP '+manifest['stop_reason'],flush=True)
    finally:
        for c in cases:
            if c['status'] in ('planned','running'): c['status']='not_run'
        manifest.update(status='aborted' if aborted else 'completed',completed_at=runner.utc_now())
        runner.save_manifest(path,manifest)
    return 1 if aborted or any(c['status']=='failed' for c in cases) else 0

if __name__=='__main__':
    sys.exit(main())
