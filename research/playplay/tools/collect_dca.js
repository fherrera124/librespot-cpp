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
        
        let all_traces = [];

        // Intercept the stream function to log instructions
        Stalker.follow(Process.getCurrentThreadId(), {
            events: { call: false, ret: false, exec: false, block: false, compile: false },
            transform: function (iterator) {
                let instruction = iterator.next();
                while (instruction !== null) {
                    if (instruction.address.compare(streamAddress) >= 0 && instruction.address.compare(streamAddress.add(0x1000)) < 0) {
                        const mnem = instruction.mnemonic;
                        const opstr = instruction.opStr;
                        // We want to capture the T-box reads. 
                        // e.g. mov edi, dword ptr [r14 + r8*4 + 0x196acd0]
                        if (mnem === 'mov' && opstr.indexOf('r14') !== -1 && opstr.indexOf('*4') !== -1) {
                            iterator.putCallout((ctx) => {
                                // Record the offset
                                // Since we don't know which register is the index, we can just record the actual memory address accessed!
                                // We can extract it by evaluating the expression or simply reading the register values.
                                // Actually, Frida's Instruction doesn't give us the computed address directly in putCallout unless we parse the opStr.
                                // To make it easy, let's just use Frida's built-in memory access tracing if possible, 
                                // or parse the registers.
                            });
                        }
                    }
                    iterator.keep();
                    instruction = iterator.next();
                }
            }
        });
        Stalker.unfollow(Process.getCurrentThreadId());
        // Since putCallout parsing registers is annoying, let's just use Interceptor on the exact addresses of the first 16 reads!
        // We have the disassembly!
    } catch (e) {}
});
