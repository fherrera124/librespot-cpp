const mod = Process.getModuleByName('Spotify.dll');

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
let current_trace = [];
let all_traces = [];

const hooks = [
    { offset: 0xd9d131, reg: 'r8' },
    { offset: 0xd9d139, reg: 'rcx' },
    { offset: 0xd9d14b, reg: 'rax' },
    { offset: 0xd9d15a, reg: 'rax' },
    { offset: 0xd9d17e, reg: 'rcx' },
    { offset: 0xd9d189, reg: 'rcx' },
    { offset: 0xd9d19b, reg: 'rax' },
    { offset: 0xd9d1aa, reg: 'rax' },
    { offset: 0xd9d1ce, reg: 'rcx' },
    { offset: 0xd9d1d9, reg: 'rcx' },
    { offset: 0xd9d1eb, reg: 'rax' },
    { offset: 0xd9d1fa, reg: 'rax' },
    { offset: 0xd9d21c, reg: 'rcx' },
    { offset: 0xd9d227, reg: 'rcx' },
    { offset: 0xd9d238, reg: 'rax' },
    { offset: 0xd9d246, reg: 'rax' }
];

let attachedHooks = [];

setImmediate(() => {
    try {
        const obfuscated = '7a154493af30b49d753cd246d6e9e83a';
        const b4_seq = '3a034bf8';
        
        const request=Memory.alloc(256),input=allocateHex(obfuscated),auxiliary=Memory.alloc(4);
        request.writeByteArray(new Uint8Array(256));
        request.add(0x70).writeU8(1); 
        auxiliary.writeByteArray(b4_seq.match(/../g).map(b=>parseInt(b,16)));
        
        let generated28 = null;
        let ours = false;
        const hook = Interceptor.attach(mod.base.add(0x49eaa4), {
            onEnter(args) { this.ours = true; this.output = args[2]; },
            onLeave() { if (this.ours) generated28 = this.output.readByteArray(28); }
        });

        pipeline(request, input, auxiliary, ptr(0));
        hook.detach();

        const context = Memory.alloc(4096);
        const wrapped = Memory.alloc(28);
        wrapped.writeByteArray(generated28);
        init(context, wrapped, auxiliary);

        activeThread = Process.getCurrentThreadId();

        for (let i = 0; i < hooks.length; i++) {
            const h = hooks[i];
            const listener = Interceptor.attach(mod.base.add(h.offset), {
                onEnter(args) {
                    if (this.threadId === activeThread) {
                        current_trace.push(this.context[h.reg].and(0xFF).toNumber());
                    }
                }
            });
            attachedHooks.push(listener);
        }

        const output = Memory.alloc(32);
        
        // Generate 256 blocks to collect traces
        for (let i = 0; i < 256; i++) {
            current_trace = [];
            stream(context, output);
            all_traces.push(current_trace);
        }

        for (const h of attachedHooks) h.detach();

        send({type: 'done', traces: all_traces});

    } catch (e) {
        send({type: 'error', stack: e.toString()});
    }
});
