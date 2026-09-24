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

        let blocks = [];
        
        Stalker.follow(Process.getCurrentThreadId(), {
            events: { call: false, ret: false, exec: false, block: true, compile: false },
            onReceive: function (events) {
                const parsed = Stalker.parse(events);
                for (const e of parsed) {
                    if (e[0] === 'block') {
                        const start = e[1];
                        if (start.compare(mod.base) >= 0 && start.compare(mod.base.add(mod.size)) < 0) {
                            blocks.push(start.sub(mod.base).toString(16));
                        }
                    }
                }
            }
        });

        init(context, wrapped, auxiliary);
        Stalker.unfollow(Process.getCurrentThreadId());
        
        // Count unique blocks
        let uniqueBlocks = [...new Set(blocks)];
        send({type: 'done', unique_blocks_count: uniqueBlocks.length, unique_blocks: uniqueBlocks.slice(0, 50)});

    } catch (e) {
        send({type: 'error', stack: e.toString()});
    }
});
