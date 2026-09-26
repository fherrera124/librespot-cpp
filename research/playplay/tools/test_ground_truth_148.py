#!/usr/bin/env python3
"""Offline regression tests for the active token148/v5 fixture and runner."""

import copy
import json
from pathlib import Path
import tempfile
import threading
import unittest

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from check_ground_truth_148 import DEFAULT, verify
from validate_windows_vm import CaptureMessages, assess_report, capture_assessment, make_script, vectors_for_scripts
from verify_token148_content import IV


class GroundTruth148Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(DEFAULT.read_text())

    def test_saved_controls(self):
        self.assertEqual(verify(), {"build": "1.2.92.148", "cases": 2, "verified": True})

    def test_corrupted_reference_rejected(self):
        for field, value in (("aes", "0" * 32), ("obfuscated_key", "0" * 32),
                             ("native_block16", "0" * 32)):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                broken = copy.deepcopy(self.fixture)
                broken["cases"][0][field] = value
                path = Path(directory) / "broken.json"
                path.write_text(json.dumps(broken))
                with self.assertRaises(ValueError):
                    verify(path)

    def test_runner_assesses_full_native_stream(self):
        events = []
        for case in self.fixture["cases"]:
            key = bytes.fromhex(case["aes"])
            enc = Cipher(algorithms.AES(key), modes.CTR(IV)).encryptor()
            stream = enc.update(bytes(case["content"]["bytes"])) + enc.finalize()
            blocks = [stream[i:i + 16].hex() for i in range(0, len(stream), 16)]
            events.append({"type": "pipeline_control", "file_id": case["file_id"],
                           "candidate": "0" * 32, "blocks": blocks, "repeat_blocks": blocks,
                           "deterministic": True})
        report = {"mode": "default_capture", "events": events + [{"type": "done"}], "results": []}
        self.assertEqual(assess_report(report, self.fixture), 0)
        self.assertEqual(report["summary"]["pass"], 2)
        # The prewrap candidate is intentionally different from the AES reference.
        # It must not decide content validity.
        altered = copy.deepcopy(report)
        altered["results"] = []
        altered["events"][0]["blocks"][20] = "0" * 32
        self.assertEqual(assess_report(altered, self.fixture), 1)
        self.assertEqual(altered["summary"]["mismatch"], 1)
        self.assertFalse(altered["summary"]["control_pass"])

    def test_script_has_no_reference_aes(self):
        script = make_script(self.fixture, Path(__file__).with_name("check_fresh_license_148.js"))
        for case in self.fixture["cases"]:
            self.assertNotIn(case["aes"], script)
        self.assertIn('"block_count": 256', script)

    def test_legacy_script_aliases_and_explicit_data(self):
        vectors = vectors_for_scripts(self.fixture)
        self.assertEqual(len(vectors), 2)
        self.assertEqual(vectors[0]["obfuscated"], self.fixture["cases"][0]["obfuscated_key"])
        self.assertEqual(vectors[0]["expected"], self.fixture["cases"][0]["aes"])
        script = make_script(self.fixture, Path(__file__).with_name("check_fresh_license_148.js"),
                             custom=True, script_data=[{"label": "test"}], exceptions="steal")
        self.assertIn('"label": "test"', script)
        self.assertIn(self.fixture["cases"][0]["aes"], script)
        self.assertIn('var exceptionMode = "steal"', script)

    def test_diagnostic_outcomes_and_detach(self):
        report = {"events": [{"type": "attached"}, {"type": "done"}], "results": []}
        self.assertEqual(capture_assessment(report, custom=True, planned=2), 2)
        self.assertEqual(report["assessment"], "inconclusive_no_diagnostic_result")
        report = {"events": [{"type": "keystream_control", "match": False}, {"type": "done"}], "results": []}
        self.assertEqual(capture_assessment(report, custom=True, planned=2), 1)
        self.assertEqual(report["assessment"], "diagnostic_mismatch")
        report = {"events": [{"type": "vector_error"}, {"type": "done"}], "results": []}
        self.assertEqual(capture_assessment(report, custom=True, planned=2), 2)
        done = threading.Event()
        callbacks = CaptureMessages(report, done)
        callbacks.closing = True
        callbacks.detached("application-requested", None)
        self.assertNotIn("error", report)
        callbacks.closing = False
        callbacks.detached("process-terminated", None)
        self.assertIn("Session detached", report["error"])


if __name__ == "__main__":
    unittest.main()
