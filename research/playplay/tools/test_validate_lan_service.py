"""Local protocol checks; never contact Windows or Spotify."""

from contextlib import redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

from validate_lan_service import DEFAULT_VECTORS, validate


class LanValidationTests(unittest.TestCase):
    def setUp(self):
        data = json.loads(DEFAULT_VECTORS.read_text())
        self.expected = {
            obfuscated: item["aes"]
            for item in data["vectors_token_E"]
            for obfuscated in item["obfuscated"].values()
        }
        self.requests = []
        self.mode = "pass"
        test = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                test.requests.append(data)
                status = 200
                payload = {"aes_key": test.expected[data["obfuscated_key"]], "success": True}
                if test.mode == "mismatch":
                    payload["aes_key"] = "00" * 16
                elif test.mode == "bad_hex":
                    payload["aes_key"] = "0g" * 16
                elif test.mode == "not_initialized":
                    status = 500
                    payload = {"error": "NOT_INITIALIZED"}
                elif test.mode == "service_error":
                    payload["error"] = "EXCEPTION"
                elif test.mode == "bad_json":
                    payload = None
                body = json.dumps(payload).encode() if payload else b"{invalid"
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_port}/deob"
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def run_validation(self, repeat=1):
        with redirect_stdout(io.StringIO()):
            return validate(self.url, DEFAULT_VECTORS, 2, repeat)

    def test_all_vectors_and_repeated_requests(self):
        report = self.run_validation(repeat=2)
        self.assertEqual(report["summary"], {
            "planned": 22, "completed": 22, "pass": 22, "mismatch": 0, "error": 0,
        })
        self.assertTrue(all(set(req) == {"obfuscated_key"} for req in self.requests))
        self.assertEqual(self.requests[:11], self.requests[11:])

    def test_wrong_keys_are_mismatches(self):
        self.mode = "mismatch"
        report = self.run_validation()
        self.assertEqual(report["summary"]["mismatch"], 11)
        self.assertEqual(report["summary"]["error"], 0)

    def test_invalid_responses_stop_without_claiming_mismatch(self):
        for mode in ("bad_hex", "not_initialized", "service_error", "bad_json"):
            with self.subTest(mode=mode):
                self.mode = mode
                report = self.run_validation()
                self.assertEqual(report["summary"]["completed"], 1)
                self.assertEqual(report["summary"]["error"], 1)
                self.assertEqual(report["summary"]["mismatch"], 0)

    def test_cli_exit_codes_and_report(self):
        script = Path(__file__).with_name("validate_lan_service.py")
        with tempfile.TemporaryDirectory() as directory:
            for mode, exit_code in (("pass", 0), ("mismatch", 1), ("not_initialized", 2)):
                self.mode = mode
                report_path = Path(directory) / f"{mode}.json"
                args = [sys.executable, str(script), "--url", self.url,
                        "--report", str(report_path)]
                result = subprocess.run(args, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, exit_code, result.stderr)
                report = json.loads(report_path.read_text())
                self.assertEqual(report["vector_count"], 11)
                original = report_path.read_bytes()
                rerun = subprocess.run(args, capture_output=True, text=True, timeout=10)
                self.assertEqual(rerun.returncode, 2)
                self.assertEqual(report_path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
