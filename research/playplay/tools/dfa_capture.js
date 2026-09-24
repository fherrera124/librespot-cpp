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
        
        // Save the clean context
        const cleanContext = Memory.alloc(740);
        Memory.copy(cleanContext, context, 740);

        // 1. Generate correct ciphertext
        stream(context, output);
        const correct_ct = hex(output.readByteArray(16));

        // 2. Setup fault injection hook at the start of Round 9.
        // Looking at the disassembly:
        // Round 8 ends around d9d52f. Let's hook d9d52f: mov edi, dword ptr [r14 + rcx*4 + 0x19634d0]
        // Actually, we can hook BEFORE Round 9 starts.
        // d9d51d: mov eax, dword ptr [r12 + 0x40] (Round 8 key xor for last word)
        // Let's hook d9d52f, which is the FIRST instruction of Round 9!
        // At this point, the state from Round 8 is in eax, edx, r8d, r9d ? 
        // We can just corrupt one of them!
        // Let's corrupt 'rcx' which is used as an index right here: mov edi, dword ptr [r14 + rcx*4 + 0x19634d0]
        let inject_fault = false;
        let fault_val = 0;
        const faultHook = Interceptor.attach(mod.base.add(0xd9d52f), {
            onEnter(args) {
                if (inject_fault) {
                    this.context.rcx = this.context.rcx.xor(fault_val);
                    inject_fault = false; // only inject once
                }
            }
        });

        let faulty_cts = [];

        // 3. Generate faulty ciphertexts
        for (let i = 1; i <= 4; i++) {
            // Restore clean context
            Memory.copy(context, cleanContext, 740);
            
            inject_fault = true;
            fault_val = i; // XOR with 1, 2, 3, 4
            
            stream(context, output);
            faulty_cts.push(hex(output.readByteArray(16)));
        }

        faultHook.detach();

        send({
            type: 'dfa_data',
            correct: correct_ct,
            faulty: faulty_cts
        });

    } catch (e) {
        send({type: 'error', stack: e.toString()});
    }
});
