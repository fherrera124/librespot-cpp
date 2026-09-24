const mod = Process.getModuleByName('Spotify.dll');

function allocateHex(value){
    const p=Memory.alloc(value.length/2);
    p.writeByteArray(value.match(/../g).map(b=>parseInt(b,16)));
    return p;
}

const pipelineAddress=mod.base.add(0x4a0268);
const initAddress=mod.base.add(0xd9e2e4);
const options={exceptions:'propagate',traps:'all'};
const pipeline=new NativeFunction(pipelineAddress,'void',['pointer','pointer','pointer','pointer'],options);
const init=new NativeFunction(initAddress,'void',['pointer','pointer','pointer'],options);

setImmediate(() => {
    try {
        const obfuscated = '7a154493af30b49d753cd246d6e9e83a';
        const b4_seq = '3a034bf8';
        const targetAesHex = 'a503a84c1dc9271460cc13f142e0bae2';
        const targetPattern = targetAesHex.match(/../g).map(x => parseInt(x, 16));
        
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

        let found_stack = false;

        Stalker.follow(Process.getCurrentThreadId(), {
            events: { call: false, ret: false, exec: false, block: true, compile: false },
            transform: function (iterator) {
                let instruction = iterator.next();
                while (instruction !== null) {
                    iterator.keep();
                    if (instruction.mnemonic === 'ret') {
                        iterator.putCallout((ctx) => {
                            // Scan stack
                            const sp = ctx.rsp;
                            try {
                                const mem = sp.sub(0x1000).readByteArray(0x2000);
                                const view = new Uint8Array(mem);
                                for (let i = 0; i < view.length - 16; i++) {
                                    let match = true;
                                    for (let j = 0; j < 16; j++) {
                                        if (view[i+j] !== targetPattern[j]) {
                                            match = false; break;
                                        }
                                    }
                                    if (match) {
                                        found_stack = true;
                                    }
                                }
                            } catch(e) {}
                        });
                    }
                    instruction = iterator.next();
                }
            }
        });

        init(context, wrapped, auxiliary);
        Stalker.unfollow(Process.getCurrentThreadId());
        
        send({type: 'done', found: found_stack});

    } catch (e) {
        send({type: 'error', stack: e.toString()});
    }
});
