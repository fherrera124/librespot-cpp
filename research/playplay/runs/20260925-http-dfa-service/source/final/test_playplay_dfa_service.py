"""HTTP contract, worker deadlines, and real offline recovery regressions."""
import json
import contextlib
import io
from pathlib import Path
import time
import unittest

from playplay_dfa_service import (
    BackendBusy, ExtractionError, ExtractionTimeout, FridaDfaBackend,
    _make_server, create_app, recover_key,
)


def stuck_worker(connection, pid):
    connection.send(("ready", {"pid": pid}))
    connection.recv()
    time.sleep(60)


def echo_worker(connection, pid):
    connection.send(("ready", {"pid": pid}))
    while True:
        value = connection.recv()
        if value is None:
            return
        connection.send(("key", value[0]))


class FakeBackend:
    def __init__(self):
        self.calls = []
        self.error = None

    def health(self):
        return {"ready": True, "busy": False}

    def extract(self, obfuscated, auxiliary):
        self.calls.append((obfuscated, auxiliary))
        if self.error:
            raise self.error
        return "ab" * 16


class HttpContractTest(unittest.TestCase):
    def setUp(self):
        self.backend = FakeBackend()
        self.client = create_app(self.backend, "test-token").test_client()
        self.headers = {"X-PlayPlay-Token": "test-token"}
        self.payload = {"obfuscated_key": "AB" * 16, "b4_seq": "1234ABCD"}

    def test_requires_authentication_before_backend(self):
        self.assertEqual(self.client.post("/deob", json=self.payload).status_code, 401)
        self.assertEqual(self.client.get("/health").status_code, 401)
        self.assertEqual(self.backend.calls, [])

    def test_requires_exact_license_fields(self):
        for payload in (
            {"obfuscated_key": "ab" * 16},
            dict(self.payload, b4_seq="00"),
            dict(self.payload, b4_seq="zz" * 4),
            dict(self.payload, obfuscated_key="ab" * 15),
            [],
        ):
            with self.subTest(payload=payload):
                result = self.client.post("/deob", json=payload, headers=self.headers)
                self.assertEqual(result.status_code, 400)
        self.assertEqual(self.backend.calls, [])

    def test_passes_actual_b4_and_returns_only_key(self):
        result = self.client.post("/deob", json=self.payload, headers=self.headers)
        self.assertEqual(result.status_code, 200)
        self.assertEqual(result.json, {"success": True, "aes_key": "ab" * 16})
        self.assertEqual(self.backend.calls, [("ab" * 16, "1234abcd")])
        self.assertEqual(result.headers["Cache-Control"], "no-store")

    def test_bounded_errors_and_body(self):
        for error, status, code in (
            (ExtractionTimeout("Timed out"), 504, "extraction_timeout"),
            (BackendBusy("Busy"), 503, "busy"),
            (ExtractionError("Failed"), 503, "extraction_failed"),
        ):
            self.backend.error = error
            result = self.client.post("/deob", json=self.payload, headers=self.headers)
            self.assertEqual((result.status_code, result.json["error"]), (status, code))
        self.assertEqual(self.client.post("/deob", data="x" * 2048,
            content_type="application/json", headers=self.headers).status_code, 413)


class WorkerTest(unittest.TestCase):
    def test_second_server_cannot_bind_the_same_port(self):
        server = _make_server("127.0.0.1", 0)
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    _make_server("127.0.0.1", server.port)
        finally:
            server.server_close()

    def test_serial_request_and_shutdown(self):
        backend = FridaDfaBackend(1, worker=echo_worker)
        try:
            self.assertTrue(backend.health()["ready"])
            self.assertEqual(backend.extract("ab" * 16, "00" * 4), "ab" * 16)
            backend.lock.acquire()
            try:
                with self.assertRaises(BackendBusy):
                    backend.extract("ab" * 16, "00" * 4)
            finally:
                backend.lock.release()
        finally:
            backend.close()
        self.assertFalse(backend.process.is_alive())

    def test_timeout_disables_worker_and_next_request_is_immediate(self):
        backend = FridaDfaBackend(1, timeout=0.1, worker=stuck_worker)
        try:
            start = time.monotonic()
            with self.assertRaises(ExtractionTimeout):
                backend.extract("ab" * 16, "00" * 4)
            self.assertLess(time.monotonic() - start, 4)
            self.assertFalse(backend.health()["ready"])
            self.assertFalse(backend.process.is_alive())
            with self.assertRaises(ExtractionError):
                backend.extract("ab" * 16, "00" * 4)
        finally:
            backend.close()


class RealRecoveryTest(unittest.TestCase):
    def test_saved_faults_recover_both_keys(self):
        run = Path(__file__).resolve().parents[1] / "runs/20260924-unicorn-feasibility/raw"
        traces = json.loads((run / "context-faults.json").read_text())["cases"]
        references = json.loads((run / "dfa-verification.json").read_text())["cases"]
        expected = {row["run"]: row["recovered_aes"] for row in references}
        for trace in traces:
            with self.subTest(case=trace["run"]):
                self.assertEqual(recover_key(trace), expected[trace["run"]])


if __name__ == "__main__":
    unittest.main()
