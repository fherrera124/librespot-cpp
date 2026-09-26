#!/usr/bin/env python3
"""List direct-call candidates to known VM entries in the exact raw 148 dump.

A matching E8 byte may lie in data or in another instruction. Confirm each
candidate with disassembly and a function boundary before using it as a callsite.
"""

import argparse
import hashlib
from pathlib import Path
import struct

DUMP = Path(__file__).resolve().parents[2] / 'dlls/Spotify_1.2.92.148_dump.dll'
SHA256 = '275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc'
TARGETS = {0x49CB88: 'VM init', 0x49EAA4: 'VM transform',
           0x49F854: 'candidate consumer', 0xD9E2E4: 'stream init',
           0xD9D0F0: 'block generator'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--full', action='store_true', help='include all known targets')
    args = parser.parse_args()
    raw = DUMP.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SHA256:
        parser.error(f'unexpected dump SHA256: {digest}')
    targets = TARGETS if args.full else {rva: TARGETS[rva] for rva in (0x49CB88, 0x49EAA4)}
    print(f'dump={DUMP} sha256={digest} layout=raw-offset-equals-rva')
    for target, label in targets.items():
        hits = []
        offset = 0
        while (offset := raw.find(b'\xe8', offset)) != -1:
            if offset + 5 <= len(raw):
                destination = offset + 5 + struct.unpack_from('<i', raw, offset + 1)[0]
                if destination == target:
                    hits.append(offset)
            offset += 1
        print(f'{label} {target:#x}: ' + ', '.join(f'{hit:#x}' for hit in hits))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
