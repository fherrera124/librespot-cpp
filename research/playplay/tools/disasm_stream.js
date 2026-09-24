const mod = Process.getModuleByName('Spotify.dll');
let cursor = mod.base.add(0xd9d0f0);
const end = cursor.add(0x200);
let output = [];
while (cursor.compare(end) < 0) {
    try {
        const insn = Instruction.parse(cursor);
        output.push(insn.address.sub(mod.base).toString(16) + ": " + insn.mnemonic + " " + insn.opStr);
        if (insn.mnemonic === 'ret') break;
        if (insn.mnemonic === 'jmp' && !insn.opStr.startsWith('0x')) {
            // indirect jmp?
        }
        cursor = insn.next;
    } catch(e) {
        break;
    }
}
send({type: 'disasm', data: output});
