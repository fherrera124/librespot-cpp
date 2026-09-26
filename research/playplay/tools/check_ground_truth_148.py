#!/usr/bin/env python3
"""Verify the two saved Spotify 1.2.92.148 PlayPlay controls offline.

Run from the repository root; no Windows process, network or credentials needed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from reverse_key_schedule import reverse_key_schedule
from verify_token148_content import IV, first_page_valid

ROOT = Path(__file__).resolve().parents[3]
DEFAULT = ROOT / "research/playplay/data/ground-truth-vectors.json"


def read_json(path):
    raw = path.read_bytes()
    return json.loads(raw), hashlib.sha256(raw).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def hex_bytes(value, size, label):
    require(isinstance(value, str) and re.fullmatch("[0-9a-f]{%d}" % (size * 2), value),
            f"invalid {label}")
    return bytes.fromhex(value)


def verify(fixture_path=DEFAULT):
    fixture, _ = read_json(fixture_path)
    require(fixture["schema_version"] == 2, "unsupported schema")
    require(fixture["build"]["version"] == "1.2.92.148", "unexpected build")
    require(fixture["token"]["version"] == 5, "unexpected token version")
    hex_bytes(fixture["token"]["hex"], 16, "token")
    require(fixture["aes_ctr"] == {"iv": IV.hex(), "ogg_magic_offset": 167}, "unexpected audio CTR settings")
    paths = {}
    for label in ("dfa_verification", "context_faults", "context_dump", "direct_aes"):
        ref = fixture["provenance"][label]
        path = ROOT / ref["file"]
        data, digest = read_json(path)
        require(digest == ref["sha256"], f"{label} source hash mismatch")
        paths[label] = data
    direct = paths["direct_aes"]
    require(fixture["build"]["dll_sha256"] == direct["expected_dll_sha256"], "DLL hash mismatch")
    require(direct["build"] == fixture["build"]["version"], "direct AES build mismatch")
    dfa = {row["run"]: row for row in paths["dfa_verification"]["cases"]}
    faults = {row["run"]: row for row in paths["context_faults"]["cases"]}
    contexts = {row["run"]: row for row in paths["context_dump"]["events"] if row["type"] == "context_dump" and row["state"] == "after_block_1"}
    cases = fixture["cases"]
    require(len(cases) == len(dfa) == len(faults) == len(direct["licenses"]) == 2, "expected exactly two controls")
    seen = set()
    for case in cases:
        file_id = case["file_id"]
        hex_bytes(file_id, 20, "file_id")
        require(file_id not in seen, "duplicate file_id")
        seen.add(file_id)
        key = hex_bytes(case["aes"], 16, "AES")
        hex_bytes(case["obfuscated_key"], 16, "obfuscated_key")
        hex_bytes(case["b4_seq"], 4, "b4_seq")
        k10 = hex_bytes(case["k10"], 16, "K10")
        native = hex_bytes(case["native_block16"], 16, "native block")
        require(reverse_key_schedule(k10) == key, f"K10 reverse schedule mismatch: {file_id}")
        source = case["provenance"]
        license_path = ROOT / source["license"]
        license_data, license_sha = read_json(license_path)
        require(license_sha == source["license_sha256"], f"license hash mismatch: {file_id}")
        direct_row = direct["licenses"][source["direct_aes_entry"]]
        require(source["license"] == "research/playplay/" + direct_row["fixture"] and
                license_sha == direct_row["fixture_sha256"], f"direct AES license link mismatch: {file_id}")
        require((license_data["file_id"], license_data["token"], license_data["version"],
                 license_data["obfuscated_key"], license_data["b4_seq"], license_data["reference_aes"])
                == (file_id, fixture["token"]["hex"], 5, case["obfuscated_key"], case["b4_seq"], case["aes"]),
                f"license fields mismatch: {file_id}")
        run = source["dfa_run"]
        require(run == source["context_run"] and run in dfa and run in faults and run in contexts,
                f"run provenance mismatch: {file_id}")
        require(dfa[run]["recovered_aes"] == case["aes"] and dfa[run]["k10"] == case["k10"]
                and dfa[run]["correct_block"] is True and dfa[run]["ogg_vorbis_crc_valid"] is True,
                f"DFA evidence mismatch: {file_id}")
        require(faults[run]["correct"] == case["native_block16"] == contexts[run]["block"]
                == direct_row["native_block16"], f"native block mismatch: {file_id}")
        require(direct_row["input16"] == case["obfuscated_key"], f"direct AES input mismatch: {file_id}")
        content = case["content"]
        require(content["file"] == license_data["content_file"] and
                content["sha256"] == license_data["content_sha256"] and
                content["bytes"] == license_data["content_bytes"], f"content provenance mismatch: {file_id}")
        encrypted = (ROOT / content["file"]).read_bytes()
        require(len(encrypted) == content["bytes"] == 4096 and
                hashlib.sha256(encrypted).hexdigest() == content["sha256"], f"content bytes/hash mismatch: {file_id}")
        block = Cipher(algorithms.AES(key), modes.ECB()).encryptor().update(IV)
        require(block == native, f"AES native block mismatch: {file_id}")
        decryptor = Cipher(algorithms.AES(key), modes.CTR(IV)).decryptor()
        plain = decryptor.update(encrypted) + decryptor.finalize()
        require(first_page_valid(plain), f"Ogg/Vorbis CRC mismatch: {file_id}")
    require({row["fixture"] for row in direct["licenses"].values()} ==
            {case["provenance"]["license"].removeprefix("research/playplay/") for case in cases},
            "fixture set mismatch")
    return {"build": fixture["build"]["version"], "cases": len(cases), "verified": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=DEFAULT)
    args = parser.parse_args()
    print(json.dumps(verify(args.fixture)))


if __name__ == "__main__":
    main()
