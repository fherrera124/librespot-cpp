"""Check candidate keys against small encrypted Spotify audio-cache prefixes."""
import argparse
import json
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

IV = bytes.fromhex('72e067fbddcbcf77ebe8bc643f630d93')


def has_vorbis_header(ciphertext, key):
    decryptor = Cipher(algorithms.AES(key), modes.CTR(IV)).decryptor()
    plain = decryptor.update(ciphertext) + decryptor.finalize()
    offset = 167
    if len(plain) < offset + 28 or plain[offset:offset + 6] != b'OggS\x00\x02':
        return False
    segments = plain[offset + 26]
    packet = offset + 27 + segments
    return segments > 0 and plain[packet:packet + 7] == b'\x01vorbis'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache-dir', type=Path, required=True)
    parser.add_argument('--keys-json', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    candidates = json.loads(args.keys_json.read_text())['candidates']
    for item in candidates:
        if len(bytes.fromhex(item['key'])) != 16:
            parser.error('Each key must contain 16 bytes')
    if not args.cache_dir.is_dir():
        parser.error('Cache directory does not exist')
    report = {'cache_dir': str(args.cache_dir), 'tested_files': 0, 'matches': [],
              'errors': [], 'key_labels': [item['label'] for item in candidates]}
    with args.report.open('x', encoding='utf-8') as output:
        for path in sorted(args.cache_dir.glob('*/*.file')):
            try:
                with path.open('rb') as stream:
                    prefix = stream.read(512)
                report['tested_files'] += 1
                for item in candidates:
                    if has_vorbis_header(prefix, bytes.fromhex(item['key'])):
                        report['matches'].append({'file': str(path), 'key_label': item['label'],
                                                  'ogg_offset': 167, 'vorbis_identification': True})
            except OSError as exc:
                report['errors'].append({'file': str(path), 'error': str(exc)})
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps(report))
    return 0 if report['matches'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
