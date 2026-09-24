"""Independent offline DFA/content evaluation, using system Python + cryptography.

Load only math definitions from the existing extractor; do not import Frida.
"""
import ast
import hashlib
import json
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

run=Path(__file__).resolve().parents[1]
tools=Path('research/playplay/tools')
source=(tools/'extractor.py').read_text()
module=ast.parse(source)
module.body=[n for n in module.body if isinstance(n,(ast.Assign,ast.For,ast.FunctionDef))]
math={};exec(compile(module,'extractor-math-only','exec'),math)
reverse={'__name__':'audit'};exec((tools/'reverse_key_schedule.py').read_text(),reverse)
verify={'__name__':'audit'};exec((tools/'verify_token148_content.py').read_text(),verify)
licenses={
    'test_2f43127d_b4':'token148-license-20260924T164814Z-retry.json',
    'test_1a8e5b04_b4':'token148-license-second-20260924T164814Z.json',
}
faults_path=run/'raw/context-faults.json'
results=[]
for row in json.loads(faults_path.read_text())['cases']:
    k10=math['solve_dfa'](row['correct'],row['faults']);assert k10 is not None
    key=reverse['reverse_key_schedule'](list(k10))
    license=json.loads((Path('research/playplay/docs')/licenses[row['run']]).read_text())
    content=Path(license['content_file']).read_bytes()
    assert hashlib.sha256(content).hexdigest()==license['content_sha256']
    cipher=Cipher(algorithms.AES(key),modes.CTR(verify['IV'])).decryptor()
    plain=cipher.update(content)+cipher.finalize()
    ecb=Cipher(algorithms.AES(key),modes.ECB()).encryptor()
    first=(ecb.update(verify['IV'])+ecb.finalize()).hex()
    result=dict(run=row['run'],recovered_aes=key.hex(),k10=k10.hex(),
        correct_block=first==row['correct'],matches_reference=key.hex()==license['reference_aes'],
        content_bytes=len(content),ogg_vorbis_crc_valid=verify['first_page_valid'](plain))
    assert all(result[k] for k in ('correct_block','matches_reference','ogg_vorbis_crc_valid'))
    results.append(result);print(json.dumps(result),flush=True)
with (run/'raw/dfa-verification.json').open('x') as f:
    json.dump(dict(faults_sha256=hashlib.sha256(faults_path.read_bytes()).hexdigest(),
        extractor_sha256=hashlib.sha256(source.encode()).hexdigest(),cases=results),f,indent=2);f.write('\n')
