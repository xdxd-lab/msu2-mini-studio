"""Synthetic recording tests; no external/private hardware logs are committed."""

import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "macos" / "analyze_results.py"
SPEC = importlib.util.spec_from_file_location("analyze_results", SCRIPT)
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


class RecordingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.csv_path = self.directory / "synthetic.csv"
        self.log_path = self.directory / "synthetic.log"

    def recording(self, kinds=("full", "bounding_box", "unchanged", "full"), utf16=False):
        rows = []
        unchanged = 0
        for number, kind in enumerate(kinds):
            is_unchanged = kind == "unchanged"
            unchanged += int(is_unchanged)
            full = kind == "full"
            row = dict.fromkeys(analyzer.CSV_FIELDS, 0)
            row.update(frame_id=number, capture_ms=0.1, convert_ms=0.1, encode_ms=0.2,
                       window_ack_ms=0 if is_unchanged else 0.3,
                       payload_write_ms=0 if is_unchanged else 1,
                       host_latency_ms=0.2 if is_unchanged else (40 if number == 0 else 20 if full else 2),
                       wire_bytes=0 if is_unchanged else 100 if full else 20,
                       write_calls=0 if is_unchanged else 2,
                       changed_pixels=0 if is_unchanged else 12800 if number == 0 else 48,
                       transmitted_pixels=0 if is_unchanged else 12800 if full else 312,
                       rectangles=0 if is_unchanged else 1, update_kind=kind, unchanged_total=unchanged)
            rows.append(row)
        self.write_rows(rows)
        count, sent = len(rows), len(rows) - unchanged
        full_count = kinds.count("full")
        contents = (
            "MSN handshake verified; firmware_banner=1; configured_model=MINI_160x80 (dimensions not queried)\n"
            "update=diff encoding=pair target_fps=30 keyframe_ms=2000\n"
            "captured={count} host_frames_sent={sent} waiting_frames_replaced=0 bytes={wire} "
            "elapsed_s=1.0000 host_send_fps={sent}.0000 processed={count} unchanged_frames={unchanged} "
            "full_frames={full} partial_frames={partial} rectangles_sent={sent}; "
            "this is NOT measured LCD refresh FPS\n"
        ).format(count=count, sent=sent, wire=sum(row["wire_bytes"] for row in rows),
                 unchanged=unchanged, full=full_count, partial=sent-full_count)
        self.log_path.write_text(contents, encoding="utf-16" if utf16 else "utf-8")
        return rows

    def write_rows(self, rows):
        with self.csv_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=analyzer.CSV_FIELDS)
            writer.writeheader()
            writer.writerows(rows)

    def test_utf16_and_periodic_full_remains_in_steady_sent_statistics(self):
        self.recording(utf16=True)
        result = analyzer.analyze_case(self.csv_path, self.log_path)
        self.assertEqual(result["status"], "PASS", result["errors"])
        self.assertEqual(result["csv"]["processed_rows"], 4)
        self.assertEqual(result["csv"]["sent_rows"], 3)
        self.assertEqual(result["csv"]["unchanged_rows"], 1)
        self.assertEqual(result["statistics"]["initial_full"]["count"], 1)
        steady = result["statistics"]["steady_sent_including_later_full"]
        self.assertEqual(steady["count"], 2)
        self.assertEqual(steady["metrics"]["host_latency_ms"]["mean"], 11)
        self.assertAlmostEqual(steady["metrics"]["host_latency_ms"]["p95"], 19.1)
        self.assertEqual(result["statistics"]["later_full"]["count"], 1)

    def test_static_unchanged_rows_do_not_count_as_sent_frames(self):
        self.recording(("full", "unchanged", "unchanged"))
        result = analyzer.analyze_case(self.csv_path, self.log_path)
        self.assertEqual(result["status"], "PASS", result["errors"])
        self.assertEqual(result["csv"]["sent_rows"], 1)
        self.assertEqual(result["csv"]["unchanged_rows"], 2)
        self.assertEqual(result["csv"]["wire_bytes"], 100)
        self.assertEqual(result["statistics"]["steady_sent_including_later_full"]["count"], 0)

    def test_truncation_or_inconsistent_final_summary_cannot_pass(self):
        self.recording()
        original = self.log_path.read_text()
        for broken in ("MSN handshake verified\ncaptured=1 host_frames_sent=1\n",
                       original.replace("bytes=220", "bytes=221"),
                       original.replace("rectangles_sent=3", "rectangles_sent=4"),
                       original.replace("captured=4", "captured=5")):
            with self.subTest(log=broken):
                self.log_path.write_text(broken)
                result = analyzer.analyze_case(self.csv_path, self.log_path)
                self.assertEqual(result["status"], "FAIL")
                self.assertTrue(result["errors"])

    def test_invalid_rows_cannot_pass(self):
        rows = self.recording()
        for index, key, value in ((1, "frame_id", 0), (1, "host_latency_ms", "nan"),
                                  (2, "wire_bytes", 20), (2, "unchanged_total", 0)):
            with self.subTest(key=key):
                changed = [dict(row) for row in rows]
                changed[index][key] = value
                self.write_rows(changed)
                result = analyzer.analyze_case(self.csv_path, self.log_path)
                self.assertEqual(result["status"], "FAIL")

    def test_timeout_preserves_completed_row_statistics_without_final_totals(self):
        self.recording()
        self.log_path.write_text("MSN handshake verified\nError: response timeout\n")
        manifest_path = self.directory / "manifest.json"
        manifest_path.write_text(json.dumps({"status": "failed", "cases": [{
            "csv_path": str(self.csv_path), "log_path": str(self.log_path),
            "status": "failed", "exit_code": 1, "expected_frames": 600,
        }]}))
        result = analyzer.analyze_case(self.csv_path, self.log_path, [manifest_path])
        self.assertEqual(result["status"], "FAIL")
        self.assertNotIn("final_log", result)
        self.assertNotIn("calculated_host_send_fps", result)
        self.assertEqual(result["csv"]["processed_rows"], 4)
        self.assertEqual(result["csv"]["last_frame_id"], 3)
        self.assertEqual(result["csv"]["last_observed_dropped_total"], 0)
        self.assertEqual(result["statistics"]["partial_sent"]["count"], 1)
        self.assertEqual(result["manifest"]["case"]["exit_code"], 1)
        rendered = analyzer.report_table({"cases": [result], "status": "FAIL"})
        self.assertIn("Incomplete run: CSV contains 4 completed updates", rendered)
        self.assertIn("no final host FPS", rendered)

    def test_cli_checks_manifest_expected_count_and_exit_status(self):
        self.recording()
        manifest = {"schema_version": 1, "status": "completed", "cases": [{
            "name": "synthetic", "csv_path": str(self.csv_path), "log_path": str(self.log_path),
            "expected_frames": 4, "status": "passed", "exit_code": 0,
        }]}
        manifest_path = self.directory / "manifest.json"
        output = self.directory / "analysis.json"
        for key, value, expected_exit in (("expected_frames", 4, 0), ("expected_frames", 5, 1),
                                           ("exit_code", 1, 1)):
            manifest["cases"][0]["expected_frames"] = 4
            manifest["cases"][0]["exit_code"] = 0
            manifest["cases"][0][key] = value
            manifest_path.write_text(json.dumps(manifest))
            with self.subTest(key=key, value=value), contextlib.redirect_stdout(io.StringIO()):
                exit_code = analyzer.main([str(self.directory), "--output", str(output)])
                self.assertEqual(exit_code, expected_exit)
                report = json.loads(output.read_text())
                self.assertEqual(report["status"], "PASS" if expected_exit == 0 else "FAIL")

    def test_no_results_or_orphaned_log_returns_failure(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(analyzer.main([str(self.directory)]), 1)
        self.log_path.write_text("partial log")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(analyzer.main([str(self.directory)]), 1)

    def test_manifest_case_with_both_recordings_missing_is_not_ignored(self):
        self.recording()
        manifest = {"schema_version": 1, "status": "completed", "cases": [{
            "name": "synthetic", "csv_path": str(self.csv_path), "log_path": str(self.log_path),
            "expected_frames": 4, "status": "passed", "exit_code": 0,
        }, {
            "name": "missing", "csv_path": str(self.directory / "missing.csv"),
            "log_path": str(self.directory / "missing.log"), "expected_frames": 4,
            "status": "passed", "exit_code": 0,
        }]}
        (self.directory / "manifest.json").write_text(json.dumps(manifest))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(analyzer.main([str(self.directory)]), 1)


if __name__ == "__main__":
    unittest.main()
