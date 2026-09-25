// Loaded only through playplay_148_preflight.guarded_source().
// The host serializes calls; this script also limits its hook to its own thread.
const playplayModule = requireVerifiedBuild148();
const playplayOptions = { exceptions: 'propagate', traps: 'all' };
const playplayPipeline = new NativeFunction(playplayModule.base.add(0x4a0268),
    'void', ['pointer', 'pointer', 'pointer', 'pointer'], playplayOptions);
const playplayInit = new NativeFunction(playplayModule.base.add(0xd9e2e4),
    'void', ['pointer', 'pointer', 'pointer'], playplayOptions);
const playplayStream = new NativeFunction(playplayModule.base.add(0xd9d0f0),
    'void', ['pointer', 'pointer'], playplayOptions);

function playplayBytes(value, count) {
    if (typeof value !== 'string' || value.length !== count * 2 ||
        !/^[0-9a-fA-F]+$/.test(value)) {
        throw new Error('Invalid input length or encoding');
    }
    return value.match(/../g).map(part => parseInt(part, 16));
}

function playplayHex(buffer) {
    return Array.from(new Uint8Array(buffer), byte =>
        byte.toString(16).padStart(2, '0')).join('');
}

rpc.exports = {
    check() {
        requireVerifiedBuild148();
        return true;
    },
    deob(obfuscatedHex, b4Hex) {
        // A later request must not reuse a stale module or changed code bytes.
        requireVerifiedBuild148();
        const request = Memory.alloc(256);
        const input = Memory.alloc(16);
        const auxiliary = Memory.alloc(4);
        request.writeByteArray(new Uint8Array(256));
        request.add(0x70).writeU8(1);
        input.writeByteArray(playplayBytes(obfuscatedHex, 16));
        auxiliary.writeByteArray(playplayBytes(b4Hex, 4));

        let descriptor = null;
        const callerThread = Process.getCurrentThreadId();
        const hook = Interceptor.attach(playplayModule.base.add(0x49eaa4), {
            onEnter(args) {
                if (this.threadId === callerThread) {
                    this.output = args[2];
                }
            },
            onLeave() {
                if (this.output !== undefined) {
                    descriptor = this.output.readByteArray(28);
                }
            }
        });
        try {
            send({stage: 'pipeline'});
            playplayPipeline(request, input, auxiliary, ptr(0));
        } finally {
            hook.detach();
        }
        if (descriptor === null) {
            throw new Error('Pipeline did not produce a descriptor');
        }

        const context = Memory.alloc(4096);
        const cleanContext = Memory.alloc(740);
        const wrapped = Memory.alloc(28);
        const output = Memory.alloc(32);
        const streamAuxiliary = Memory.alloc(4);
        streamAuxiliary.writeU32(0);
        context.writeByteArray(new Uint8Array(4096));
        wrapped.writeByteArray(descriptor);
        // This is an output auxiliary of the stream initializer, not b4_seq.
        send({stage: 'initialize'});
        playplayInit(context, wrapped, streamAuxiliary);
        Memory.copy(cleanContext, context, 740);

        playplayStream(context, output);
        const correct = playplayHex(output.readByteArray(16));
        const faults = [];
        send({stage: 'faults'});
        // These 16 offsets resolved both saved 148 contexts in the offline run.
        for (let offset = 0xa0; offset < 0xb0; offset++) {
            Memory.copy(context, cleanContext, 740);
            context.add(offset).writeU8(context.add(offset).readU8() ^ 1);
            playplayStream(context, output);
            const ct = playplayHex(output.readByteArray(16));
            if (ct !== correct) faults.push({ offset: offset.toString(16), ct: ct });
        }
        send({stage: 'captured'});
        return { correct: correct, faults: faults };
    }
};
