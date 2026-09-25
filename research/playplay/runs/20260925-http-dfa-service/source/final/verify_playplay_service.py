#!/usr/bin/env python3
"""Check HTTP AES extraction against saved licenses/content, without account tokens.

Run on Linux from the repository root. Only obfuscated_key and b4_seq cross HTTP;
reference AES and encrypted content remain local. Reports never contain AES values.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import time
from urllib import error, request

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from verify_token148_content import IV, first_page_valid


class NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8765/deob")
    parser.add_argument("--license", action="append", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=2)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    # Reserve the report before any requests, never overwrite old evidence.
    with args.report.open("x", encoding="utf-8") as report_file:
        report = {"started_at": datetime.now(timezone.utc).isoformat(), "cases": []}
        opener = request.build_opener(request.ProxyHandler({}), NoRedirect())
        headers = {"Content-Type": "application/json"}
        if os.environ.get("PLAYPLAY_SERVICE_TOKEN"):
            headers["X-PlayPlay-Token"] = os.environ["PLAYPLAY_SERVICE_TOKEN"]
        try:
            for path in args.license:
                raw = path.read_bytes()
                license = json.loads(raw)
                encrypted = Path(license["content_file"]).read_bytes()
                if hashlib.sha256(encrypted).hexdigest() != license["content_sha256"]:
                    raise ValueError("Encrypted content hash mismatch")
                payload = {name: license[name] for name in ("obfuscated_key", "b4_seq")}
                for repeat in range(args.repeat):
                    row = {"file_id": license["file_id"], "repeat": repeat + 1,
                           "license_sha256": hashlib.sha256(raw).hexdigest()}
                    start = time.monotonic()
                    try:
                        req = request.Request(args.url, data=json.dumps(payload).encode(), headers=headers)
                        with opener.open(req, timeout=26) as response:
                            row["http_status"] = response.status
                            body = response.read(4097)
                        if len(body) > 4096:
                            raise ValueError("Oversized response")
                        answer = json.loads(body)
                        key_hex = answer.get("aes_key", "")
                        if answer.get("success") is not True or not isinstance(key_hex, str) or not re.fullmatch(r"[0-9a-fA-F]{32}", key_hex):
                            raise ValueError("Invalid AES response")
                        key = bytes.fromhex(key_hex)
                        decryptor = Cipher(algorithms.AES(key), modes.CTR(IV)).decryptor()
                        plain = decryptor.update(encrypted) + decryptor.finalize()
                        row.update(matches_reference=key_hex.lower() == license["reference_aes"].lower(),
                                   ogg_vorbis_crc_valid=first_page_valid(plain), content_bytes=len(encrypted))
                        row["pass"] = row["matches_reference"] and row["ogg_vorbis_crc_valid"]
                    except error.HTTPError as exc:
                        row.update(http_status=exc.code, **{"pass": False})
                        try:
                            row["error"] = json.loads(exc.read(4096)).get("error", "http_error")
                        except (ValueError, AttributeError):
                            row["error"] = "http_error"
                    except (OSError, ValueError, error.URLError):
                        row.update(error="transport_or_validation_error", **{"pass": False})
                    row["seconds"] = round(time.monotonic() - start, 3)
                    report["cases"].append(row)
                    print(json.dumps(row), flush=True)
                    if not row["pass"]:
                        return 1
            return 0
        finally:
            report["all_pass"] = len(report["cases"]) == len(args.license) * args.repeat and all(row["pass"] for row in report["cases"])
            json.dump(report, report_file, indent=2)
            report_file.write("\n")


if __name__ == "__main__":
    raise SystemExit(main())
