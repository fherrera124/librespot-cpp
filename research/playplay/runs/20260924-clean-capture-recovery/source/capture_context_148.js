// Clean context capturer for Spotify 1.2.92.148
// Triggers the pipeline with known obfuscated inputs, captures the 28-byte descriptor,
// initializes the audio context, and dumps the 740-byte internal state.

const mod = Process.getModuleByName('Spotify.dll');

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
const initAddress=mod.base.add(0xd9e2e4);
const streamAddress=mod.base.add(0xd9d0f0);

const options={exceptions:'propagate',traps:'all'};
const pipeline=new NativeFunction(pipelineAddress,'void',['pointer','pointer','pointer','pointer'],options);
const init=new NativeFunction(initAddress,'void',['pointer','pointer','pointer'],options);
const stream=new NativeFunction(streamAddress,'void',['pointer','pointer'],options);

let activeThread = null;

send({type: 'attached', mode: 'context_dump', base: mod.base.toString()});

function triggerPipelineAndContext(label, obfuscated, b4_seq) {
    // 1. Run Pipeline
    const request=Memory.alloc(256),input=allocateHex(obfuscated),auxiliary=Memory.alloc(4);
    request.writeByteArray(new Uint8Array(256));
    request.add(0x70).writeU8(1); // Disable callback
    auxiliary.writeByteArray(b4_seq.match(/../g).map(b=>parseInt(b,16)));
    
    let generated28 = null;
    const hook = Interceptor.attach(mod.base.add(0x49eaa4), {
        onEnter(args) {
            if (this.threadId === activeThread) {
                this.ours = true;
                this.output = args[2];
            }
        },
        onLeave() {
            if (this.ours) {
                generated28 = this.output.readByteArray(28);
            }
        }
    });

    activeThread=Process.getCurrentThreadId();
    try {
        pipeline(request, input, auxiliary, ptr(0));
    } finally {
        activeThread=null;
    }
    hook.detach();

    if (!generated28) throw new Error("Failed to capture 28-byte descriptor for " + label);

    // 2. Initialize Context
    const context = Memory.alloc(4096);
    context.writeByteArray(new Uint8Array(4096)); // clear
    const wrapped = Memory.alloc(28);
    wrapped.writeByteArray(generated28);
    
    init(context, wrapped, auxiliary);
    
    // 3. Dump context before generating blocks
    send({
        type: 'context_dump',
        run: label,
        state: 'initial',
        wrapped28: hex(generated28),
        context: hex(context.readByteArray(740))
    });

    // 4. Generate block and dump again
    const output = Memory.alloc(32);
    stream(context, output);
    
    send({
        type: 'context_dump',
        run: label,
        state: 'after_block_1',
        block: hex(output.readByteArray(16)),
        context: hex(context.readByteArray(740))
    });
}

setImmediate(() => {
    try {
        triggerPipelineAndContext('test_2f43127d_b4', '7a154493af30b49d753cd246d6e9e83a', '3a034bf8');
        triggerPipelineAndContext('test_1a8e5b04_b4', '3169b37896f5f0a05685df1c09dce0e8', '4b023e13');
        send({type: 'clean_capture_done'});
    } catch(e) {
        send({type: 'error', stack: e.toString()});
    }
});
