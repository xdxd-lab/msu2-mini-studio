#!/usr/bin/env python3
"""Bounded session-state/recovery probes and unchanged 60-FPS comparisons."""
import argparse
import csv
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

import run_hardware as runner
from run_fps_sweep import ORIGINAL_SHA, PORT, summarize as summarize_stream
from serial_guard import SerialGuard

ROOT = runner.PROJECT_ROOT
PROBE = ROOT / 'results/macos/session_state_tools/session_state_probe'

def read_rows(path):
    with Path(path).open(newline='') as handle:
        return list(csv.DictReader(handle))

def expected_operations(rows, scenario):
    """Derive the full planned sequence; a failed recovery may not claim completion."""
    expected = [('initial_handshake', 0), ('initial_full', 0)]
    indexed = {(row['operation'], int(row['cycle'])): row for row in rows}
    def add_send(operation, cycle):
        expected.append((operation, cycle))
        if indexed.get((operation, cycle), {}).get('status') == 'failed':
            if scenario.startswith('chunk3840'):
                expected.append(('failure_rx_monitor', cycle))
            expected.extend((name, cycle) for name in ('recovery_handshake', 'recovered_full_1', 'recovered_full_2'))
    if scenario == 'passive_idle':
        for cycle in (1, 2):
            expected.append(('idle_rx_monitor', cycle)); add_send('after_idle_full', cycle)
    elif scenario == 'silent_idle':
        expected.append(('silent_idle', 1)); add_send('after_idle_full', 1)
    elif scenario == 'active_control':
        for cycle in range(1, 7):
            expected.extend((('active_gap_monitor', cycle), ('active_full', cycle)))
    else:
        expected.append(('silent_gap', 1)); add_send('second_full', 1)
    return expected

def summarize_probe(case):
    log = Path(case['log_path']).read_text() if Path(case['log_path']).exists() else ''
    try:
        rows = read_rows(case['csv_path'])
    except (OSError, csv.Error) as error:
        rows = []; case['validation_errors'] = ['CSV unavailable: ' + str(error)]
    case['probe_rows'] = rows
    failures = [dict(operation=row.get('operation'), cycle=row.get('cycle'),
                     phase=row.get('failure_phase'), error=row.get('error'))
                for row in rows if row.get('status') == 'failed']
    case['protocol_failures'] = failures
    if 'Operation not permitted (errno 1)' in log:
        case.update(status='failed', recording_status='not_started', stage='sandbox_open_denied')
        return
    errors = list(case.get('validation_errors', []))
    complete_marker = 'COMPLETE protocol_probe scenario=' + case['scenario'] + ';'
    if log.count(complete_marker) != 1:
        errors.append('Missing complete planned probe summary')
    if not rows:
        errors.append('No probe rows')
    try:
        if [(row['operation'], int(row['cycle'])) for row in rows] != expected_operations(rows, case['scenario']):
            errors.append('Incomplete or unexpected operation sequence')
        for index, row in enumerate(rows):
            if None in row or len(row) != 17 or int(row['index']) != index:
                errors.append('Invalid CSV fields or row index')
            if row['status'] not in ('completed', 'failed'):
                errors.append('Unknown operation status')
            if row['operation'] in ('initial_handshake', 'initial_full', 'recovery_handshake', 'recovered_full_1', 'recovered_full_2', 'active_full') and row['status'] != 'completed':
                errors.append('Initial/control/recovery operation failed')
            if row['wire_bytes']:
                chunk = int(row['chunk'])
                if int(row['wire_bytes']) != 32970 or int(row['write_calls']) != 1 + (32952 + chunk - 1) // chunk:
                    errors.append('Unexpected full bytes/chunk count')
                if row['status'] != 'completed' or not row['window_ack_ms'] or not row['payload_write_ms']:
                    errors.append('Invalid completed send metrics')
            elif row['status'] == 'completed' and 'full' in row['operation']:
                errors.append('Completed full send lacks transfer metrics')
            if row['status'] == 'failed' and not row['failure_phase']:
                errors.append('Failed operation lacks phase')
            if row['rx_bytes']:
                rx, banners = int(row['rx_bytes']), int(row['banners'])
                if banners * 6 > rx or (banners > 0) != (float(row['first_banner_ms']) >= 0):
                    errors.append('Inconsistent RX/banner metrics')
            if row['operation'] in ('silent_idle', 'silent_gap', 'active_gap_monitor') or (
                row['operation'] in ('idle_rx_monitor', 'failure_rx_monitor') and int(row['banners']) == 0):
                if float(row['actual_ms']) < int(row['requested_ms']):
                    errors.append('Wait shorter than requested')
    except (ValueError, KeyError, TypeError) as error:
        errors.append('Invalid CSV values: ' + str(error))
    if case.get('exit_code') != (1 if failures else 0):
        errors.append('Exit code disagrees with protocol failures')
    if case.get('binary_sha256_before') != case.get('binary_sha256_expected') or case.get('binary_sha256_after') != case.get('binary_sha256_expected'):
        errors.append('Executable hash mismatch')
    if case.get('launcher_error'):
        errors.append(case['launcher_error'])
    case['validation_errors'] = errors
    case['recording_status'] = 'complete' if not errors else 'partial_or_invalid'
    case['stage'] = failures[-1]['phase'] if failures else ('completed' if not errors else 'other_failure')
    case['status'] = 'failed' if errors else ('diagnostic_complete_with_failures' if failures else 'passed')

def execute_probe(case):
    process = None
    with SerialGuard(PORT) as lease:
        case.update(serial_guard=lease.evidence, started_at=runner.utc_now(), status='running')
        try:
            case['binary_sha256_before'] = runner.sha256_file(PROBE)
            if case['binary_sha256_before'] != case['binary_sha256_expected']:
                case['launcher_error'] = 'Diagnostic binary changed; not launched'
                return
            with Path(case['log_path']).open('x') as log:
                log.write('Command: ' + shlex.join(case['command']) + '\n\n'); log.flush()
                process = subprocess.Popen(case['command'], stdout=log, stderr=subprocess.STDOUT)
                process.communicate(timeout=110)
        except (OSError, subprocess.TimeoutExpired) as error:
            case['launcher_error'] = str(error)
        except KeyboardInterrupt:
            case['launcher_error'] = 'Interrupted'; raise
        finally:
            if process is not None and process.returncode is None:
                runner.stop_process(process)
            case['exit_code'] = process.returncode if process is not None else None
            case['binary_sha256_after'] = runner.sha256_file(PROBE)
            case['completed_at'] = runner.utc_now()

def make_cases(directory, build):
    cases = []
    probe_sha = build['sha256'][str(PROBE.relative_to(ROOT))]
    for scenario in ('passive_idle', 'silent_idle', 'active_control'):
        cases.append(dict(name=scenario, kind='probe', scenario=scenario, binary_sha256_expected=probe_sha))
    def stream(name):
        case = runner.make_case(name, 'moving', 'diff', 60, 3600, keyframe_ms=0)
        case.update(kind='stream', chunk=60, binary_sha256_expected=ORIGINAL_SHA)
        cases.append(case)
    stream('original_60_60s_before_chunk3840')
    for scenario in ('chunk3840_gap0', 'chunk3840_gap100'):
        cases.append(dict(name=scenario, kind='probe', scenario=scenario, binary_sha256_expected=probe_sha))
    stream('original_60_60s_after_chunk3840')
    for index, case in enumerate(cases, 1):
        stem = '{:02d}_{}'.format(index, case['name'])
        case.update(status='planned', exit_code=None, csv_path=str(directory / (stem + '.csv')),
                    log_path=str(directory / (stem + '.log')))
        case['command'] = ([str(PROBE), PORT, case['csv_path'], case['scenario']] if case['kind'] == 'probe'
                           else runner.build_command((ROOT / 'msu2_cpp_v0_2/build/msu2').resolve(), PORT, case, Path(case['csv_path'])))
    return cases

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', default='session_state_a')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    if not args.label.replace('_', '').isalnum(): parser.error('Label must be alphanumeric or underscore')
    if runner.sha256_file(ROOT / 'msu2_cpp_v0_2/build/msu2') != ORIGINAL_SHA:
        parser.error('Original executable changed; no hardware opened')
    build = json.loads((PROBE.parent / 'build.json').read_text())
    for path, sha in build['sha256'].items():
        if runner.sha256_file(ROOT / path) != sha: parser.error('Diagnostic build mismatch: ' + path)
    directory = runner.create_run_directory(ROOT / 'results/macos/session_state')
    cases = make_cases(directory, build)
    manifest = dict(schema_version=1, suite='session_state', label=args.label, status='dry_run' if args.dry_run else 'running',
                    started_at=runner.utc_now(), cases=cases, diagnostic_build=build,
                    original_binary_sha256=ORIGINAL_SHA, visual_verification='not_observed',
                    method='Sequential guarded processes, 3s gaps, fixed original diff algorithm. Diagnostic full pattern only; passive reads are not USB/power-idle measurements. Recovery uses strict banner plus exact MSNCN echo on the same descriptor. 3840 recovery uses chunk60. No automatic case retries.')
    path = directory / 'manifest.json'
    runner.save_manifest(path, manifest)
    print('SUITE_RESULTS=' + str(directory), flush=True)
    if args.dry_run:
        for case in cases: print(shlex.join(case['command']))
        return 0
    aborted = False
    try:
        for index, case in enumerate(cases):
            if index: time.sleep(3)
            case['previous_case_stage'] = cases[index - 1].get('stage') if index else None
            runner.save_manifest(path, manifest)
            print('START ' + case['name'], flush=True)
            if case['kind'] == 'stream':
                runner.execute_case(case); summarize_stream(case)
            else:
                execute_probe(case); summarize_probe(case)
            runner.save_manifest(path, manifest)
            print('END ' + case['name'] + ' status=' + case['status'] + ' stage=' + case['stage'], flush=True)
            if case['stage'] == 'sandbox_open_denied' or (case['status'] == 'failed' and case['stage'] == 'other_failure'):
                aborted = True; break
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        aborted = True; manifest['stop_reason'] = str(error) or 'Interrupted'
        print('STOP ' + manifest['stop_reason'], flush=True)
    finally:
        for case in cases:
            if case['status'] in ('planned', 'running'): case['status'] = 'not_run'
        manifest.update(status='aborted' if aborted else 'completed', completed_at=runner.utc_now())
        runner.save_manifest(path, manifest)
    return 1 if aborted or any(case['status'] != 'passed' for case in cases) else 0

if __name__ == '__main__':
    sys.exit(main())
