"""Probe captured descriptor init with one explicitly identified copy stub.

Reuse only setup/definitions of context_faults.py via AST, without its experiment
loop or output writer. The identified copy routine is 0x17780e0 (see playbook).
All other unmapped accesses/functions still fail; no generic success stubs.
"""
import ast
import json
from pathlib import Path
from unicorn import UC_HOOK_CODE, UcError
from unicorn.x86_const import UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_RAX, UC_X86_REG_RSP, UC_X86_REG_RIP

setup_path = Path(__file__).with_name('context_faults.py')
tree = ast.parse(setup_path.read_text())
end = next(i for i,n in enumerate(tree.body) if isinstance(n, ast.For))
tree.body = tree.body[:end]
ns = {'__file__': str(setup_path)}
exec(compile(tree, str(setup_path), 'exec'), ns)
base, data = ns['base'], ns['DATA']
results=[]
for initial in [e for e in ns['capture']['events'] if e.get('state') == 'initial']:
    uc=ns['machine']();calls=[]

    def copy(uc,address,size,user):
        dst,src,n=(uc.reg_read(r) for r in (UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8))
        if n > 0x100000:
            raise ValueError('Unexpected copy size')
        calls.append(dict(dst=hex(dst),src=hex(src),size=n))
        uc.mem_write(dst,bytes(uc.mem_read(src,n)))
        rsp=uc.reg_read(UC_X86_REG_RSP)
        ret=int.from_bytes(uc.mem_read(rsp,8),'little')
        uc.reg_write(UC_X86_REG_RAX,dst)
        uc.reg_write(UC_X86_REG_RSP,rsp+8)
        uc.reg_write(UC_X86_REG_RIP,ret)

    uc.hook_add(UC_HOOK_CODE,copy,begin=base+0x17780e0,end=base+0x17780e0)
    uc.mem_write(data+0x3000,bytes.fromhex(initial['wrapped28']))
    result=dict(run=initial['run'],copy_calls=calls)
    try:
        ns['call'](uc,0xd9e2e4,[data,data+0x3000,data+0x4000])
        ns['call'](uc,0xd9d0f0,[data,data+0x2000])
        output=bytes(uc.mem_read(data+0x2000,16)).hex()
        expected=next(e['block'] for e in ns['capture']['events'] if e.get('run')==initial['run'] and e.get('state')=='after_block_1')
        result.update(status='returned',output=output,expected=expected,match=output==expected)
    except (UcError,RuntimeError,ValueError) as exc:
        result.update(status='failed',error=str(exc),rip=hex(uc.reg_read(UC_X86_REG_RIP)),rva=hex(uc.reg_read(UC_X86_REG_RIP)-base))
    print(json.dumps(result),flush=True);results.append(result)
with (ns['run']/'raw/init-copy-probe.json').open('x') as f:
    json.dump(dict(cases=results),f,indent=2);f.write('\n')
