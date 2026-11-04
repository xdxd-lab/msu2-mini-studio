"""Sweep orchestration only, using fake failed children; no device access."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / 'scripts/macos'
with mock.patch.object(sys, 'path', [str(SCRIPTS)] + sys.path):
    spec = importlib.util.spec_from_file_location('fps_sweep', SCRIPTS / 'run_fps_sweep.py')
    sweep = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sweep)


class SweepTests(unittest.TestCase):
    def run_fake(self, callback, args):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(sweep.runner, 'PROJECT_ROOT', Path(directory)), mock.patch.object(sweep.runner, 'sha256_file', return_value=sweep.ORIGINAL_SHA):
                with mock.patch.object(sweep.runner, 'execute_case', side_effect=callback) as launch:
                    with contextlib.redirect_stdout(io.StringIO()):
                        with mock.patch.object(sweep.time, 'sleep'):
                            code = sweep.main(args)
                data = json.loads(next(Path(directory).rglob('manifest.json')).read_text())
                return code, data, launch.call_count

    def test_ack_failure_does_not_skip_higher_fps_or_retry(self):
        seen = []
        def fail(case):
            seen.append(case['fps'])
            case.update(status='failed', exit_code=1)
            Path(case['log_path']).write_text('MSN handshake verified\nerror: response timeout\n')
            return False
        code, data, count = self.run_fake(fail, ['--fps', '45', '46', '47'])
        self.assertEqual((code, count, seen), (1, 3, [45, 46, 47]))
        self.assertEqual(data['status'], 'completed')
        self.assertEqual([c['stage'] for c in data['cases']], ['lcd_ack_timeout'] * 3)
        self.assertEqual(data['cases'][1]['previous_case_stage'], 'lcd_ack_timeout')

    def test_busy_guard_aborts_without_advancing(self):
        def busy(case):
            raise RuntimeError('Another test holds the shared device lock')
        code, data, count = self.run_fake(busy, ['--fps', '45', '46'])
        self.assertEqual((code, count, data['status']), (1, 1, 'aborted'))
        self.assertEqual([c['status'] for c in data['cases']], ['not_run'] * 2)

    def test_valid_recording_cannot_override_failed_binary_acceptance(self):
        def rejected(case):
            case.update(status='failed', exit_code=0, error='binary changed during the run')
        with mock.patch.object(sweep.analyzer, 'analyze_case', side_effect=lambda *args: {
                'status': 'PASS', 'errors': [], 'warnings': []}):
            code, data, count = self.run_fake(rejected, ['--fps', '45'])
        self.assertEqual((code, count), (1, 1))
        analysis = data['cases'][0]['analysis']
        self.assertEqual(analysis['recording_status'], 'PASS')
        self.assertEqual(analysis['acceptance_status'], 'FAIL')
        self.assertEqual(analysis['status'], 'FAIL')
        self.assertIn('binary changed', analysis['errors'][0])

    def test_dry_run_lists_every_integer_fps_and_never_launches(self):
        code, data, count = self.run_fake(lambda case: self.fail('opened device'), ['--dry-run'])
        self.assertEqual((code, count), (0, 0))
        self.assertEqual([c['fps'] for c in data['cases']], list(range(45, 61)))
        self.assertEqual([c['expected_frames'] for c in data['cases']], [f * 30 for f in range(45, 61)])


if __name__ == '__main__':
    unittest.main()
