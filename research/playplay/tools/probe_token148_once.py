#!/usr/bin/env python3
"""Request one reference license locally; never persist or send account tokens to Windows.

Wire fields and login flow follow protobuf/*.proto and CredentialsResolver.cpp.
Only the explicit report (license/reference data) and optional encrypted CDN prefix
are written. An existing report is never overwritten.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time

import requests

TOKEN = "02d29f82a8396930aab0a5885c81da7a"
CLIENT_ID = "65b708073fc0480ea92a077233ca87bd"


def varint(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128)
        n >>= 7
    out.append(n)
    return bytes(out)


def field(n, value):
    if isinstance(value, int):
        return varint(n << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint((n << 3) | 2) + varint(len(value)) + value


def decode(data):
    pos = 0
    out = {}

    def read_varint():
        nonlocal pos
        value = 0
        for shift in range(0, 70, 7):
            if pos >= len(data):
                raise ValueError("Truncated protobuf varint")
            byte = data[pos]
            pos += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
        raise ValueError("Oversized protobuf varint")

    while pos < len(data):
        tag = read_varint()
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError("Invalid protobuf field number")
        if wire == 0:
            value = read_varint()
        elif wire in (1, 2, 5):
            size = read_varint() if wire == 2 else (8 if wire == 1 else 4)
            if pos + size > len(data):
                raise ValueError("Truncated protobuf field")
            value = data[pos:pos + size]
            pos += size
        else:
            raise ValueError("Unsupported protobuf wire type")
        if number not in out:
            out[number] = value
        elif isinstance(out[number], list):
            out[number].append(value)
        else:
            out[number] = [out[number], value]
    return out


def authenticate(http, session_path):
    saved = json.loads(session_path.read_text())
    sdk = field(1, field(5, b"")) + field(2, saved["deviceId"])
    client_data = field(1, "0.1.0") + field(2, CLIENT_ID) + field(3, sdk)
    headers = {"Accept": "application/x-protobuf", "Content-Type": "application/x-protobuf"}
    response = http.post("https://clienttoken.spotify.com/v1/clienttoken",
                         data=field(1, 1) + field(2, client_data), headers=headers,
                         timeout=20, allow_redirects=False)
    if response.status_code != 200:
        raise ValueError(f"Client token HTTP {response.status_code}")
    granted = decode(response.content)
    if granted.get(1) != 1 or 2 not in granted:
        raise ValueError("Client token was not granted")
    client_token = decode(granted[2])[1].decode()
    client_info = field(1, CLIENT_ID) + field(2, saved["deviceId"])
    stored = field(1, saved["username"]) + field(2, base64.b64decode(saved["blob"], validate=True))
    response = http.post("https://login5.spotify.com/v3/login",
                         data=field(1, client_info) + field(100, stored),
                         headers={**headers, "Client-Token": client_token},
                         timeout=20, allow_redirects=False)
    if response.status_code != 200:
        raise ValueError(f"Login5 HTTP {response.status_code}")
    login = decode(response.content)
    if 1 not in login:
        raise ValueError(f"Login5 not granted; error enum={login.get(2)}")
    bearer = decode(login[1])[2].decode()
    return {"Client-Token": client_token, "Authorization": "Bearer " + bearer}


def probe(args, report):
    http = requests.Session()
    http.trust_env = False
    headers = authenticate(http, args.session)
    report["authenticated"] = True
    resolved = http.get("https://apresolve.spotify.com/?type=spclient", timeout=20)
    resolved.raise_for_status()
    host = resolved.json()["spclient"][0]
    if not re.fullmatch(r"[a-z0-9-]+\.spotify\.com(?::443)?", host):
        raise ValueError("Unexpected spclient host")
    body = (field(1, 5) + field(2, bytes.fromhex(TOKEN))
            + field(4, args.interactivity) + field(5, 1) + field(6, int(time.time())))
    report.update(host=host, request_hex=body.hex())
    response = http.post(f"https://{host}/playplay/v1/key/{args.file_id}", data=body,
                         headers={**headers, "Content-Type": "application/x-protobuf"},
                         timeout=20, allow_redirects=False)
    report.update(http_status=response.status_code, response_hex=response.content.hex())
    if response.status_code != 200:
        return
    license_fields = decode(response.content)
    key = license_fields.get(1, b"")
    seq = license_fields.get(2, b"")
    if not isinstance(key, bytes) or len(key) != 16 or not isinstance(seq, bytes) or len(seq) != 4:
        raise ValueError("Unexpected license field lengths")
    report.update(obfuscated_key=key.hex(), b4_seq=seq.hex())
    if not args.content:
        return
    storage = http.get(f"https://{host}/storage-resolve/files/audio/interactive/{args.file_id}",
                       params={"alt": "json", "product": 9}, headers=headers,
                       timeout=20, allow_redirects=False)
    report["storage_status"] = storage.status_code
    if storage.status_code != 200:
        return
    cdn_url = storage.json()["cdnurl"][0]
    if not cdn_url.startswith("https://"):
        raise ValueError("Non-HTTPS CDN URL")
    # Account headers are never passed to the CDN. Read only a small prefix.
    with http.get(cdn_url, headers={"Range": "bytes=0-4095"}, stream=True, timeout=20) as cdn:
        report["cdn_status"] = cdn.status_code
        if cdn.status_code not in (200, 206):
            return
        if cdn.status_code == 206 and not cdn.headers.get("Content-Range", "").startswith("bytes 0-"):
            raise ValueError("CDN returned a range with the wrong starting offset")
        chunk = cdn.raw.read(4096)
    with args.content.open("xb") as target:
        target.write(chunk)
    report.update(content_file=str(args.content), content_bytes=len(chunk),
                  content_sha256=hashlib.sha256(chunk).hexdigest())
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    cipher = Cipher(algorithms.AES(bytes.fromhex(args.aes)),
                    modes.CTR(bytes.fromhex("72e067fbddcbcf77ebe8bc643f630d93")))
    decryptor = cipher.decryptor()
    plain = decryptor.update(chunk) + decryptor.finalize()
    start = 167
    ogg = plain[start:]
    packet = 27 + ogg[26] if len(ogg) >= 27 else 0
    report["reference_content_valid"] = bool(
        len(ogg) >= 27 and ogg[:4] == b"OggS" and ogg[4] == 0
        and ogg[5] & 2 and ogg[packet:packet + 7] == b"\x01vorbis")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--file-id", default="2f43127d80edc9cd9f12f441e1cb7904b680f9da")
    parser.add_argument("--aes", default="a503a84c1dc9271460cc13f142e0bae2")
    parser.add_argument("--interactivity", type=int, choices=(1, 3), default=1)
    parser.add_argument("--content", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.file_id) or not re.fullmatch(r"[0-9a-f]{32}", args.aes):
        parser.error("Invalid file ID or reference AES")
    if args.content and args.content.exists():
        parser.error("Content output already exists")
    report = dict(started_at=datetime.now(timezone.utc).isoformat(), token=TOKEN,
                  version=5, file_id=args.file_id, reference_aes=args.aes,
                  interactivity=args.interactivity, content_type=1)
    with args.report.open("x") as output:
        try:
            probe(args, report)
        except Exception as exc:
            # Network exceptions can contain signed URLs; do not serialize them.
            report["error_type"] = type(exc).__name__
            if isinstance(exc, ValueError) and not isinstance(exc, requests.RequestException):
                report["error"] = str(exc)
        finally:
            report["finished_at"] = datetime.now(timezone.utc).isoformat()
            json.dump(report, output, indent=2)
            output.write("\n")
    print(json.dumps({k: report[k] for k in (
        "authenticated", "http_status", "storage_status", "cdn_status",
        "reference_content_valid", "error_type", "error") if k in report}))
    if "obfuscated_key" not in report or "error_type" in report:
        return 2
    if args.content and not report.get("reference_content_valid"):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
