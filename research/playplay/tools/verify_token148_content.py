#!/usr/bin/env python3
"""Independently compare native streams to AES-128-CTR and verify Ogg/Vorbis.

Run from the repository root (content_file in license reports is relative to it).
No Windows connection or credentials are required for this verification.
"""
import argparse
import hashlib
import json
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

IV = bytes.fromhex("72e067fbddcbcf77ebe8bc643f630d93")


def first_page_valid(plain):
    page = bytearray(plain[167:])
    if len(page) < 28 or page[:6] != b"OggS\x00\x02" or page[26] == 0:
        return False
    packet = 27 + page[26]
    if len(page) < packet or page[packet:packet + 7] != b"\x01vorbis":
        return False
    size = packet + sum(page[27:packet])
    if size > len(page):
        return False
    page = page[:size]
    expected = int.from_bytes(page[22:26], "little")
    page[22:26] = bytes(4)
    crc = 0
    for byte in page:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc == expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", type=Path, required=True)
    parser.add_argument("--license", type=Path, action="append", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    vm_raw = args.vm.read_bytes()
    vm = json.loads(vm_raw)
    events = vm["events"]
    if vm.get("error") or any(e["type"] == "fatal" for e in events) or not any(e["type"] == "done" for e in events):
        raise ValueError("VM run failed or incomplete")
    rows = [e for e in events if e["type"] == "pipeline_control" and e["aes_known"]]
    results = []
    for license_path in args.license:
        license_raw = license_path.read_bytes()
        license = json.loads(license_raw)
        encrypted = Path(license["content_file"]).read_bytes()
        if hashlib.sha256(encrypted).hexdigest() != license["content_sha256"]:
            raise ValueError("Content hash mismatch")
        aes = bytes.fromhex(license["reference_aes"])
        cipher = Cipher(algorithms.AES(aes), modes.CTR(IV))
        decrypt = cipher.decryptor()
        expected_plain = decrypt.update(encrypted) + decrypt.finalize()
        encrypt = cipher.encryptor()
        expected_stream = encrypt.update(bytes(len(encrypted))) + encrypt.finalize()
        matched = [r for r in rows if r["file_id"] == license["file_id"]]
        if not matched:
            raise ValueError("Missing resource in VM report")
        for row in matched:
            native = bytes.fromhex("".join(row["blocks"]))
            repeated = bytes.fromhex("".join(row["repeat_blocks"]))
            plain = bytes(a ^ b for a, b in zip(encrypted, native))
            candidate_decrypt = Cipher(algorithms.AES(bytes.fromhex(row["candidate"])), modes.CTR(IV)).decryptor()
            candidate_plain = candidate_decrypt.update(encrypted) + candidate_decrypt.finalize()
            result = dict(label=row["label"], file_id=license["file_id"],
                          license_sha256=hashlib.sha256(license_raw).hexdigest(),
                          content_sha256=license["content_sha256"], bytes=len(encrypted),
                          native_matches_aes128ctr=native == expected_stream,
                          repeated_stream_matches=native == repeated,
                          plaintext_matches_reference=plain == expected_plain,
                          vorbis_ogg_crc_valid=first_page_valid(plain),
                          candidate_is_reference_aes=row["candidate"] == license["reference_aes"],
                          candidate_vorbis_ogg_crc_valid=first_page_valid(candidate_plain))
            result["pass"] = all(result[k] for k in (
                "native_matches_aes128ctr", "repeated_stream_matches",
                "plaintext_matches_reference", "vorbis_ogg_crc_valid"))
            results.append(result)
    report = dict(vm_report=str(args.vm), vm_sha256=hashlib.sha256(vm_raw).hexdigest(),
                  cases=results, all_pass=bool(results) and all(r["pass"] for r in results))
    with args.report.open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report, indent=2))
    return 0 if report["all_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
