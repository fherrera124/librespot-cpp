#!/usr/bin/env python3
"""Local HTTP bridge for the hash-verified Spotify 1.2.92.148 PlayPlay DFA path.

Run on the same Windows host as Spotify.exe. Dependencies: flask, frida,
cryptography, psutil. Requests contain only the PlayPlay response fields; account
credentials are neither needed nor accepted. The service does not log keys.
"""

import argparse
import hmac
import ipaddress
import multiprocessing
import os
from pathlib import Path
import re
import threading
import time


TOOLS = Path(__file__).resolve().parent
RPC_SOURCE = TOOLS / "playplay_dfa_rpc.js"
HEX16 = re.compile(r"[0-9a-fA-F]{32}\Z")
HEX4 = re.compile(r"[0-9a-fA-F]{8}\Z")
AUDIO_IV = bytes.fromhex("72e067fbddcbcf77ebe8bc643f630d93")


class ExtractionError(Exception):
    """A failed extraction, with no key material in its message."""


class ExtractionTimeout(ExtractionError):
    pass


class BackendBusy(ExtractionError):
    pass


def _field(value, pattern):
    return isinstance(value, str) and pattern.fullmatch(value) is not None


def _validate_trace(trace):
    if not isinstance(trace, dict) or not _field(trace.get("correct"), HEX16):
        raise ExtractionError("Frida returned an invalid trace")
    faults = trace.get("faults")
    if not isinstance(faults, list) or len(faults) > 159:
        raise ExtractionError("Frida returned an invalid trace")
    if any(not isinstance(row, dict) or not _field(row.get("ct"), HEX16)
           for row in faults):
        raise ExtractionError("Frida returned an invalid trace")
    return trace


def recover_key(trace):
    """Solve DFA and verify the recovered AES against the correct stream block."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from extractor import solve_dfa
    from reverse_key_schedule import reverse_key_schedule

    trace = _validate_trace(trace)
    round10 = solve_dfa(trace["correct"], trace["faults"])
    if round10 is None:
        raise ExtractionError("DFA did not resolve all AES columns")
    key = reverse_key_schedule(list(round10))
    if not isinstance(key, bytes) or len(key) != 16:
        raise ExtractionError("Invalid key schedule result")
    cipher = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    first_block = cipher.update(AUDIO_IV) + cipher.finalize()
    if first_block.hex() != trace["correct"].lower():
        raise ExtractionError("Recovered key failed stream verification")
    return key.hex()


class _NativeBackend:
    """One attachment, one persistent RPC script, one extraction at a time."""

    def __init__(self, pid, progress=None):
        import frida
        import psutil
        from playplay_148_preflight import guarded_source, preflight

        process = psutil.Process(pid)
        if process.name().lower() != "spotify.exe" or any(
            arg.startswith("--type=") for arg in process.cmdline()
        ):
            raise ExtractionError("Select the main Spotify.exe process")
        # Read all sources/dependencies before attaching to the target.
        source = RPC_SOURCE.read_text(encoding="utf-8")
        from extractor import solve_dfa  # noqa: F401
        from cryptography.hazmat.primitives.ciphers import Cipher  # noqa: F401
        self.lock = threading.Lock()
        self.detached = False
        self.session = frida.attach(pid)
        self.script = None
        try:
            identity = preflight(self.session, pid)
            source = guarded_source(source, identity)
            self.script = self.session.create_script(source)
            if progress is not None:
                def on_message(message, data):
                    payload = message.get("payload", {})
                    if message.get("type") == "send" and isinstance(payload, dict):
                        stage = payload.get("stage")
                        if stage in ("pipeline", "initialize", "faults", "captured"):
                            progress(stage)
                self.script.on("message", on_message)
            self.script.load()
            self.session.on("detached", self._on_detached)
            self.script.exports_sync.check()
            self.identity = {name: identity[name] for name in ("pid", "version", "sha256")}
        except Exception:
            self.close()
            raise

    def _on_detached(self, *unused):
        self.detached = True

    def extract(self, obfuscated_key, b4_seq):
        with self.lock:
            if self.detached or self.script is None:
                raise ExtractionError("Spotify attachment is unavailable")
            try:
                trace = self.script.exports_sync.deob(obfuscated_key, b4_seq)
            except Exception as exc:
                # Frida exceptions can include argument values and stack text.
                raise ExtractionError("Frida extraction failed") from None
            return recover_key(trace)

    def close(self):
        with self.lock:
            if self.script is not None:
                try:
                    self.script.unload()
                except Exception:
                    pass
                self.script = None
            if self.session is not None:
                try:
                    self.session.detach()
                except Exception:
                    pass
                self.session = None
            self.detached = True


def _worker_main(connection, pid):
    """Only this child process owns the Frida attachment and native calls."""
    backend = None
    send_lock = threading.Lock()
    def send(message):
        with send_lock:
            connection.send(message)
    try:
        backend = _NativeBackend(pid, lambda stage: send(("stage", stage)))
        send(("ready", backend.identity))
        while True:
            command = connection.recv()
            if command is None:
                return
            try:
                send(("key", backend.extract(*command)))
            except ExtractionError:
                send(("error", "Extraction failed or attachment unavailable"))
            except Exception:
                send(("error", "Extraction failed"))
    except (EOFError, BrokenPipeError):
        pass
    except Exception:
        try:
            send(("error", "Worker initialization failed; check files, PID and build"))
        except (EOFError, BrokenPipeError, OSError):
            pass
    finally:
        if backend is not None:
            backend.close()
        connection.close()


class FridaDfaBackend:
    """Bounded HTTP wait; after a timeout, fail closed instead of reattaching."""

    def __init__(self, pid, timeout=20.0, startup_timeout=20.0, worker=_worker_main):
        if timeout <= 0 or startup_timeout <= 0:
            raise ValueError("Timeouts must be positive")
        self.timeout = timeout
        self.lock = threading.Lock()
        self.failed = False
        self.identity = {}
        self.stage = "starting"
        context = multiprocessing.get_context("spawn")
        self.connection, child = context.Pipe()
        self.process = context.Process(target=worker, args=(child, pid), daemon=True)
        self.process.start()
        child.close()
        try:
            kind, payload = self._receive(startup_timeout)
            if kind != "ready":
                raise ExtractionError(payload)
            self.identity = payload
        except Exception:
            self._stop()
            raise

    def _receive(self, timeout):
        deadline = time.monotonic() + timeout
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self.connection.poll(remaining):
                    raise ExtractionTimeout("Extraction timed out; worker disabled")
                message = self.connection.recv()
                if message[0] != "stage":
                    return message
                self.stage = message[1]
        except (EOFError, OSError):
            raise ExtractionError("Extraction worker disconnected") from None

    def _stop(self):
        self.failed = True
        if self.process.is_alive():
            self.process.terminate()
        self.process.join(timeout=1)
        if self.process.is_alive():
            self.process.kill()
            self.process.join(timeout=1)
        self.connection.close()

    def health(self):
        ready = not self.failed and self.process.is_alive()
        return {"ready": ready, "busy": self.lock.locked(), "stage": self.stage, **self.identity}

    def extract(self, obfuscated_key, b4_seq):
        if not self.lock.acquire(blocking=False):
            raise BackendBusy("An extraction is already running")
        try:
            if self.failed or not self.process.is_alive():
                raise ExtractionError("Extraction worker is unavailable")
            try:
                self.stage = "requested"
                self.connection.send((obfuscated_key, b4_seq))
                kind, payload = self._receive(self.timeout)
            except (ExtractionError, OSError):
                self._stop()
                raise
            if kind != "key" or not _field(payload, HEX16):
                self._stop()
                raise ExtractionError("Extraction failed; worker disabled")
            self.stage = "verified"
            return payload
        finally:
            self.lock.release()

    def close(self):
        if self.lock.acquire(blocking=False):
            try:
                if not self.failed and self.process.is_alive():
                    try:
                        self.connection.send(None)
                        self.process.join(timeout=2)
                    except (BrokenPipeError, OSError):
                        pass
                self._stop()
            finally:
                self.lock.release()
        else:
            self._stop()


def create_app(backend, token):
    from flask import Flask, jsonify, request

    app = Flask(__name__)
    app.config["MAX_CONTENT_LENGTH"] = 1024
    app.config["PROPAGATE_EXCEPTIONS"] = False

    @app.before_request
    def authenticate():
        supplied = request.headers.get("X-PlayPlay-Token", "")
        if token and not hmac.compare_digest(supplied.encode("utf-8"), token.encode("utf-8")):
            return jsonify(success=False, error="unauthorized"), 401

    @app.after_request
    def no_cache(response):
        response.headers["Cache-Control"] = "no-store"
        return response

    @app.get("/health")
    def health():
        status = backend.health()
        return jsonify(status), 200 if status["ready"] else 503

    @app.post("/deob")
    def deob():
        if not request.is_json:
            return jsonify(success=False, error="expected_json"), 415
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not _field(payload.get("obfuscated_key"), HEX16):
            return jsonify(success=False, error="invalid_fields"), 400
        b4_seq = payload.get("b4_seq")
        if not _field(b4_seq, HEX4):
            return jsonify(success=False, error="invalid_fields"), 400
        try:
            key = backend.extract(payload["obfuscated_key"].lower(), b4_seq.lower())
            if not _field(key, HEX16):
                raise ExtractionError("Invalid key returned by extraction worker")
        except ExtractionTimeout as exc:
            return jsonify(success=False, error="extraction_timeout", detail=str(exc)), 504
        except BackendBusy:
            return jsonify(success=False, error="busy"), 503
        except ExtractionError as exc:
            return jsonify(success=False, error="extraction_failed", detail=str(exc)), 503
        except Exception:
            return jsonify(success=False, error="internal_error"), 500
        return jsonify(success=True, aes_key=key)

    @app.errorhandler(413)
    def too_large(_error):
        return jsonify(success=False, error="request_too_large"), 413

    return app


def _valid_listen_config(host, token, allow_unauthenticated_loopback):
    try:
        loopback = ipaddress.ip_address(host).is_loopback
    except ValueError as exc:
        raise ValueError("--host must be a numeric IP address") from exc
    if not token and not (loopback and allow_unauthenticated_loopback):
        raise ValueError("Set PLAYPLAY_SERVICE_TOKEN, or explicitly allow unauthenticated loopback")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="PID of the running Spotify.exe")
    parser.add_argument("--host", default="127.0.0.1", help="numeric listen address")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--allow-unauthenticated-loopback", action="store_true")
    parser.add_argument("--timeout", type=float, default=20.0, help="maximum extraction seconds")
    args = parser.parse_args(argv)
    if args.pid <= 0 or not 1 <= args.port <= 65535 or not 0 < args.timeout <= 120:
        parser.error("Invalid PID, port or timeout (must be 0 < timeout <= 120)")
    token = os.environ.get("PLAYPLAY_SERVICE_TOKEN", "")
    try:
        _valid_listen_config(args.host, token, args.allow_unauthenticated_loopback)
    except ValueError as exc:
        parser.error(str(exc))

    backend = FridaDfaBackend(args.pid, timeout=args.timeout)
    try:
        app = create_app(backend, token)
        import logging
        logging.getLogger("werkzeug").disabled = True
        app.run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    finally:
        backend.close()


if __name__ == "__main__":
    main()
