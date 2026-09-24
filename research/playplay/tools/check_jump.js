const mod = Process.getModuleByName('Spotify.dll');

setImmediate(() => {
    try {
        const jumpTarget = mod.base.add(0x211f388);
        const mem = Memory.readByteArray(jumpTarget, 32);
        const bytes = Array.from(new Uint8Array(mem), b => b.toString(16).padStart(2, '0')).join(' ');
        send({type: 'done', bytes: bytes});
    } catch(e) {
        send({type: 'error', stack: e.toString()});
    }
});
