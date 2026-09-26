#!/usr/bin/env python3
"""Capture build-148 Windows diagnostics; evaluate saved content on Linux offline.

Default capture injects only obfuscated_key/b4_seq. --script enables the legacy
diagnostic interface and explicitly injects reference AES in `vectors`.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import threading

from playplay_148_preflight import DLL_SHA256, guarded_source, preflight

DEFAULT = Path(__file__).resolve().parent.parent / "data/ground-truth-vectors.json"
ROOT = Path(__file__).resolve().parents[3]


def vectors_for_scripts(fixture):
    return [{"file_id": row["file_id"], "label": row["file_id"][:8],
             "version": "v5", "obfuscated": row["obfuscated_key"],
             "aes": row["aes"], "expected": row["aes"], "b4_seq": row["b4_seq"]}
            for row in fixture["cases"]]


def make_script(fixture, script_path, *, custom=False, script_data=None,
                exceptions="propagate", advance_track=False):
    if custom:
        source = "var exceptionMode = " + json.dumps(exceptions) + ";\n"
        source += "var vectors = " + json.dumps(vectors_for_scripts(fixture)) + ";\n"
        if script_data is not None:
            source += "var diagnosticData = " + json.dumps(script_data) + ";\n"
    else:
        data = [{"label": row["file_id"][:8], "file_id": row["file_id"],
                 "obfuscated": row["obfuscated_key"], "b4_seq": row["b4_seq"],
                 "block_count": row["content"]["bytes"] // 16} for row in fixture["cases"]]
        source = "var diagnosticData = " + json.dumps(data) + ";\n"
    source += script_path.read_text()
    if advance_track:
        source += "\n" + Path(__file__).with_name("advance_spotify_track.js").read_text()
    return source


def capture_assessment(report, *, custom, planned):
    events = report["events"]
    done = any(row.get("type") == "done" for row in events)
    fatal = any(row.get("type") == "fatal" for row in events)
    if report.get("error") or fatal or not done:
        report["summary"] = {"planned": planned, "completed": 0, "pass": 0,
                             "mismatch": 0, "error": 1, "control_pass": False}
        report["assessment"] = "inconclusive_execution_error"
        return 2
    if custom:
        substantive = [row for row in events if row.get("type") in (
            "control_replay", "result", "vector_error",
            "copies", "raw_output", "pipeline_control", "keystream_control")]
        errors = [row for row in substantive if row.get("type") == "vector_error" or
                  row.get("status") == "error"]
        mismatches = [row for row in substantive if row.get("match") is False or
                      row.get("status") == "mismatch"]
        if not substantive:
            report["assessment"] = "inconclusive_no_diagnostic_result"
            code = 2
        elif errors:
            report["assessment"] = "inconclusive_diagnostic_error"
            code = 2
        elif mismatches:
            report["assessment"] = "diagnostic_mismatch"
            code = 1
        else:
            report["assessment"] = "diagnostic_completed_unverified"
            code = 0
        report["summary"] = {"planned": planned, "completed": len(substantive),
                             "pass": 0, "mismatch": len(mismatches), "error": len(errors),
                             "control_pass": code == 0}
        return code
    rows = [row for row in events if row.get("type") == "pipeline_control"]
    expected_ids = {row["file_id"] for row in planned}
    ids = [row.get("file_id") for row in rows]
    if len(rows) != len(planned) or set(ids) != expected_ids or len(ids) != len(set(ids)) or any(
        len(row.get("blocks", [])) != 256 or len(row.get("repeat_blocks", [])) != 256
        for row in rows
    ):
        report["assessment"] = "inconclusive_incomplete_capture"
        report["summary"] = {"planned": len(planned), "completed": len(rows),
                             "pass": 0, "mismatch": 0, "error": 1, "control_pass": False}
        return 2
    nondeterministic = sum(row.get("deterministic") is not True for row in rows)
    if nondeterministic:
        report["assessment"] = "capture_mismatch"
        report["summary"] = {"planned": len(planned), "completed": len(rows),
                             "pass": 0, "mismatch": nondeterministic, "error": 0,
                             "control_pass": False}
        return 1
    report["assessment"] = "capture_completed_unverified"
    report["summary"] = {"planned": len(planned), "completed": len(rows),
                         "pass": 0, "mismatch": 0, "error": 0, "control_pass": True}
    return 0


def assess_report(report, fixture):
    """Offline cryptographic assessment of a saved default capture."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from verify_token148_content import IV, first_page_valid
    report["results"] = []
    if report.get("mode") != "default_capture" or report.get("error"):
        report["assessment"] = "inconclusive_wrong_mode_or_error"
        return 2
    if capture_assessment(report, custom=False, planned=fixture["cases"]) != 0:
        return 2
    cases = {row["file_id"]: row for row in fixture["cases"]}
    for event in (row for row in report["events"] if row.get("type") == "pipeline_control"):
        case = cases[event["file_id"]]
        encrypted = (ROOT / case["content"]["file"]).read_bytes()
        key = bytes.fromhex(case["aes"])
        enc = Cipher(algorithms.AES(key), modes.CTR(IV)).encryptor()
        expected = enc.update(bytes(len(encrypted))) + enc.finalize()
        try:
            native = bytes.fromhex("".join(event["blocks"]))
            repeat = bytes.fromhex("".join(event["repeat_blocks"]))
        except (ValueError, TypeError, KeyError):
            report["assessment"] = "inconclusive_malformed_stream"
            return 2
        plain = bytes(a ^ b for a, b in zip(encrypted, native))
        result = {"file_id": case["file_id"], "version": "v5",
                  "expected": case["native_block16"], "actual": native[:16].hex(),
                  "native_matches_aes128ctr": native == expected,
                  "repeat_matches": native == repeat,
                  "ogg_vorbis_crc_valid": len(native) == len(encrypted) and first_page_valid(plain)}
        result["status"] = "pass" if all(result[key] for key in (
            "native_matches_aes128ctr", "repeat_matches", "ogg_vorbis_crc_valid")) else "mismatch"
        report["results"].append(result)
    mismatch = sum(row["status"] == "mismatch" for row in report["results"])
    report["summary"].update(pass_=len(cases) - mismatch, mismatch=mismatch,
                             control_pass=mismatch == 0)
    report["summary"]["pass"] = report["summary"].pop("pass_")
    report["assessment"] = "candidate_mismatch" if mismatch else "all_vectors_match"
    return 1 if mismatch else 0


class CaptureMessages:
    def __init__(self, report, done):
        self.report = report
        self.done = done
        self.closing = False

    def message(self, message, data):
        if message["type"] == "error":
            self.report["error"] = message.get("stack", str(message))
            self.done.set()
        elif message["type"] == "send":
            event = message["payload"]
            self.report["events"].append(event)
            if event.get("type") == "fatal":
                self.report["error"] = event.get("error", "Frida fatal error")
            if event.get("type") in ("fatal", "done"):
                self.done.set()

    def detached(self, reason, crash):
        if not self.closing and not self.done.is_set():
            self.report["error"] = f"Session detached: {reason}"
            self.done.set()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="Verified main Spotify.exe PID for capture")
    parser.add_argument("--vectors", type=Path, default=DEFAULT)
    parser.add_argument("--report", type=Path, help="New capture report; never overwrites")
    parser.add_argument("--evaluate-report", type=Path, help="Evaluate saved default capture offline")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--script", type=Path, help="Explicit legacy diagnostic JS")
    parser.add_argument("--script-data", type=Path, help="JSON array injected as diagnosticData")
    parser.add_argument("--exceptions", choices=("steal", "propagate"), default="steal")
    parser.add_argument("--advance-track", action="store_true")
    parser.add_argument("--capture-only", action="store_true", help="Explicitly retain native output for offline evaluation")
    args = parser.parse_args()
    if args.script_data and not args.script:
        parser.error("--script-data requires --script")
    if not 0 < args.timeout < float("inf"):
        parser.error("--timeout must be positive and finite")
    if args.evaluate_report and (args.pid or args.report or args.script or args.script_data or args.advance_track):
        parser.error("--evaluate-report is an offline operation")
    if not args.evaluate_report and (not args.pid or not args.report):
        parser.error("capture requires --pid and --report")
    if args.script_data:
        script_data = json.loads(args.script_data.read_text())
        if not isinstance(script_data, list):
            parser.error("--script-data must contain a JSON array")
    else:
        script_data = None
    raw = args.vectors.read_bytes()
    fixture = json.loads(raw)
    if (fixture.get("schema_version") != 2 or fixture.get("build", {}).get("dll_sha256") != DLL_SHA256
            or len(fixture.get("cases", [])) != 2):
        parser.error("expected the two build-148 controls")
    if args.evaluate_report:
        from check_ground_truth_148 import verify
        verify(args.vectors)
        report = json.loads(args.evaluate_report.read_text())
        if report.get("vectors_sha256") != hashlib.sha256(raw).hexdigest():
            parser.error("capture fixture hash differs from current fixture")
        code = assess_report(report, fixture)
        print(json.dumps({"assessment": report["assessment"], "summary": report.get("summary")}))
        return code
    import frida
    import psutil
    process = psutil.Process(args.pid)
    if process.name().lower() != "spotify.exe" or any(
        "--type=" in part.lower() or "crashpad" in part.lower() for part in process.cmdline()
    ):
        parser.error("PID must belong to the main Spotify.exe process")
    custom = args.script is not None
    script_path = args.script or Path(__file__).with_name("check_fresh_license_148.js")
    plain_source = make_script(fixture, script_path, custom=custom, script_data=script_data,
                               exceptions=args.exceptions, advance_track=args.advance_track)
    report = {"started_at": datetime.now(timezone.utc).isoformat(), "pid": args.pid,
              "vectors_sha256": hashlib.sha256(raw).hexdigest(),
              "mode": "alternative_script" if custom else "default_capture",
              "script_file": script_path.name,
              "exceptions": args.exceptions if custom else "propagate",
              "results": [], "events": []}
    done = threading.Event()
    callbacks = CaptureMessages(report, done)
    with args.report.open("x", encoding="utf-8") as output:
        session = script = None
        try:
            session = frida.attach(args.pid)
            session.on("detached", callbacks.detached)
            identity = preflight(session, args.pid)
            report["spotify_dll"] = {"version": identity["version"], "sha256": identity["sha256"]}
            # Guard must execute before any diagnostic hook or NativeFunction.
            source = guarded_source("requireVerifiedBuild148();\n" + plain_source, identity)
            report["script_sha256"] = hashlib.sha256(source.encode()).hexdigest()
            script = session.create_script(source)
            script.on("message", callbacks.message)
            script.load()
            if not done.wait(args.timeout):
                report["error"] = "Timed out waiting for diagnostic"
        except (Exception, KeyboardInterrupt) as exc:
            report["error"] = str(exc) or type(exc).__name__
        finally:
            callbacks.closing = True
            if script:
                try:
                    script.unload()
                except Exception:
                    pass
            if session:
                try:
                    session.detach()
                except Exception:
                    pass
            code = capture_assessment(report, custom=custom,
                                      planned=(len(script_data) if script_data is not None else len(fixture["cases"]))
                                      if custom else fixture["cases"])
            report["finished_at"] = datetime.now(timezone.utc).isoformat()
            json.dump(report, output, indent=2)
            output.write("\n")
    print(json.dumps({"assessment": report["assessment"], "summary": report["summary"]}))
    return code


if __name__ == "__main__":
    sys.exit(main())
