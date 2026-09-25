"""Comprueba el anexo crudo de la búsqueda directa AES en Spotify 148."""

import hashlib
import json
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

from reverse_key_schedule import RCON, SBOX


ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "docs/DIRECT_AES_148_DATA.json"
IV = bytes.fromhex("72e067fbddcbcf77ebe8bc643f630d93")


def round_keys(key):
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        previous = words[-1][:]
        if i % 4 == 0:
            previous = [SBOX[b] for b in previous[1:] + previous[:1]]
            previous[0] ^= RCON[i // 4]
        words.append([a ^ b for a, b in zip(words[-4], previous)])
    return [bytes(sum(words[4 * i:4 * i + 4], [])) for i in range(11)]


def windows(data, size):
    return (data[i:i + size] for i in range(len(data) - size + 1))


def main():
    assert round_keys(bytes.fromhex("000102030405060708090a0b0c0d0e0f"))[1].hex() == (
        "d6aa74fdd2af72fadaa678f1d6ab76fe")
    capture = json.loads(DATA.read_text(encoding="utf-8"))
    shared = [bytes.fromhex(value) for value in capture["vm_shared"].values()]
    assert [len(value) for value in shared] == [3072, 512]

    blocks_ok = 0
    matches = []
    checked16 = checked8 = 0
    for label, item in capture["licenses"].items():
        fixture_path = ROOT / item["fixture"]
        fixture_bytes = fixture_path.read_bytes()
        assert hashlib.sha256(fixture_bytes).hexdigest() == item["fixture_sha256"]
        fixture = json.loads(fixture_bytes)
        assert item["input16"] == fixture["obfuscated_key"]
        key = bytes.fromhex(fixture["reference_aes"])
        rounds = round_keys(key)
        assert len(rounds) == 11
        block = Cipher(algorithms.AES(key), modes.ECB()).encryptor().update(IV)
        blocks_ok += block.hex() == item["native_block16"]

        fields = shared + [bytes.fromhex(item[name]) for name in (
            "input16", "init16", "candidate16", "context740", "native_block16")]
        fields.extend(bytes.fromhex(value) for value in item["descriptors28"])
        fields.extend(bytes.fromhex(value) for value in item["copy_source16_unique"])
        assert len(bytes.fromhex(item["context740"])) == 740
        for field in fields:
            full = set(windows(field, 16))
            half = set(windows(field, 8))
            checked16 += len(field) - 15 if len(field) >= 16 else 0
            checked8 += len(field) - 7 if len(field) >= 8 else 0
            for number, round_key in enumerate(rounds):
                if round_key in full or round_key[:8] in half or round_key[8:] in half:
                    matches.append((label, number))

    assert blocks_ok == len(capture["licenses"]), "Bloque nativo distinto de AES(K0, IV)"
    print(f"bloques={blocks_ok}/{len(capture['licenses'])} ventanas16={checked16} "
          f"ventanas8={checked8} coincidencias={matches}")
    return 1 if matches else 0


if __name__ == "__main__":
    raise SystemExit(main())
