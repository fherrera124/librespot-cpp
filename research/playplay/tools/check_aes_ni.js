const mod = Process.getModuleByName('Spotify.dll');
const start = mod.base.add(0xd9d0f0);
let found = false;
let cursor = start;
// simplistic linear sweep for 0x1000 bytes just to see
for (let i = 0; i < 2000; i++) {
    try {
        const insn = Instruction.parse(cursor);
        if (insn.mnemonic.startsWith('aes')) {
            send({type: 'aes_ni_found', address: cursor.toString(), mnemonic: insn.mnemonic, ops: insn.opStr});
            found = true;
        }
        cursor = insn.next;
    } catch (e) {
        cursor = cursor.add(1);
    }
}
if (!found) send({type: 'no_aes_ni_in_linear_sweep'});
send({type: 'done'});
