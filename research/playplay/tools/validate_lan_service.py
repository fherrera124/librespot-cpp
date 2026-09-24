#!/usr/bin/env python3
"""Compare the LAN /deob endpoint with saved Token E vectors (no credentials)."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
from http.client import HTTPException
import json
from pathlib import Path
import re
import sys
from urllib import error, request
from urllib.parse import urlsplit


DEFAULT_VECTORS = Path(__file__).resolve().parents[1] / "data/ground-truth-vectors.json"


def hex_value(value, byte_count):
    if not isinstance(value, str) or not re.fullmatch(
        rf"[0-9a-fA-F]{{{byte_count * 2}}}", value
    ):
        raise ValueError(f"Expected exactly {byte_count} bytes of hexadecimal data")
    return value.lower()


class NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def validate(url, vectors_path, timeout, repeat):
    raw = vectors_path.read_bytes()
    data = json.loads(raw)
    vectors = []
    for item in data["vectors_token_E"]:
        file_id = hex_value(item["file_id"], 20)
        expected = hex_value(item["aes"], 16)
        for version, obfuscated in item["obfuscated"].items():
            vectors.append((file_id, version, hex_value(obfuscated, 16), expected))
    if not vectors:
        raise ValueError("No Token E vectors found")

    # LAN requests should go directly to the chosen server, ignoring proxy env vars.
    opener = request.build_opener(request.ProxyHandler({}), NoRedirect())
    report = {
        "started_at": datetime.now(timezone.utc).isoformat(),
        "url": url,
        "vectors_sha256": hashlib.sha256(raw).hexdigest(),
        "vector_count": len(vectors),
        "repeat": repeat,
        "results": [],
    }
    for run in range(1, repeat + 1):
        for file_id, version, obfuscated, expected in vectors:
            result = {"run": run, "file_id": file_id, "version": version}
            req = request.Request(
                url,
                data=json.dumps({"obfuscated_key": obfuscated}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with opener.open(req, timeout=timeout) as response:
                    result["http_status"] = response.status
                    if response.status != 200:
                        raise ValueError(f"Expected HTTP 200, got {response.status}")
                    body = response.read(65537)
                    if len(body) > 65536:
                        raise ValueError("Response exceeds 64 KiB")
                    payload = json.loads(body)
                if not isinstance(payload, dict):
                    raise ValueError("Response must be a JSON object")
                if "error" in payload or payload.get("success") is False:
                    raise ValueError(f"Service error: {payload.get('error', 'success=false')}")
                actual = hex_value(payload.get("aes_key"), 16)
                result.update(
                    status="pass" if actual == expected else "mismatch",
                    expected=expected,
                    actual=actual,
                )
            except error.HTTPError as exc:
                result.update(status="error", http_status=exc.code, error=str(exc))
            except (error.URLError, OSError, ValueError, HTTPException) as exc:
                result.update(status="error", error=str(exc))
            report["results"].append(result)
            print(
                f"{result['status'].upper()} run={run} {file_id} {version}"
                + (f" ({result['error']})" if "error" in result else ""),
                flush=True,
            )

            # An unavailable/uninitialized service is not evidence of a token mismatch.
            # Stop on infrastructure/protocol errors instead of sending the full batch.
            if result["status"] == "error":
                break
        if report["results"][-1]["status"] == "error":
            break

    counts = Counter(result["status"] for result in report["results"])
    report["summary"] = {
        "planned": len(vectors) * repeat,
        "completed": len(report["results"]),
        **{status: counts[status] for status in ("pass", "mismatch", "error")},
    }
    report["finished_at"] = datetime.now(timezone.utc).isoformat()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="LAN service URL, including /deob")
    parser.add_argument("--vectors", type=Path, default=DEFAULT_VECTORS)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--repeat", type=int, default=1, help="Sequential passes over all vectors")
    parser.add_argument("--report", type=Path, required=True, help="New JSON report path")
    args = parser.parse_args()
    url = urlsplit(args.url)
    if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password:
        parser.error("--url must be an HTTP(S) endpoint without credentials")
    if not 0 < args.timeout < float("inf") or args.repeat < 1:
        parser.error("--timeout must be positive and finite; --repeat must be >= 1")
    try:
        # Exclusive creation avoids overwriting evidence from an earlier run.
        with args.report.open("x", encoding="utf-8") as output:
            report = validate(args.url, args.vectors, args.timeout, args.repeat)
            json.dump(report, output, indent=2)
            output.write("\n")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"Validation could not run: {exc}", file=sys.stderr)
        return 2
    summary = report["summary"]
    print(json.dumps(summary))
    return 2 if summary["error"] else 1 if summary["mismatch"] else 0


if __name__ == "__main__":
    sys.exit(main())
