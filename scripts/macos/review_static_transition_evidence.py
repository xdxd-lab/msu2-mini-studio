#!/usr/bin/env python3
"""Read-only hardware evidence audit; writes its review JSON, never opens USB."""
import hashlib
import json
from pathlib import Path
import sys

import analyze_results as analyzer
from run_static_transition_suite import parse_trace

ROOT=Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def phase(frame,case):
    if frame<case['warm_frames']: return 'warmup'
    if frame<case['resume_frame']: return 'static'
    return 'resume'


def review(directory):
    manifest_path=directory/'manifest.json'
    manifest=json.loads(manifest_path.read_text())
    errors=[];cases=[];files={str(manifest_path.relative_to(ROOT)):sha(manifest_path)}
    for name,digest in manifest['build_metadata']['sha256'].items():
        if sha(ROOT/name)!=digest: errors.append('Build file mismatch: '+name)
    for case in manifest['cases']:
        if case['status']=='not_run': continue
        if case.get('stage')=='sandbox_open_denied':
            cases.append(dict(name=case['name'],hardware_started=False,reason='sandbox denied before serial open'))
            continue
        if not case.get('serial_guard') or len(case['serial_guard']['aliases_checked'])!=2 or case['serial_guard']['lsof_exit_code']!=1:
            errors.append('Invalid guard evidence: '+case['name'])
        if case.get('binary_sha256_before')!=manifest['binary']['sha256'] or case.get('binary_sha256_after')!=manifest['binary']['sha256']:
            errors.append('Executable hash mismatch: '+case['name'])
        log_path=Path(case['log_path']);csv_path=Path(case['csv_path'])
        files[str(log_path.relative_to(ROOT))]=sha(log_path)
        events=parse_trace(log_path.read_text())
        faults=[]
        for event in events:
            if event['kind']!='E': continue
            discard=max((e['line'] for e in events if e['kind']=='F' and e['line']<event['line']),default=0)
            rx=[e for e in events if e['kind']=='R' and discard<e['line']<event['line']]
            failed_window=next((e for e in reversed(events) if e['kind']=='W' and e['phase']=='window_write'
                               and e['line']<event['line']),None)
            initial_ok=any(e['kind']=='M' and e['phase']=='initial_handshake_ok' and e['line']<event['line'] for e in events)
            scope='initial_handshake' if not initial_ok else ('recovery_handshake' if event['phase'] in ('banner_wait','echo_write','echo_wait') else 'update')
            fault=dict(event,operation_scope=scope,source_phase=phase(event['frame'],case) if initial_ok else 'not_started',
                       error=bytes.fromhex(event['data']).decode('utf-8'),
                       rx_bytes=sum(e['size'] for e in rx),rx_event_lines=[e['line'] for e in rx if e['size']],
                       failed_window=failed_window,
                       failed_window_wait_ms=event['t_ms']-failed_window['t_ms'] if failed_window else None)
            faults.append(fault)
        rows=analyzer.read_csv(csv_path) if csv_path.exists() else []
        if csv_path.exists(): files[str(csv_path.relative_to(ROOT))]=sha(csv_path)
        updates=[e for e in events if e['kind']=='U']
        if len(rows)!=len(updates): errors.append('CSV/trace completed-update count mismatch: '+case['name'])
        for row,event in zip(rows,updates):
            if (row['frame_id'],row['update_kind'],row['wire_bytes'],row['write_calls'],row['rectangles'])!=(
                    event['frame'],event['phase'],event['value1'],event['value2'],event['patch_count']):
                errors.append('CSV/trace completed-update mismatch: '+case['name']+' frame '+str(row['frame_id']))
        markers=[e for e in events if e['kind']=='M' and e['phase'].startswith('recovery_')]
        beginnings=[e for e in markers if e['phase']=='recovery_begin']
        entry=dict(name=case['name'],hardware_started=True,exit_code=case['exit_code'],stage=case['stage'],
                   fps=case['fps'],static_ms=case['static_ms'],recovery_enabled=case['recovery'],
                   rows=len(rows),static_rows=sum(phase(r['frame_id'],case)=='static' for r in rows),
                   resumed_rows=sum(phase(r['frame_id'],case)=='resume' for r in rows),
                   faults=faults,recovery_markers=markers,
                   recovery_source_phases=[phase(e['frame'],case) for e in beginnings],
                   wire_success_total=sum(r['wire_bytes'] for r in rows),
                   missing_csv_ids_before_last_completed=sorted(set(range(rows[-1]['frame_id']+1))-{r['frame_id'] for r in rows}) if rows else [],
                   gap_ms=case['summary']['actual_no_tx_gap_ms'],
                   summary_validation_errors=case['summary']['trace_validation_errors'])
        if entry['summary_validation_errors']: errors.extend(entry['summary_validation_errors'])
        cases.append(entry)
        for suffix in ('.evidence.json',):
            path=log_path.with_suffix(suffix)
            if path.exists(): files[str(path.relative_to(ROOT))]=sha(path)
    original=ROOT/'MSU2_MINI_CPP_v0_2_20261010/msu2_cpp_v0_2'
    checked=0
    for line in (original/'CHECKSUMS.sha256').read_text().splitlines():
        expected,name=line.split(None,1);checked+=1
        if sha(original/name.lstrip('*'))!=expected: errors.append('Original checksum mismatch: '+name)
    artifact_index=directory/'artifact_sha256.json'
    artifact_count=0
    if artifact_index.exists():
        artifacts=json.loads(artifact_index.read_text())
        for name,digest in artifacts.items():
            artifact_count+=1
            if sha(ROOT/name)!=digest: errors.append('Operator artifact hash mismatch: '+name)
    for path in list(directory.glob('*'))+list(directory.parent.glob('*')):
        if path.is_file(): files[str(path.relative_to(ROOT))]=sha(path)
    return dict(manifest=str(manifest_path.relative_to(ROOT)),manifest_status=manifest['status'],
                original_checksums_checked=checked,operator_artifact_hashes_checked=artifact_count,errors=errors,cases=cases,
                source_evidence_sha256=files,primary_opened_serial=False)


def main():
    directories=[Path(p).resolve() for p in sys.argv[1:]]
    if not directories: raise SystemExit('Pass completed hardware directories explicitly')
    result=dict(reviews=[review(p) for p in directories])
    output=ROOT/'results/macos/static_transition/primary_review.json'
    output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'errors':[r['errors'] for r in result['reviews']],
                      'cases':[(c['name'],c.get('source_evidence_status',c.get('stage')))
                               for r in result['reviews'] for c in r['cases']]}))
    return int(any(r['errors'] for r in result['reviews']))


if __name__=='__main__': sys.exit(main())
