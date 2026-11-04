#!/usr/bin/env python3
"""Sequential moving/static/moving probes; no USB access in --dry-run."""
import argparse
import json
import math
from pathlib import Path
import re
import shlex
import sys
import time

import analyze_results as analyzer
import run_hardware as runner
from run_fps_sweep import ORIGINAL_SHA, PORT
from serial_guard import DeviceBusy, SerialGuard

ROOT = runner.PROJECT_ROOT
TOOLS = ROOT / 'results/macos/static_transition_tools'
INT_FIELDS = ('frame', 'patch', 'patch_count', 'size', 'captured', 'value1', 'value2')
FIELDS = {'t_ms', 'phase', 'kind', 'data'} | set(INT_FIELDS)
BANNER = re.compile(b'\x00MSN[0-9][0-9]')


def parse_trace(text):
    events = []
    previous = -1.0
    trailer = []
    for line_number, line in enumerate(text.splitlines(), 1):
        if line.startswith('TRACE '):
            trailer.append(dict(token.split('=', 1) for token in line.split()[1:]))
        if not line.startswith('OBS '):
            continue
        tokens = line.split()[1:]
        pairs = [token.split('=', 1) for token in tokens]
        if any(len(pair) != 2 for pair in pairs):
            raise ValueError('Malformed OBS at line ' + str(line_number))
        event = dict(pairs)
        if len(pairs) != len(FIELDS) or set(event) != FIELDS:
            raise ValueError('Incorrect/duplicate OBS fields at line ' + str(line_number))
        event['t_ms'] = float(event['t_ms'])
        if not math.isfinite(event['t_ms']) or event['t_ms'] < 0 or event['t_ms'] < previous:
            raise ValueError('Nonfinite/nonmonotonic trace clock')
        previous = event['t_ms']
        for key in INT_FIELDS:
            event[key] = analyzer.numeric(event[key], key, integer=True)
        if not re.fullmatch('[0-9a-f]*', event['data']) or len(event['data']) != 2 * event['captured']:
            raise ValueError('Trace captured hex length mismatch')
        if event['captured'] > min(512, event['size']):
            raise ValueError('Trace capture size exceeds capacity')
        if event['kind'] == 'R' and event['captured'] != event['size']:
            raise ValueError('Truncated RX capture')
        if event['kind'] in ('T', 'W') and event['captured'] != min(18, event['size']):
            raise ValueError('Incorrect TX command-prefix capture size')
        if event['kind'] not in {'R', 'T', 'W', 'F', 'P', 'U', 'E', 'M'}:
            raise ValueError('Unknown trace event kind')
        event['line'] = line_number
        events.append(event)
    if len(trailer) != 1 or trailer[0] != {'events': str(len(events)), 'overflow': '0'}:
        raise ValueError('Missing/overflowed/inconsistent trace trailer')
    return events


def cases():
    matrix = [('A01_45_static3_baseline', 45, 3, 0),
              ('A02_45_static6_baseline', 45, 6, 0),
              ('A03_60_static6_baseline', 60, 6, 0),
              ('A04_45_static6_recovery', 45, 6, 1),
              ('A05_60_static6_recovery', 60, 6, 1),
              ('A06_45_static3_baseline_repeat', 45, 3, 0)]
    return [dict(runner.make_case(name, 'moving', 'diff', fps, fps*(5+hold), keyframe_ms=0),
                 chunk=60, repeat=1, static_ms=hold*1000, recovery=bool(recovery),
                 warm_frames=fps, resume_frame=fps*(1+hold))
            for name, fps, hold, recovery in matrix]


def summarize(case):
    text = Path(case['log_path']).read_text()
    errors = []
    try:
        trace = parse_trace(text)
    except (ValueError, TypeError) as error:
        trace = []; errors.append(str(error))
    try:
        rows = analyzer.read_csv(case['csv_path'])
    except (OSError, ValueError) as error:
        rows = []; case['csv_error'] = str(error)
    fault_events = [e for e in trace if e['kind'] == 'E']
    for e in fault_events:
        e['error'] = bytes.fromhex(e['data']).decode('utf-8', errors='replace')
    first_fault = fault_events[0] if fault_events else None
    markers = [e for e in trace if e['kind'] == 'M' and (e['phase'].startswith('recovery_') or e['phase']=='original_failure')]
    resumed_window = next((e for e in trace if e['kind']=='T' and e['phase']=='window_write'
                           and e['frame']>=case['resume_frame']), None)
    last_payload = next((e for e in reversed(trace) if e['kind']=='W' and e['phase']=='payload_write'
                         and resumed_window and e['t_ms']<resumed_window['t_ms']), None)
    static_rows = [r for r in rows if case['warm_frames']<=r['frame_id']<case['resume_frame']]
    if any(r['update_kind']!='unchanged' or r['wire_bytes'] or r['write_calls'] or r['rectangles'] for r in static_rows):
        errors.append('Static source unexpectedly transmitted an update')
    if any(e['kind'] in ('T','W') and case['warm_frames']<=e['frame']<case['resume_frame'] for e in trace):
        errors.append('Static source performed TX')
    if any(e['kind']=='R' and case['warm_frames']<=e['frame']<case['resume_frame'] for e in trace):
        errors.append('Static source performed an unexpected RX read')
    phases = {e['phase'] for e in markers}
    attempts = sum(e['phase']=='recovery_begin' for e in markers)
    if attempts>1 or (not case['recovery'] and attempts):
        errors.append('Recovery attempt violated configured bound')
    if attempts and not (first_fault and first_fault['phase']=='window_ack_wait' and first_fault['value1']>0):
        errors.append('Recovery started without observed MSN in failed window ACK')
    if 'recovery_full_ok' in phases and 'recovery_handshake_ok' not in phases:
        errors.append('Full recovery without strict handshake')
    recovery_full_rows = [r for r in rows if r['frame_id']>=case['resume_frame'] and r['update_kind']=='full']
    if 'recovery_full_ok' in phases and not recovery_full_rows:
        errors.append('Recovery full was not committed in CSV')
    if case['exit_code']==0:
        if not rows or rows[-1]['frame_id']!=case['expected_frames']-1:
            errors.append('Successful process did not finish the source sequence')
        if not static_rows or not resumed_window:
            errors.append('Successful process lacks static/resume coverage')
    fault_rx = b''
    if first_fault:
        prior_discard = max((e['line'] for e in trace if e['kind']=='F' and e['line']<first_fault['line']), default=0)
        fault_rx = b''.join(bytes.fromhex(e['data']) for e in trace
                            if e['kind']=='R' and prior_discard<e['line']<first_fault['line'])
        if len(BANNER.findall(fault_rx)) != first_fault['value1']:
            errors.append('Cross-read banner count inconsistent with failed ACK RX')
    summary = dict(trace_validation_errors=errors, trace_events=len(trace),
                   initial_handshake_verified='MSN handshake verified' in text,
                   static_csv_rows=len(static_rows), static_tx_events=sum(e['kind']=='T' and case['warm_frames']<=e['frame']<case['resume_frame'] for e in trace),
                   static_rx_events=sum(e['kind']=='R' and case['warm_frames']<=e['frame']<case['resume_frame'] for e in trace),
                   completed_csv_rows=len(rows), completed_sent_rows=sum(r['update_kind']!='unchanged' for r in rows),
                   last_completed_frame_id=rows[-1]['frame_id'] if rows else None,
                   last_observed_dropped_total=rows[-1]['dropped_total'] if rows else None,
                   first_fault=first_fault, failure_rx_bytes=len(fault_rx),
                   failure_rx_banners=len(BANNER.findall(fault_rx)),
                   failure_rx_contains_ack_pattern=b'\x02\x03' in fault_rx,
                   failure_rx_contains_lcd_command_echo=b'\x02\x03\x07\x00\x00\x00' in fault_rx,
                   resumed_window=resumed_window, last_payload_before_resume=last_payload,
                   actual_no_tx_gap_ms=(resumed_window['t_ms']-last_payload['t_ms']) if resumed_window and last_payload else None,
                   recovery_attempts=attempts, recovery_markers=markers,
                   recovered_full_committed=len(recovery_full_rows),
                   resumed_partial_rows=sum(r['frame_id']>=case['resume_frame'] and r['update_kind'] not in ('full','unchanged') for r in rows))
    if 'Operation not permitted (errno 1)' in text:
        stage='sandbox_open_denied'
    elif not summary['initial_handshake_verified']:
        stage='handshake_' + (first_fault['phase'] if first_fault else 'unknown')
    elif attempts:
        stage='recovered_original_failure' if 'recovery_full_ok' in phases and case['exit_code']==0 else 'recovery_failed'
    elif first_fault:
        stage=first_fault['phase']
    elif case['status']=='passed':
        stage='completed_without_fault'
    else:
        stage='other_failure'
    case['stage']=stage
    case['summary']=summary
    case['diagnostic_evidence_status']='valid' if not errors else 'invalid'
    if errors and case['status']=='passed':
        case['status']='failed';case['error']='Trace validation failed: '+errors[0]
    # An opt-in recovery pass never erases the preserved original protocol fault.
    case['original_protocol_failure_observed']=first_fault is not None
    (Path(case['log_path']).with_suffix('.evidence.json')).write_text(json.dumps(summary, indent=2)+'\n')


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--case', choices=[c['name'] for c in cases()])
    parser.add_argument('--label', default='initial')
    args=parser.parse_args(argv)
    if not re.fullmatch('[a-zA-Z0-9_]+',args.label):
        parser.error('label must be alphanumeric/underscore')
    metadata=json.loads((TOOLS/'build.json').read_text())
    mismatches=[name for name, digest in metadata['sha256'].items() if runner.sha256_file(ROOT/name)!=digest]
    if mismatches or runner.sha256_file(ROOT/'msu2_cpp_v0_2/build/msu2')!=ORIGINAL_SHA:
        parser.error('Build/original provenance changed; no device opened: '+str(mismatches))
    binary=TOOLS/'msu2_static_transition'
    binary_sha=runner.sha256_file(binary)
    directory=runner.create_run_directory(ROOT/'results/macos/static_transition')
    selected=[c for c in cases() if not args.case or c['name']==args.case]
    for case in selected:
        stem=case['name']
        case.update(stem=stem,status='planned',exit_code=None,binary_sha256_expected=binary_sha,
                    csv_path=str(directory/(stem+'.csv')),log_path=str(directory/(stem+'.log')))
        case['command']=runner.build_command(binary,PORT,case,Path(case['csv_path']))
        case['command']+=['--diag-static-ms',str(case['static_ms']),'--diag-recovery',str(int(case['recovery']))]
    manifest=dict(schema_version=1,suite='static_transition',label=args.label,
                  status='dry_run' if args.dry_run else 'running',started_at=runner.utc_now(),
                  binary=dict(path=str(binary),sha256=binary_sha),original_binary_sha256=ORIGINAL_SHA,
                  build_metadata=metadata,build_metadata_sha256=runner.sha256_file(TOOLS/'build.json'),cases=selected,
                  visual_verification='not_observed',
                  design='1s moving, 3/6s identical capture, 4s moving; same algorithm; 3s inter-case; no automatic protocol retries; only opt-in one same-fd strict handshake+invalidated/full recovery')
    manifest_path=directory/'manifest.json'
    runner.save_manifest(manifest_path,manifest)
    print('STATIC_TRANSITION_RESULTS='+str(directory),flush=True)
    if args.dry_run:
        for case in selected: print(shlex.join(case['command']))
        return 0
    aborted=False
    try:
        for index,case in enumerate(selected):
            if index: time.sleep(3)
            print('START '+case['name'],flush=True)
            # Retain the shared lease through process cleanup and independently
            # check both aliases immediately before every launch.
            while True:
                try:
                    with SerialGuard(PORT) as lease:
                        case['serial_guard']=lease.evidence
                        runner._execute_case_unlocked(case)
                    break
                except DeviceBusy as error:
                    case.setdefault('busy_waits',[]).append(dict(at=runner.utc_now(),reason=str(error)))
                    runner.save_manifest(manifest_path,manifest)
                    print('WAIT natural device-owner completion: '+str(error),flush=True)
                    time.sleep(5)
            summarize(case)
            runner.save_manifest(manifest_path,manifest)
            print('END {} stage={} exit={} rows={} gap_ms={} recovery={}'.format(
                case['name'],case['stage'],case['exit_code'],case['summary']['completed_csv_rows'],
                case['summary']['actual_no_tx_gap_ms'],case['summary']['recovery_attempts']),flush=True)
            if case['stage']=='sandbox_open_denied' or case['diagnostic_evidence_status']=='invalid':
                aborted=True;break
    except (OSError,RuntimeError,ValueError,KeyboardInterrupt) as error:
        aborted=True;manifest['stop_reason']=str(error) or 'Interrupted'
        print('STOP '+manifest['stop_reason'],flush=True)
    finally:
        manifest['status']='aborted' if aborted else 'completed'
        manifest['completed_at']=runner.utc_now()
        for case in selected:
            if case['status']=='planned': case['status']='not_run'
        manifest['original_binary_sha256_after']=runner.sha256_file(ROOT/'msu2_cpp_v0_2/build/msu2')
        runner.save_manifest(manifest_path,manifest)
    return 1 if aborted or any(c['status']!='passed' for c in selected) else 0


if __name__=='__main__':
    sys.exit(main())
