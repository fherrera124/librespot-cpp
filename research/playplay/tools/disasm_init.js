const mod = Process.getModuleByName('Spotify.dll');
let cursor = mod.base.add(0xd9e2e4);
const end = cursor.add(0x200);
let output = [];
while (cursor.compare(end) < 0) {
    try {
        const insn = Instruction.parse(cursor);
        output.push(insn.address.sub(mod.base).toString(16) + ": " + insn.mnemonic + " " + insn.opStr);
        if (insn.mnemonic === 'ret') break;
        cursor = insn.next;
    } catch(e) {
        break;
    }
}
send({type: 'disasm', data: output});
