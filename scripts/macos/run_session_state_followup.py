#!/usr/bin/env python3
"""One separately recorded passive-idle probe; no automatic retries."""
import argparse
import json
import shlex
import subprocess
import sys

import run_session_state_suite as suite

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    if not args.label.replace('_', '').isalnum(): parser.error('Invalid label')
    if suite.runner.sha256_file(suite.ROOT / 'msu2_cpp_v0_2/build/msu2') != suite.ORIGINAL_SHA:
        parser.error('Original executable changed; no device opened')
    build = json.loads((suite.PROBE.parent / 'build.json').read_text())
    for path, sha in build['sha256'].items():
        if suite.runner.sha256_file(suite.ROOT / path) != sha: parser.error('Diagnostic build mismatch: ' + path)
    directory = suite.runner.create_run_directory(suite.ROOT / 'results/macos/session_state_followup')
    case = suite.make_cases(directory, build)[0]
    manifest = dict(schema_version=1, suite='session_state_followup', label=args.label,
                    status='dry_run' if args.dry_run else 'running', started_at=suite.runner.utc_now(),
                    cases=[case], diagnostic_build=build, original_binary_sha256=suite.ORIGINAL_SHA,
                    visual_verification='not_observed',
                    method='One separate passive_idle launch after the prior suite has naturally finished; same unchanged probe; no automatic retries; not a replacement for earlier handshake failure.')
    path = directory / 'manifest.json'
    suite.runner.save_manifest(path, manifest)
    print('FOLLOWUP_RESULTS=' + str(directory), flush=True)
    if args.dry_run:
        print(shlex.join(case['command'])); return 0
    aborted = False
    try:
        suite.execute_probe(case)
        suite.summarize_probe(case)
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        aborted = True; case.update(status='not_run' if case['status'] == 'planned' else 'failed', error=str(error) or 'Interrupted')
        manifest['stop_reason'] = str(error) or 'Interrupted'
    finally:
        manifest.update(status='aborted' if aborted else 'completed', completed_at=suite.runner.utc_now())
        suite.runner.save_manifest(path, manifest)
    print('END passive_idle status=' + case['status'] + ' stage=' + str(case.get('stage')), flush=True)
    return 0 if case['status'] == 'passed' else 1

if __name__ == '__main__':
    sys.exit(main())
