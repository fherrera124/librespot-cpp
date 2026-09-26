#!/usr/bin/env python3
"""Disassemble a short RVA window of the exact raw Spotify 1.2.92.148 dump."""

import argparse
import hashlib
from pathlib import Path

DUMP = Path(__file__).resolve().parents[2] / 'dlls/Spotify_1.2.92.148_dump.dll'
SHA256 = '275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rva', type=lambda value: int(value, 0))
    parser.add_argument('--size', type=lambda value: int(value, 0), default=0x80)
    args = parser.parse_args()
    if not 1 <= args.size <= 0x1000:
        parser.error('size must be 1..0x1000')
    raw = DUMP.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SHA256:
        parser.error(f'unexpected dump SHA256: {digest}')
    if not 0 <= args.rva < len(raw) or args.rva + args.size > len(raw):
        parser.error('RVA window outside dump')
    try:
        import capstone
    except ImportError as error:
        parser.error(f'capstone required: {error}')
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    print(f'dump={DUMP} sha256={digest} layout=raw-offset-equals-rva')
    print('A window starting inside an instruction can decode incorrectly; verify boundaries.')
    for insn in decoder.disasm(raw[args.rva:args.rva + args.size], args.rva):
        print(f'{insn.address:#x}: {insn.mnemonic} {insn.op_str}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
