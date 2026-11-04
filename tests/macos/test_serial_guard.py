"""Only temporary advisory locks and mocked owner checks; never open USB."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[2] / 'scripts/macos/serial_guard.py'
SPEC = importlib.util.spec_from_file_location('serial_guard', PATH)
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)


class GuardTests(unittest.TestCase):
    def test_aliases_share_one_lock_until_released(self):
        with tempfile.TemporaryDirectory() as directory:
            check = mock.Mock(return_value={'idle': True})
            with guard.SerialGuard('/dev/cu.TEST', directory, check):
                with self.assertRaises(guard.DeviceBusy):
                    with guard.SerialGuard('/dev/tty.TEST', directory, check):
                        self.fail('second test entered')
            with guard.SerialGuard('/dev/tty.TEST', directory, check):
                pass
            self.assertEqual(check.call_count, 2)

    def test_owner_check_failure_releases_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(guard.DeviceBusy):
                with guard.SerialGuard('/dev/cu.TEST', directory, mock.Mock(side_effect=guard.DeviceBusy('busy'))):
                    pass
            with guard.SerialGuard('/dev/cu.TEST', directory, lambda port: {'idle': True}):
                pass

    def test_busy_or_unverifiable_owner_check_fails_closed(self):
        for status, out, err in [(0, 'msu2 PID device', ''), (1, '', 'not permitted'), (2, '', '')]:
            with self.subTest(status=status, err=err), mock.patch.object(guard.Path, 'exists', return_value=True):
                with mock.patch.object(guard.subprocess, 'run', return_value=mock.Mock(returncode=status, stdout=out, stderr=err)):
                    with self.assertRaises(guard.DeviceBusy):
                        guard.check_idle('/dev/cu.TEST')

    def test_idle_owner_check_checks_both_aliases(self):
        with mock.patch.object(guard.Path, 'exists', return_value=True):
            with mock.patch.object(guard.subprocess, 'run', return_value=mock.Mock(returncode=1, stdout='', stderr='')) as run:
                result = guard.check_idle('/dev/cu.TEST')
        self.assertEqual(result['aliases_checked'], ['/dev/cu.TEST', '/dev/tty.TEST'])
        self.assertEqual(run.call_args.args[0][-2:], result['aliases_checked'])


if __name__ == '__main__':
    unittest.main()
