// Search known AES values at all memcpy sources of at least 8 bytes.
// An 8-byte copy can expose a 16-byte key at its source in the reference build.
const mod = Process.findModuleByName('Spotify.dll');
if (mod === null) throw new Error('Spotify.dll not loaded');
const transformAddress = mod.base.add(0x49eaa4);
const copyAddress = mod.base.add(0x17780e0);
const signature = Array.from(new Uint8Array(copyAddress.readByteArray(20)));
if (signature.slice(0, 6).join(',') !== [0x48, 0x8b, 0xc1, 0x4c, 0x8d, 0x15].join(',') ||
    signature.slice(10, 16).join(',') !== [0x49, 0x83, 0xf8, 0x0f, 0x0f, 0x87].join(',')) {
    throw new Error('Copy routine signature does not match build 148');
}
const transform = new NativeFunction(transformAddress, 'void',
    ['pointer', 'pointer', 'pointer', 'pointer'], {exceptions: 'propagate', traps: 'all'});
let started = false;
let activeThread = null;
let activeLabel = null;
let listener = null;
let copies = [];
let dropped = 0;
let counts = {};
let matches = [];
const expectedKeys = new Set(vectors.map(v=>v.expected));
function hex(buffer) {
    return Array.from(new Uint8Array(buffer), b => b.toString(16).padStart(2, '0')).join('');
}
function startBatch(label) { activeLabel = label; copies = []; dropped = 0; counts = {}; matches = []; }
function finishBatch() {
    send({type: 'copies', label: activeLabel, copies: copies, dropped: dropped, counts: counts, matches: matches});
    activeLabel = null;
}
function run(snapshot, inputBytes, initBytes, label) {
    const vm = Memory.alloc(144); vm.writeByteArray(snapshot);
    const input = Memory.alloc(16); input.writeByteArray(inputBytes);
    const output = Memory.alloc(32);
    const init = Memory.alloc(16); init.writeByteArray(initBytes);
    startBatch(label);
    transform(vm, input, output, init);
    finishBatch();
    send({type: 'raw_output', label: label, raw: hex(output.readByteArray(28))});
}
Interceptor.attach(transformAddress, {
    onEnter(args) {
        if (started) return;
        started = true;
        this.original = true;
        this.output = args[2];
        activeThread = this.threadId;
        try {
            this.snapshot = args[0].readByteArray(144);
            this.initBytes = args[3].readByteArray(16);
            this.originalInput = args[1].readByteArray(16);
            listener = Interceptor.attach(copyAddress, {
                onEnter(copyArgs) {
                    if (this.threadId !== activeThread || activeLabel === null) return;
                    const size = copyArgs[2].toUInt32();
                    counts[size] = (counts[size] || 0) + 1;
                    if (size < 8) return;
                    try {
                        const value = hex(copyArgs[1].readByteArray(16));
                        const item = {size: size, return_rva: this.returnAddress.sub(mod.base).toString(), hex: value};
                        if (expectedKeys.has(value)) matches.push(item);
                        if (copies.length < 2000) copies.push(item); else dropped++;
                    } catch (e) {
                        if (copies.length < 2000) copies.push({size: size, error: e.toString()});
                    }
                }
            });
            Interceptor.flush();
            send({type: 'captured', input: hex(this.originalInput), init: hex(this.initBytes), stack: Thread.backtrace(this.context, Backtracer.ACCURATE).map(a=>a.sub(mod.base).toString())});
            startBatch('natural');
        } catch (e) {
            send({type: 'fatal', error: e.toString()});
            activeLabel = null;
        }
    },
    onLeave(retval) {
        if (!this.original) return;
        try {
            if (activeLabel !== null) finishBatch();
            send({type: 'raw_output', label: 'natural', raw: hex(this.output.readByteArray(28))});
        } catch (e) {
            send({type: 'fatal', error: e.toString()});
        }
        const snapshot = this.snapshot;
        const initBytes = this.initBytes;
        const originalInput = this.originalInput;
        activeLabel = null;
        setImmediate(function () {
            activeThread = Process.getCurrentThreadId();
            try {
                run(snapshot, originalInput, initBytes, 'control_replay');
                for (let i = 0; i < vectors.length; i++) {
                    run(snapshot, vectors[i].obfuscated.match(/../g).map(b => parseInt(b, 16)), initBytes, 'vector_' + i);
                }
            } catch (e) {
                send({type: 'fatal', error: e.toString()});
            } finally {
                if (listener !== null) listener.detach();
                activeThread = null;
                activeLabel = null;
                send({type: 'done'});
            }
        });
    }
});
send({type: 'attached', module: mod.path, base: mod.base.toString(), transform_rva: '0x49eaa4',
      copy_rva: '0x17780e0', mode: 'search_aes_all_copy_sizes'});
