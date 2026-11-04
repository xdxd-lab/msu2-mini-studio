#!/usr/bin/env python3
"""Sequential 45..60 FPS comparisons. One failed stream remains failed; no retry."""
import argparse
import json
from pathlib import Path
import sys
import subprocess
import time

import run_hardware as runner
import analyze_results as analyzer

PORT = '/dev/cu.usbmodem01234567891'
ORIGINAL_SHA = '4b64f7a3b542653ac843551cd0d7193adbb339c09ff64840e460e992307d960c'


def classify(case):
    text = Path(case['log_path']).read_text() if Path(case['log_path']).exists() else ''
    if case['status'] == 'passed':
        return 'completed'
    if 'Operation not permitted (errno 1)' in text:
        return 'sandbox_open_denied'
    if 'no MSN version banner' in text:
        return 'handshake_banner'
    if 'response timeout' in text:
        return 'lcd_ack_timeout' if 'MSN handshake verified' in text else 'handshake_echo'
    return 'other_failure'


def summarize(case):
    analysis = analyzer.analyze_case(case['csv_path'], case['log_path'])
    analysis['recording_status'] = analysis['status']
    analysis['acceptance_status'] = 'PASS' if case['status'] == 'passed' and case.get('exit_code') == 0 else 'FAIL'
    if analysis['acceptance_status'] != 'PASS':
        analysis['status'] = 'FAIL'
        analysis['errors'].append('Case acceptance failed: ' + case.get('error', case['status']))
    case['analysis'] = analysis
    case['stage'] = classify(case)
    try:
        rows = analyzer.read_csv(case['csv_path'])
    except (OSError, ValueError):
        rows = []
    case['initial_dropped_total'] = rows[0]['dropped_total'] if rows else None
    case['post_initial_drops'] = rows[-1]['dropped_total'] - rows[0]['dropped_total'] if rows else None
    case['last_completed_frame_id'] = rows[-1]['frame_id'] if rows else None
    case['completed_csv_rows'] = len(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fps', nargs='+', type=int, default=list(range(45, 61)))
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--label', default='ascending')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    if not args.fps or any(f < 45 or f > 60 for f in args.fps) or args.fps != sorted(set(args.fps)):
        parser.error('--fps must be unique, ascending values in 45..60')
    if args.seconds != 30:
        parser.error('This controlled experiment uses exactly 30 seconds per FPS')
    if not args.label.replace('_', '').isalnum():
        parser.error('--label must be alphanumeric or underscore')
    binary = (runner.PROJECT_ROOT / 'msu2_cpp_v0_2/build/msu2').resolve()
    if runner.sha256_file(binary) != ORIGINAL_SHA:
        parser.error('Original executable SHA256 does not match; no device opened')
    directory = runner.create_run_directory(runner.PROJECT_ROOT / 'results/macos/fps_sweep')
    cases = []
    for index, fps in enumerate(args.fps, 1):
        c = runner.make_case('moving_diff_' + str(fps), 'moving', 'diff', fps, fps * args.seconds, keyframe_ms=0)
        stem = '{:02d}_fps{}'.format(index, fps)
        c.update(chunk=60, stem=stem, repeat=1, status='planned', exit_code=None,
                 binary_sha256_expected=ORIGINAL_SHA,
                 csv_path=str(directory / (stem + '.csv')), log_path=str(directory / (stem + '.log')))
        c['command'] = runner.build_command(binary, PORT, c, Path(c['csv_path']))
        cases.append(c)
    manifest = {'schema_version': 1, 'label': args.label, 'suite': 'fps_sweep',
                'started_at': runner.utc_now(), 'status': 'dry_run' if args.dry_run else 'running',
                'binary': {'path': str(binary), 'sha256': ORIGINAL_SHA}, 'cases': cases,
                'visual_verification': 'not_observed',
                'design': 'ascending; 30s; independent new process; 3s inter-case; fixed algorithm/chunk/ACK; no automatic retries'}
    manifest_path = directory / 'manifest.json'
    runner.save_manifest(manifest_path, manifest)
    print('SWEEP_RESULTS=' + str(directory), flush=True)
    if args.dry_run:
        for c in cases:
            print(' '.join(c['command']))
        return 0
    aborted = False
    try:
        for index, c in enumerate(cases):
            if index:
                time.sleep(3)
            c['previous_case_stage'] = cases[index-1].get('stage') if index else None
            print('START fps={} frames={}'.format(c['fps'], c['expected_frames']), flush=True)
            runner.execute_case(c)
            summarize(c)
            runner.save_manifest(manifest_path, manifest)
            print('END fps={} stage={} rows={} initial_drop={} later_drop={}'.format(
                c['fps'], c['stage'], c['completed_csv_rows'], c['initial_dropped_total'], c['post_initial_drops']), flush=True)
            if c['stage'] in ('sandbox_open_denied', 'other_failure'):
                aborted = True
                break
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        aborted = True
        manifest['stop_reason'] = str(error) or 'Interrupted'
        print('STOP: ' + manifest['stop_reason'], flush=True)
    finally:
        manifest['status'] = 'aborted' if aborted else 'completed'
        manifest['completed_at'] = runner.utc_now()
        for c in cases:
            if c['status'] == 'planned':
                c['status'] = 'not_run'
        runner.save_manifest(manifest_path, manifest)
    return 1 if aborted or any(c['status'] != 'passed' for c in cases) else 0


if __name__ == '__main__':
    sys.exit(main())
