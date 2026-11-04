"""Offline acceptance and cleanup tests; no serial device is opened."""
import contextlib
import csv
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS=Path(__file__).resolve().parents[2]/'scripts/macos'
with mock.patch.object(sys,'path',[str(SCRIPTS)]+sys.path):
    spec=importlib.util.spec_from_file_location('idle_ack_suite',SCRIPTS/'run_idle_ack_suite.py')
    suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)

class IdleSuiteTests(unittest.TestCase):
    def fixture(self,directory):
        c=dict(csv_path=str(Path(directory)/'probe.csv'),log_path=str(Path(directory)/'probe.log'),
               order='ascending',chunk=60,status='process_completed')
        rows=[dict(index=i,label='test',idle_requested_ms=n,idle_actual_ms=n+0.1,status='completed',
                   window_ack_ms=0.3,payload_write_ms=36,send_host_ms=36.5,wire_bytes=32970,write_calls=551,
                   failure_phase='') for i,n in enumerate([0,0,5000,0,30000,0,60000,0])]
        Path(c['log_path']).write_text('MSN handshake verified\nCOMPLETE sends=8\n')
        return c,rows
    def write_rows(self,case,rows):
        with Path(case['csv_path']).open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=rows[0].keys());writer.writeheader();writer.writerows(rows)
    def test_eight_identical_full_sends_can_pass(self):
        with tempfile.TemporaryDirectory() as d:
            c,rows=self.fixture(d);self.write_rows(c,rows)
            self.assertTrue(suite.validate_probe(c))
    def test_missing_failed_or_corrupt_send_cannot_pass(self):
        for change in ('missing','failed','short_idle','wrong_bytes','wrong_chunk_count'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as d:
                c,rows=self.fixture(d)
                if change=='missing': rows.pop()
                elif change=='failed': rows[2]['status']='failed'
                elif change=='short_idle': rows[2]['idle_actual_ms']=4999
                elif change=='wrong_bytes': rows[2]['wire_bytes']=1218
                else: rows[2]['write_calls']=10
                self.write_rows(c,rows);self.assertFalse(suite.validate_probe(c))
    def test_oserror_child_is_reaped_inside_serial_lease(self):
        with tempfile.TemporaryDirectory() as d:
            child=mock.Mock(returncode=None);child.poll.return_value=None
            child.communicate.side_effect=OSError('fake wait error')
            def reap(): child.returncode=-9
            child.wait.side_effect=reap
            c=dict(command=['fake'],log_path=str(Path(d)/'log'),binary_sha256_expected='sha')
            @contextlib.contextmanager
            def lease(port):
                yield mock.Mock(evidence={'mock':True})
                self.assertIsNotNone(child.returncode,'released lock before child exit')
            with mock.patch.object(suite,'SerialGuard',side_effect=lease):
                with mock.patch.object(suite.runner,'sha256_file',return_value='sha'):
                    with mock.patch.object(suite.subprocess,'Popen',return_value=child):
                        self.assertFalse(suite.execute_probe(c))
            self.assertEqual(c['exit_code'],-9);self.assertEqual(c['status'],'failed')
            child.kill.assert_called_once();child.wait.assert_called_once()

if __name__=='__main__': unittest.main()
