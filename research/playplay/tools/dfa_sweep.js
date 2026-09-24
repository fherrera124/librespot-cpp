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

        const output = Memory.alloc(32);
        const cleanContext = Memory.alloc(740);
        Memory.copy(cleanContext, context, 740);

        stream(context, output);
        const correct_ct = hex(output.readByteArray(16));

        let faulty_cts = [];

        // Sweep from 0x10 to 0xDF
        for (let offset = 0x10; offset < 0xE0; offset++) {
            Memory.copy(context, cleanContext, 740);
            const val = context.add(offset).readU8();
            context.add(offset).writeU8(val ^ 1);
            stream(context, output);
            faulty_cts.push({
                offset: offset.toString(16),
                ct: hex(output.readByteArray(16))
            });
        }

        send({
            type: 'dfa_data',
            correct: correct_ct,
            faults: faulty_cts
        });

    } catch (e) {
        send({type: 'error', stack: e.toString()});
    }
});
