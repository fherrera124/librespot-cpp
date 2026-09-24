// Clean capturer for Spotify 1.2.92.148
// Does NOT inject vectors, diagnosticData, or known AES keys.
// Triggers the pipeline manually with known inputs (obfuscated key, b4_seq) to avoid requiring UI interaction.

// The runner verifies disk version/hash and supplies a live-module guard.
if (typeof requireVerifiedBuild148 !== 'function') {
    throw new Error('Use run_clean_capture.py: verified build 148 required');
}
const mod = requireVerifiedBuild148();

function hex(buffer) {
    if (!buffer) return null;
    return Array.from(new Uint8Array(buffer), b => b.toString(16).padStart(2, '0')).join('');
}
function allocateHex(value){
    const p=Memory.alloc(value.length/2);
    p.writeByteArray(value.match(/../g).map(b=>parseInt(b,16)));
    return p;
}

const pipelineAddress=mod.base.add(0x4a0268);
const options={exceptions:'propagate',traps:'all'};
const pipeline=new NativeFunction(pipelineAddress,'void',['pointer','pointer','pointer','pointer'],options);

let activeThread = null;
let currentRun = null;

Interceptor.attach(mod.base.add(0x49f854), {
    onEnter(args) {
        if (this.threadId === activeThread) {
            const candidate = hex(this.context.rdx.readByteArray(16));
            send({
                type: 'clean_capture',
                event: 'candidate_received',
                run: currentRun,
                rva: '0x49f854',
                candidate: candidate
            });
        }
    }
});

Interceptor.attach(mod.base.add(0x49eaa4), {
    onEnter(args) {
        if (this.threadId === activeThread) {
            this.ours = true;
            this.input = args[1];
            this.output = args[2];
            this.arg3 = args[3];
            send({
                type: 'clean_capture',
                event: 'transform_enter',
                run: currentRun,
                rva: '0x49eaa4',
                input: hex(this.input.readByteArray(16)),
                init_arg: hex(this.arg3.readByteArray(16)),
                vm_snapshot: hex(args[0].readByteArray(144))
            });
        }
    },
    onLeave() {
        if (this.ours) {
            send({
                type: 'clean_capture',
                event: 'transform_leave',
                run: currentRun,
                rva: '0x49eaa4',
                output28: hex(this.output.readByteArray(28))
            });
        }
    }
});

Interceptor.attach(mod.base.add(0xd9e2e4), {
    onEnter(args) {
        if (this.threadId === activeThread) {
            send({
                type: 'clean_capture',
                event: 'init_enter',
                run: currentRun,
                rva: '0xd9e2e4',
                wrapped28: hex(args[1].readByteArray(28)),
                auxiliary: hex(args[2].readByteArray(4))
            });
        }
    }
});

let streamCount = 0;
Interceptor.attach(mod.base.add(0xd9d0f0), {
    onEnter(args) {
        if (this.threadId === activeThread) {
            this.ours = true;
            this.output = args[1];
        }
    },
    onLeave() {
        if (this.ours) {
            streamCount++;
            if (streamCount <= 4) { // Only capture first 4 blocks to avoid spam
                send({
                    type: 'clean_capture',
                    event: 'stream_block',
                    run: currentRun,
                    rva: '0xd9d0f0',
                    block_index: streamCount,
                    block: hex(this.output.readByteArray(16))
                });
            }
        }
    }
});

send({type: 'attached', mode: 'clean_capture', base: mod.base.toString()});

function triggerPipeline(label, obfuscated, b4_seq) {
    currentRun = label;
    streamCount = 0;
    const request=Memory.alloc(256),input=allocateHex(obfuscated),auxiliary=Memory.alloc(4);
    request.writeByteArray(new Uint8Array(256));
    request.add(0x70).writeU8(1); // Disable callback
    auxiliary.writeByteArray(b4_seq.match(/../g).map(b=>parseInt(b,16)));
    
    activeThread=Process.getCurrentThreadId();
    try {
        pipeline(request, input, auxiliary, ptr(0));
    } finally {
        activeThread=null;
    }
}

setImmediate(() => {
    try {
        triggerPipeline('test_2f43127d_b4', '7a154493af30b49d753cd246d6e9e83a', '3a034bf8');
        triggerPipeline('test_1a8e5b04_b4', '3169b37896f5f0a05685df1c09dce0e8', '4b023e13');
        send({type: 'clean_capture_done'});
    } catch(e) {
        send({type: 'error', stack: e.toString()});
    }
});
