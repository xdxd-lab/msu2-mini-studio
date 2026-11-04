"""Runner unit tests: mocks and temporary files only; never touch USB."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


RUNNER_PATH = Path(__file__).resolve().parents[2] / "scripts" / "macos" / "run_hardware.py"
SPEC = importlib.util.spec_from_file_location("run_hardware", RUNNER_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class HardwareRunnerTests(unittest.TestCase):
    def setUp(self):
        # Runner tests never inspect real device ownership or open a serial port.
        patcher = mock.patch.object(runner, "guard_command", side_effect=lambda command: contextlib.nullcontext())
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_chunk_and_single_case_controls_for_manual_reconnection(self):
        with tempfile.TemporaryDirectory() as directory:
            args = ["--port", "/dev/NEVER_OPEN", "--dry-run", "--results-root", directory,
                    "--suite", "baseline", "--case", "moving_diff_30", "--repeats", "1",
                    "--chunk", "60"]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(args), 0)
            data = json.loads(next(Path(directory).glob("*/hardware/manifest.json")).read_text())
            self.assertEqual(len(data["cases"]), 1)
            case = data["cases"][0]
            self.assertEqual(case["name"], "moving_diff_30")
            self.assertEqual(case["command"][case["command"].index("--chunk") + 1], "60")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.main(args + ["--case", "missing"]), 2)
                with self.assertRaises(SystemExit):
                    runner.parse_args(args + ["--chunk", "64"])

    def test_commands_omit_keyframe_flag_for_full_and_alternate_baseline_order(self):
        cases = runner.build_cases("baseline", repeats=3, seconds=300)
        self.assertEqual(len(cases), 12)
        self.assertEqual([case["name"] for case in cases[4:8]],
                         list(reversed([case["name"] for case in cases[:4]])))
        self.assertEqual([case["expected_frames"] for case in cases[:4]], [300, 300, 450, 600])
        self.assertEqual([case["fps"] for case in cases[:4]], [30, 30, 45, 60])
        for case in cases:
            command = runner.build_command(Path("/tmp/msu2"), "/dev/TEST", case, Path("/tmp/x.csv"))
            self.assertEqual("--keyframe-ms" in command, case["update"] == "diff")
            self.assertEqual(command[command.index("--port") + 1], "/dev/TEST")

    def test_dry_run_never_launches_binary_and_preserves_unique_results(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(runner.subprocess, "Popen") as launch:
            args = ["--port", "/dev/NEVER_OPEN", "--dry-run", "--results-root", directory,
                    "--binary", str(Path(directory) / "missing-binary")]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(args), 0)
                self.assertEqual(runner.main(args), 0)
            launch.assert_not_called()
            manifests = list(Path(directory).glob("*/hardware/manifest.json"))
            self.assertEqual(len(manifests), 2)
            for path in manifests:
                data = json.loads(path.read_text())
                self.assertEqual(data["status"], "dry_run")
                self.assertEqual(data["visual_verification"], "not_observed")
                self.assertEqual([case["expected_frames"] for case in data["cases"]], [45, 90])
                self.assertIn("+0800_", path.parent.parent.name)
                self.assertTrue((path.parent / "visual_observations.txt").is_file())
                self.assertFalse(list(path.parent.glob("*.csv")))

    def test_failure_stops_suite_and_records_unexecuted_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "msu2"
            binary.write_text("test executable\n")
            binary.chmod(0o700)
            child = mock.Mock(returncode=7)
            child.communicate.return_value = (None, None)
            with mock.patch.object(runner.subprocess, "Popen", return_value=child) as launch:
                with contextlib.redirect_stdout(io.StringIO()):
                    result = runner.main(["--port", "/dev/NEVER_OPEN", "--binary", str(binary),
                                          "--results-root", directory, "--settle-seconds", "0"])
            self.assertEqual(result, 1)
            self.assertEqual(launch.call_count, 1)
            data = json.loads(next(Path(directory).glob("*/hardware/manifest.json")).read_text())
            self.assertEqual(data["status"], "failed")
            self.assertEqual(data["cases"][0]["exit_code"], 7)
            self.assertEqual(data["cases"][1]["status"], "not_run")
            self.assertEqual(len(data["binary"]["sha256"]), 64)

    def test_timeout_and_interrupt_preserve_failure_status(self):
        for interruption in (subprocess.TimeoutExpired(["fake"], 1), KeyboardInterrupt()):
            with self.subTest(interruption=type(interruption).__name__), tempfile.TemporaryDirectory() as directory:
                case = runner.make_case("fake", "bars", "full", 15, 45)
                case.update(command=["fake"], log_path=str(Path(directory) / "fake.log"),
                            csv_path=str(Path(directory) / "fake.csv"))
                child = mock.Mock(returncode=-9)
                child.poll.return_value = None
                child.communicate.side_effect = [interruption, (None, None)]
                with mock.patch.object(runner.subprocess, "Popen", return_value=child):
                    if isinstance(interruption, KeyboardInterrupt):
                        with self.assertRaises(KeyboardInterrupt):
                            runner.execute_case(case)
                        child.terminate.assert_called_once()
                        self.assertEqual(case["status"], "interrupted")
                    else:
                        self.assertFalse(runner.execute_case(case))
                        child.kill.assert_called_once()
                        self.assertEqual(case["status"], "failed")
                self.assertEqual(case["expected_frames"], 45)
                self.assertIn("completed_at", case)
                self.assertTrue(Path(case["log_path"]).is_file())

    def test_zero_exit_without_csv_does_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            case = runner.make_case("fake", "bars", "full", 15, 45)
            case.update(command=["fake"], log_path=str(Path(directory) / "fake.log"),
                        csv_path=str(Path(directory) / "fake.csv"))
            child = mock.Mock(returncode=0)
            with mock.patch.object(runner.subprocess, "Popen", return_value=child):
                self.assertFalse(runner.execute_case(case))
            self.assertEqual(case["status"], "failed")
            self.assertIn("CSV", case["error"])

    def test_communicate_oserror_reaps_child_before_releasing_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            case = runner.make_case("fake", "bars", "full", 15, 45)
            case.update(command=["fake"], log_path=str(Path(directory) / "fake.log"),
                        csv_path=str(Path(directory) / "fake.csv"))
            child = mock.Mock(returncode=None)
            child.poll.return_value = None
            child.communicate.side_effect = OSError("communicate failed")
            def reaped():
                child.returncode = -9
                return -9
            child.wait.side_effect = reaped
            @contextlib.contextmanager
            def lease(command):
                yield None
                self.assertIsNotNone(child.returncode, "guard released with live child")
            with mock.patch.object(runner, "guard_command", side_effect=lease):
                with mock.patch.object(runner.subprocess, "Popen", return_value=child):
                    self.assertFalse(runner.execute_case(case))
            child.terminate.assert_called_once()
            child.kill.assert_called_once()
            child.wait.assert_called_once()
            self.assertEqual(case['exit_code'], -9)
            self.assertEqual(case['status'], 'failed')
            self.assertIn('communicate failed', case['error'])

    def test_changed_binary_cannot_launch_or_pass(self):
        for when in ("before", "during"):
            with self.subTest(when=when), tempfile.TemporaryDirectory() as directory:
                binary = Path(directory) / "fake"
                binary.write_text("original")
                case = runner.make_case("fake", "bars", "full", 15, 45)
                case.update(command=[str(binary)], log_path=str(Path(directory) / "fake.log"),
                            csv_path=str(Path(directory) / "fake.csv"),
                            binary_sha256_expected=runner.sha256_file(binary))
                if when == "before":
                    binary.write_text("changed")
                child = mock.Mock(returncode=0)
                child.communicate.side_effect = lambda **kwargs: binary.write_text("changed")
                with mock.patch.object(runner.subprocess, "Popen", return_value=child) as launch:
                    self.assertFalse(runner.execute_case(case))
                self.assertEqual(launch.call_count, 0 if when == "before" else 1)
                self.assertEqual(case["status"], "failed")
                self.assertIn("binary changed", case["error"])
                self.assertIn("not launched" if when == "before" else "during the run", case["error"])

    def test_zero_exit_with_invalid_or_short_recording_stops_suite(self):
        results = [
            {"status": "FAIL", "errors": ["Expected exactly one final log summary"],
             "final_log": {}},
            {"status": "PASS", "errors": [], "final_log": {"captured": 44}},
        ]
        for validation in results:
            with self.subTest(validation=validation), tempfile.TemporaryDirectory() as directory:
                binary = Path(directory) / "msu2"
                binary.write_text("test executable\n")
                binary.chmod(0o700)

                def launch_child(command, **kwargs):
                    Path(command[command.index("--csv") + 1]).write_text("nonempty mock CSV\n")
                    return mock.Mock(returncode=0)

                with mock.patch.object(runner.subprocess, "Popen", side_effect=launch_child) as launch:
                    with mock.patch.object(runner, "analyze_recording", return_value=validation) as validate:
                        with contextlib.redirect_stdout(io.StringIO()):
                            result = runner.main(["--port", "/dev/NEVER_OPEN", "--binary", str(binary),
                                                  "--results-root", directory, "--settle-seconds", "0"])
                self.assertEqual(result, 1)
                self.assertEqual(launch.call_count, 1)
                self.assertEqual(validate.call_count, 1)
                data = json.loads(next(Path(directory).glob("*/hardware/manifest.json")).read_text())
                self.assertEqual(data["status"], "failed")
                self.assertEqual(data["cases"][0]["exit_code"], 0)
                self.assertEqual(data["cases"][0]["status"], "failed")
                self.assertTrue(data["cases"][0]["validation_errors"])
                self.assertEqual(data["cases"][1]["status"], "not_run")

    def test_complete_recording_can_pass_and_validator_ignores_running_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            case = runner.make_case("fake", "bars", "full", 15, 45)
            case.update(command=["fake"], log_path=str(Path(directory) / "fake.log"),
                        csv_path=str(Path(directory) / "fake.csv"))
            Path(case["csv_path"]).write_text("nonempty mock CSV\n")
            validator = mock.Mock()
            validator.analyze_case.return_value = {"status": "PASS", "errors": [],
                                                  "final_log": {"captured": 45}}
            child = mock.Mock(returncode=0)
            with mock.patch.object(runner.subprocess, "Popen", return_value=child):
                with mock.patch.object(runner, "_ANALYZER", validator):
                    self.assertTrue(runner.execute_case(case))
            validator.analyze_case.assert_called_once_with(case["csv_path"], case["log_path"],
                                                          manifest_paths=())
            self.assertEqual(case["status"], "passed")
            self.assertEqual(case["validation_errors"], [])

    def test_sibling_validator_import_rejects_malformed_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            case = runner.make_case("fake", "bars", "full", 15, 45)
            case.update(csv_path=str(Path(directory) / "fake.csv"),
                        log_path=str(Path(directory) / "fake.log"))
            Path(case["csv_path"]).write_text("bad,CSV\n1,2\n")
            Path(case["log_path"]).write_text("MSN handshake verified\n")
            with mock.patch.object(runner, "_ANALYZER", None):
                self.assertFalse(runner.validate_case(case))
            self.assertEqual(case["status"], "failed")
            self.assertTrue(case["validation_errors"])


if __name__ == "__main__":
    unittest.main()
