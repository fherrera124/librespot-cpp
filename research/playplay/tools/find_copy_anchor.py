#!/usr/bin/env python3
"""Locate the MSVC copy routine in the raw build-148 memory dump."""

import hashlib
import json
from pathlib import Path
import re

EXPECTED_SHA256 = "275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc"


def main():
    path = Path(__file__).resolve().parents[2] / "dlls/Spotify_1.2.92.148_dump.dll"
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"unexpected dump SHA256: {digest}")
    # Wildcard RIP-relative and branch displacements; preserve size dispatch.
    prefix = bytes.fromhex("48 8b c1 4c 8d 15")
    suffix = bytes.fromhex("49 83 f8 0f 0f 87")
    pattern = re.escape(prefix) + b".{4}" + re.escape(suffix) + b".{4}"
    hits = [match.start() for match in re.finditer(pattern, raw, re.DOTALL)]
    report = {"file": str(path), "layout": "raw_memory_dump",
              "sha256": digest,
              "pattern": "48 8b c1 4c 8d 15 ?? ?? ?? ?? 49 83 f8 0f 0f 87 ?? ?? ?? ??",
              "rvas": [hex(rva) for rva in hits]}
    print(json.dumps(report, indent=2))
    return 0 if len(hits) == 1 else 1


if __name__ == '__main__':
    raise SystemExit(main())
