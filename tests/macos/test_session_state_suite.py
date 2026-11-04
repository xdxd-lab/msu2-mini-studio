"""Evidence validation and lease cleanup tests; never open a serial device."""
import contextlib
import csv
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / 'scripts/macos'
with mock.patch.object(sys, 'path', [str(SCRIPTS)] + sys.path):
    spec = importlib.util.spec_from_file_location('session_state_suite', SCRIPTS / 'run_session_state_suite.py')
    suite = importlib.util.module_from_spec(spec); spec.loader.exec_module(suite)

FIELDS = 'index,operation,cycle,chunk,requested_ms,actual_ms,status,window_ack_ms,payload_write_ms,host_ms,wire_bytes,write_calls,rx_bytes,banners,first_banner_ms,failure_phase,error'.split(',')

class SessionStateTests(unittest.TestCase):
    def fixture(self, directory, recovered=False):
        case = dict(csv_path=str(Path(directory) / 'x.csv'), log_path=str(Path(directory) / 'x.log'),
                    scenario='silent_idle', exit_code=1 if recovered else 0,
                    binary_sha256_before='sha', binary_sha256_after='sha', binary_sha256_expected='sha')
        operations = [('initial_handshake', 0), ('initial_full', 0), ('silent_idle', 1), ('after_idle_full', 1)]
        if recovered:
            operations += [('recovery_handshake', 1), ('recovered_full_1', 1), ('recovered_full_2', 1)]
        rows = []
        for index, (operation, cycle) in enumerate(operations):
            row = {field: '' for field in FIELDS}
            row.update(index=index, operation=operation, cycle=cycle, chunk=60, requested_ms=0,
                       actual_ms=0, status='completed', host_ms=1)
            if operation == 'silent_idle': row.update(requested_ms=30000, actual_ms=30001)
            elif 'full' in operation:
                row.update(window_ack_ms=.3, payload_write_ms=36, host_ms=36.5, wire_bytes=32970, write_calls=551)
            if recovered and operation == 'after_idle_full':
                row.update(status='failed', host_ms=2001, wire_bytes='', write_calls='',
                           window_ack_ms='', payload_write_ms='', failure_phase='window_ack_wait', error='response timeout')
            rows.append(row)
        Path(case['log_path']).write_text('COMPLETE protocol_probe scenario=silent_idle; protocol_failure={}\n'.format(int(recovered)))
        return case, rows

    def write_rows(self, case, rows):
        with Path(case['csv_path']).open('w', newline='') as handle:
            writer = csv.DictWriter(handle, fieldnames=FIELDS); writer.writeheader(); writer.writerows(rows)

    def test_success_and_recovered_failure_are_distinguished(self):
        for recovered in (False, True):
            with self.subTest(recovered=recovered), tempfile.TemporaryDirectory() as directory:
                case, rows = self.fixture(directory, recovered); self.write_rows(case, rows)
                suite.summarize_probe(case)
                self.assertEqual(case['recording_status'], 'complete')
                self.assertEqual(case['status'], 'diagnostic_complete_with_failures' if recovered else 'passed')
                self.assertEqual(len(case['protocol_failures']), int(recovered))

    def test_missing_recovery_wrong_bytes_or_exit_cannot_pass(self):
        for corruption in ('missing_recovery', 'wrong_bytes', 'wrong_exit', 'short_idle', 'changed_hash', 'failed_recovery'):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as directory:
                case, rows = self.fixture(directory, True)
                if corruption == 'missing_recovery': rows.pop()
                elif corruption == 'wrong_bytes': rows[1]['wire_bytes'] = 1218
                elif corruption == 'wrong_exit': case['exit_code'] = 0
                elif corruption == 'short_idle': rows[2]['actual_ms'] = 29999
                elif corruption == 'changed_hash': case['binary_sha256_after'] = 'different'
                else: rows[-1].update(status='failed', failure_phase='window_ack_wait')
                self.write_rows(case, rows); suite.summarize_probe(case)
                self.assertEqual(case['status'], 'failed'); self.assertTrue(case['validation_errors'])

    def test_no_final_summary_cannot_claim_completed_plan(self):
        with tempfile.TemporaryDirectory() as directory:
            case, rows = self.fixture(directory); self.write_rows(case, rows)
            Path(case['log_path']).write_text('STOP protocol_probe scenario=silent_idle;\n')
            suite.summarize_probe(case)
            self.assertEqual(case['recording_status'], 'partial_or_invalid')

    def test_child_reaped_before_lease_release_on_launcher_failure(self):
        for failure in (OSError('communicate failed'), subprocess.TimeoutExpired(['fake'], 110)):
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as directory:
                child = mock.Mock(returncode=None); child.poll.return_value = None
                child.communicate.side_effect = [failure, (None, None)]
                def reap(process): process.returncode = -9
                @contextlib.contextmanager
                def lease(port):
                    yield mock.Mock(evidence={'mock': True})
                    self.assertIsNotNone(child.returncode, 'Live child when lease released')
                case = dict(command=['fake'], log_path=str(Path(directory) / 'x.log'), binary_sha256_expected='sha')
                with mock.patch.object(suite, 'SerialGuard', side_effect=lease), \
                     mock.patch.object(suite.runner, 'sha256_file', return_value='sha'), \
                     mock.patch.object(suite.runner, 'stop_process', side_effect=reap), \
                     mock.patch.object(suite.subprocess, 'Popen', return_value=child):
                    suite.execute_probe(case)
                self.assertEqual(case['exit_code'], -9); self.assertIn('launcher_error', case)

if __name__ == '__main__':
    unittest.main()
