"""Generate faults offline from saved contexts; also probe init with a minimal TEB.

Uses no reference keys, Frida, network, fake API returns or demand-mapped pages.
Run with the existing ppvenv from repository root.
"""
from pathlib import Path
import hashlib
import json
import struct
import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import UC_X86_REG_RIP, UC_X86_REG_RSP, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_GS_BASE

run = Path(__file__).resolve().parents[1]
raw = Path('research/dlls/Spotify_1.2.92.148_dump.dll').read_bytes()
assert hashlib.sha256(raw).hexdigest() == '275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc'
base = pefile.PE(data=raw, fast_load=True).OPTIONAL_HEADER.ImageBase
capture_path = Path('research/playplay/docs/context_dump_2026-09-24.json')
capture = json.loads(capture_path.read_text())
STACK, DATA, END, TEB, PEB = 0x1000000, 0x2000000, 0x4000000, 0x5000000, 0x5010000


def machine():
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(base, (len(raw)+4095)&~4095)
    uc.mem_write(base, raw)
    for address, size in [(STACK,0x200000),(DATA,0x100000),(END,4096),(TEB,4096),(PEB,4096)]:
        uc.mem_map(address,size)
    uc.reg_write(UC_X86_REG_GS_BASE,TEB)
    uc.mem_write(TEB+0x30,struct.pack('<Q',TEB))
    uc.mem_write(TEB+0x60,struct.pack('<Q',PEB))
    return uc


def call(uc,rva,args):
    rsp=STACK+0x200000-0x1008
    uc.mem_write(rsp,struct.pack('<Q',END));uc.reg_write(UC_X86_REG_RSP,rsp)
    for reg,value in zip((UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9),list(args)+[0]*(4-len(args))):
        uc.reg_write(reg,value)
    uc.emu_start(base+rva,END,timeout=2_000_000,count=2_000_000)
    if uc.reg_read(UC_X86_REG_RIP)!=END:
        raise RuntimeError('execution limit reached')


results=[]
for initial in [e for e in capture['events'] if e.get('state')=='initial']:
    uc=machine();context=bytes.fromhex(initial['context'])
    uc.mem_write(DATA,context);call(uc,0xd9d0f0,[DATA,DATA+0x2000])
    correct=bytes(uc.mem_read(DATA+0x2000,16)).hex()
    faults=[]
    for offset in range(0xa0,0xb0):
        changed=bytearray(context);changed[offset]^=1
        uc.mem_write(DATA,bytes(changed));call(uc,0xd9d0f0,[DATA,DATA+0x2000])
        faults.append(dict(offset=hex(offset),ct=bytes(uc.mem_read(DATA+0x2000,16)).hex()))
    result=dict(run=initial['run'],correct=correct,faults=faults)
    # A separate machine tests whether a minimal TEB is enough for descriptor init.
    uc=machine();uc.mem_write(DATA+0x3000,bytes.fromhex(initial['wrapped28']))
    try:
        call(uc,0xd9e2e4,[DATA,DATA+0x3000,DATA+0x4000])
        call(uc,0xd9d0f0,[DATA,DATA+0x2000])
        output=bytes(uc.mem_read(DATA+0x2000,16)).hex()
        result['minimal_teb_init']=dict(status='returned',output=output,match=output==correct)
    except (UcError,RuntimeError) as exc:
        rip=uc.reg_read(UC_X86_REG_RIP)
        result['minimal_teb_init']=dict(status='failed',error=str(exc),rip=hex(rip),rva=hex(rip-base))
    results.append(result)
    print(initial['run'], 'faults=',len(faults),'init=',result['minimal_teb_init'],flush=True)
with (run/'raw/context-faults.json').open('x') as f:
    json.dump(dict(capture_sha256=hashlib.sha256(capture_path.read_bytes()).hexdigest(),cases=results),f,indent=2);f.write('\n')
