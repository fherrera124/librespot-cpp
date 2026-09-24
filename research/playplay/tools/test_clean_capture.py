"""Offline regression checks; never attach to a real process.

Optional quickjs enables execution of the JS guard and exception cleanup tests.
"""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import playplay_148_preflight as gate
import run_clean_capture as runner

try:
    import quickjs
except ImportError:
    quickjs = None


class BuildGateTests(unittest.TestCase):
    def test_disk_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            dll = Path(tmp) / "Spotify.dll"
            dll.write_bytes(b"fixture binary")
            identity = dict(pid=42, arch="x64", platform="windows", path=str(dll), base="0x1000", size=1024)
            digest = hashlib.sha256(dll.read_bytes()).hexdigest()
            with patch.object(gate, "file_version", return_value=gate.VERSION), patch.object(gate, "DLL_SHA256", digest):
                self.assertEqual(gate.verify_identity(identity, 42)["sha256"], digest)
                dll.write_bytes(b"different sub-build")
                with self.assertRaisesRegex(ValueError, "Unsupported"):
                    gate.verify_identity(identity, 42)
            with patch.object(gate, "file_version", return_value="1.2.93.667"), patch.object(gate, "DLL_SHA256", hashlib.sha256(dll.read_bytes()).hexdigest()):
                with self.assertRaisesRegex(ValueError, "Unsupported"):
                    gate.verify_identity(identity, 42)
            for changes in ({"pid": 43}, {"arch": "ia32"}, {"platform": "linux"}):
                with self.assertRaisesRegex(ValueError, "Windows x64"):
                    gate.verify_identity(dict(identity, **changes), 42)

    def test_probe_unloaded_on_rejection(self):
        probe = types.SimpleNamespace(load=lambda: None, exports_sync=types.SimpleNamespace(describe=lambda: {}))
        unloaded = []
        probe.unload = lambda: unloaded.append(True)
        session = types.SimpleNamespace(create_script=lambda source: probe)
        with patch.object(gate, "verify_identity", side_effect=ValueError("wrong build")):
            with self.assertRaises(ValueError):
                gate.preflight(session, 42)
        self.assertEqual(unloaded, [True])


class RunnerTests(unittest.TestCase):
    def run_case(self, outcome, rejected=False):
        events = []

        class Script:
            def on(self, name, callback):
                self.callback = callback

            def load(self):
                if outcome == "js-error":
                    self.callback({"type": "error", "stack": "JS failed"}, None)
                elif outcome != "timeout":
                    self.callback({"type": "send", "payload": {"type": outcome, "stack": "native failed"}}, None)

            def unload(self):
                events.append("unload")

        class Session:
            def on(self, *args):
                pass

            def create_script(self, source):
                events.append("create-capture")
                return Script()

            def detach(self):
                events.append("detach")

        session = Session()
        fake_modules = {
            "frida": types.SimpleNamespace(attach=lambda pid: session),
            "psutil": types.SimpleNamespace(Process=lambda pid: types.SimpleNamespace(name=lambda: "Spotify.exe", cmdline=lambda: ["Spotify.exe"])),
        }
        with tempfile.TemporaryDirectory() as tmp:
            source, report = Path(tmp) / "capture.js", Path(tmp) / "report.json"
            source.write_text("// fixture")
            args = ["runner", "--pid", "42", "--script", str(source), "--report", str(report), "--timeout", "0.001"]
            with patch.dict(sys.modules, fake_modules), patch.object(sys, "argv", args), patch.object(runner, "preflight", side_effect=ValueError("wrong build") if rejected else None, return_value={"version": gate.VERSION}), contextlib.redirect_stdout(io.StringIO()):
                code = runner.main()
            return code, json.loads(report.read_text()), events

    def test_completion(self):
        code, report, events = self.run_case("clean_capture_done")
        self.assertEqual(code, 0)
        self.assertIn("executed_source_sha256", report)
        self.assertEqual(events, ["create-capture", "unload", "detach"])

    def test_errors_and_timeout_fail(self):
        for outcome in ("error", "js-error", "timeout"):
            with self.subTest(outcome=outcome):
                code, report, events = self.run_case(outcome)
                self.assertEqual(code, 1)
                self.assertIn("error", report)
                self.assertEqual(events[-2:], ["unload", "detach"])

    def test_rejected_build_never_loads_capture(self):
        code, report, events = self.run_case("clean_capture_done", rejected=True)
        self.assertEqual(code, 1)
        self.assertEqual(events, ["detach"])
        self.assertEqual(report["error"], "wrong build")


@unittest.skipIf(quickjs is None, "install quickjs for JS behavior checks")
class JavaScriptTests(unittest.TestCase):
    def test_sources_parse_and_refuse_unverified_execution(self):
        for name in ("capture_clean_148.js", "capture_context_148.js"):
            source = Path(__file__).with_name(name).read_text()
            ctx = quickjs.Context()
            ctx.eval("new Function(" + json.dumps(source) + ")")
            with self.assertRaisesRegex(quickjs.JSException, "verified build 148 required"):
                ctx.eval(source)

    def test_live_module_guard(self):
        identity = dict(pid=42, arch="x64", platform="windows", path="Spotify.dll", base="0x1000", size=1024)
        setup = """
        let broken = false;
        const signatures = SIGNATURES;
        const mod = {path: 'Spotify.dll', size: 1024, base: {
            toString: () => '0x1000',
            add: rva => ({readByteArray: n => new Uint8Array(
                (broken ? '00'.repeat(n) : signatures['0x' + rva.toString(16)])
                .match(/../g).map(b => parseInt(b, 16))).buffer})
        }};
        const Process = {id: 42, arch: 'x64', platform: 'windows', getModuleByName: () => mod};
        """.replace("SIGNATURES", json.dumps(gate.ENTRY_SIGNATURES))
        ctx = quickjs.Context()
        ctx.eval(setup + gate.guarded_source("requireVerifiedBuild148();", identity))
        with self.assertRaisesRegex(quickjs.JSException, "signature mismatch"):
            ctx.eval("broken = true; requireVerifiedBuild148();")
        with self.assertRaisesRegex(quickjs.JSException, "changed after preflight"):
            ctx.eval("broken = false; mod.path = 'other.dll'; requireVerifiedBuild148();")

    def test_pipeline_failure_detaches_context_hook(self):
        source = Path(__file__).with_name("capture_context_148.js").read_text()
        ctx = quickjs.Context()
        ctx.eval("""
        let detached = 0, calls = 0;
        const messages = [];
        const pointer = {add: () => pointer, writeByteArray: () => {}, writeU8: () => {}};
        function requireVerifiedBuild148() { return {base: pointer}; }
        function NativeFunction() { return () => { calls++; throw new Error('pipeline failed'); }; }
        const Memory = {alloc: () => pointer};
        const Interceptor = {attach: () => ({detach: () => detached++})};
        const Process = {getCurrentThreadId: () => 42};
        const ptr = () => pointer;
        const send = event => messages.push(event);
        const setImmediate = fn => fn();
        """ + source)
        self.assertEqual(ctx.eval("detached"), 1)
        self.assertEqual(ctx.eval("calls"), 1)
        self.assertTrue(ctx.eval("activeThread === null"))
        self.assertEqual(ctx.eval("messages[messages.length - 1].type"), "error")


if __name__ == "__main__":
    unittest.main()
