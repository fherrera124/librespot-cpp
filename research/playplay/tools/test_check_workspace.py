"""Small offline regression tests for check_workspace.py."""

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("check_workspace.py")


class WorkspaceCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.base = self.root / "research/playplay"
        (self.base / "docs").mkdir(parents=True)
        (self.base / "data").mkdir()
        self.artifact = self.base / "data/sample.bin"
        self.artifact.write_bytes(b"evidence")
        self.doc = self.base / "docs/README.md"
        self.doc.write_text("[sample](../data/sample.bin#ignored)\n", encoding="utf-8")
        self.catalog = {
            "schema_version": 1,
            "resources": [{"id": "sample", "status": "verified", "summary": "Example", "paths": ["research/playplay/data/sample.bin"]}],
            "evidence_manifests": ["research/playplay/docs/run.json"],
            "documentation": ["research/playplay/docs/README.md"],
        }
        self.manifest = {
            "schema_version": 1, "run_id": "test", "status": "verified", "summary": "Example",
            "artifacts": [{"path": "research/playplay/data/sample.bin", "sha256": hashlib.sha256(b"evidence").hexdigest(), "bytes": 8, "role": "fixture"}],
        }
        self.save()

    def save(self):
        (self.base / "catalog.json").write_text(json.dumps(self.catalog), encoding="utf-8")
        (self.base / "docs/run.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def check(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), *args], capture_output=True, text=True)

    def test_valid_and_listing(self):
        self.assertEqual(self.check().returncode, 0)
        result = self.check("--list")
        self.assertEqual(result.returncode, 0)
        self.assertIn("sample\tverified\tExample", result.stdout)

    def test_corrupt_hash(self):
        self.artifact.write_bytes(b"tampered!")
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("SHA256 mismatch", result.stderr)

    def test_broken_link(self):
        self.doc.write_text("[gone](missing.md)\n", encoding="utf-8")
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("broken link", result.stderr)

    def test_missing_resource(self):
        self.catalog["resources"][0]["paths"] = ["research/playplay/data/missing.bin"]
        self.save()
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing.bin", result.stderr)


if __name__ == "__main__":
    unittest.main()
