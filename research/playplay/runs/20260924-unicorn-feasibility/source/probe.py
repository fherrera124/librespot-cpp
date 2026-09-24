"""Bounded, strict offline feasibility probe. No fake APIs or missing-page fills.

Run from repository root with research/playplay/ppvenv/bin/python.
The historical contexts/descriptors are inputs; reference AES is never used here.
"""
from pathlib import Path
import collections
import hashlib
import json
import struct
import time
import capstone
import pefile
import unicorn
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import UC_X86_REG_RIP, UC_X86_REG_RSP, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9

ROOT = Path.cwd()
RUN = Path(__file__).resolve().parents[1]
DUMP = ROOT / 'research/dlls/Spotify_1.2.92.148_dump.dll'
raw = DUMP.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
assert digest == '275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc'
pe = pefile.PE(data=raw, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase
capture = json.loads((ROOT / 'research/playplay/docs/context_dump_2026-09-24.json').read_text())
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
STACK, DATA, RETURN = 0x1000000, 0x2000000, 0x4000000


def machine(base):
    mu = Uc(UC_ARCH_X86, UC_MODE_64)
    mu.mem_map(base, (len(raw) + 4095) & ~4095)
    mu.mem_write(base, raw)
    mu.mem_map(STACK, 0x200000)
    mu.mem_map(DATA, 0x100000)
    mu.mem_map(RETURN, 4096)
    return mu


def call(mu, base, rva, args):
    trace = collections.deque(maxlen=12)
    faults = []
    executed = 0

    def code(uc, address, size, _):
        nonlocal executed
        executed += 1
        trace.append(address)

    def missing(uc, access, address, size, value, _):
        faults.append(dict(access=access, address=hex(address), size=size, rip=hex(uc.reg_read(UC_X86_REG_RIP))))
        return False

    hooks = [mu.hook_add(UC_HOOK_CODE, code), mu.hook_add(UC_HOOK_MEM_UNMAPPED, missing)]
    rsp = STACK + 0x200000 - 0x1008
    mu.mem_write(rsp, struct.pack('<Q', RETURN))
    mu.reg_write(UC_X86_REG_RSP, rsp)
    for reg, value in zip((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), list(args) + [0] * (4 - len(args))):
        mu.reg_write(reg, value)
    result = dict(rva=hex(rva))
    start = time.monotonic()
    try:
        mu.emu_start(base + rva, RETURN, timeout=2_000_000, count=2_000_000)
        result['status'] = 'returned' if mu.reg_read(UC_X86_REG_RIP) == RETURN else 'limit-reached'
    except UcError as exc:
        result.update(status='failed', error=str(exc))
    result.update(seconds=round(time.monotonic() - start, 5), instructions=executed, rip=hex(mu.reg_read(UC_X86_REG_RIP)), faults=faults)
    result['tail'] = []
    for address in trace:
        try:
            ins = next(md.disasm(bytes(mu.mem_read(address, 15)), address))
            result['tail'].append(dict(rva=hex(address-base), instruction=ins.mnemonic+' '+ins.op_str))
        except (StopIteration, UcError):
            result['tail'].append(dict(address=hex(address)))
    for hook in hooks:
        mu.hook_del(hook)
    return result


results = []
for base in (0x180000000, image_base):
    for initial in [e for e in capture['events'] if e.get('state') == 'initial']:
        expected = next(e['block'] for e in capture['events'] if e.get('run') == initial['run'] and e.get('state') == 'after_block_1')
        mu = machine(base)
        mu.mem_write(DATA, bytes.fromhex(initial['context']))
        result = call(mu, base, 0xd9d0f0, [DATA, DATA + 0x2000])
        result.update(case='stream-from-saved-context', run=initial['run'], base=hex(base), output=bytes(mu.mem_read(DATA + 0x2000,16)).hex(), expected=expected)
        result['match'] = result['status'] == 'returned' and result['output'] == expected
        results.append(result)
        print(json.dumps({k:v for k,v in result.items() if k!='tail'}), flush=True)

        mu = machine(base)
        mu.mem_write(DATA + 0x3000, bytes.fromhex(initial['wrapped28']))
        result = call(mu, base, 0xd9e2e4, [DATA, DATA + 0x3000, DATA + 0x4000])
        result.update(case='init-from-saved-descriptor', run=initial['run'], base=hex(base))
        if result['status'] == 'returned':
            result['stream'] = call(mu, base, 0xd9d0f0, [DATA, DATA + 0x2000])
            result['output'] = bytes(mu.mem_read(DATA+0x2000,16)).hex()
            result['match'] = result['stream']['status'] == 'returned' and result['output'] == expected
        results.append(result)
        print(json.dumps({k:v for k,v in result.items() if k!='tail'}), flush=True)

    mu = machine(base)
    mu.mem_write(DATA + 0x70, b'\x01')
    mu.mem_write(DATA + 0x1000, bytes.fromhex('7a154493af30b49d753cd246d6e9e83a'))
    mu.mem_write(DATA + 0x2000, bytes.fromhex('3a034bf8'))
    result = call(mu, base, 0x4a0268, [DATA, DATA + 0x1000, DATA + 0x2000, 0])
    result.update(case='pipeline-correct-contract', base=hex(base))
    results.append(result)
    print(json.dumps({k:v for k,v in result.items() if k!='tail'}), flush=True)

report = dict(dump=str(DUMP.relative_to(ROOT)), dump_sha256=digest, header_image_base=hex(image_base), unicorn_version=unicorn.__version__, cases=results)
with (RUN/'raw/strict-probe.json').open('x') as f:
    json.dump(report,f,indent=2);f.write('\n')
