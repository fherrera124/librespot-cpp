// Candidate plaintext capture before the final randomized output wrapper.
const mod = Process.getModuleByName('Spotify.dll');
const transformAddress = mod.base.add(0x49eaa4);
const wrapAddress = mod.base.add(0x49f854);
const signature = Array.from(new Uint8Array(wrapAddress.readByteArray(13)));
if (signature.join(',') !== [0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57].join(',')) {
    throw new Error('Build-148 wrapper signature mismatch');
}
const transform = new NativeFunction(transformAddress, 'void',
    ['pointer','pointer','pointer','pointer'], {exceptions:'propagate',traps:'all'});
let started = false;
let activeThread = null;
let activeLabel = null;
let captured = [];
let naturalKey = null;
function hex(buffer) {
    return Array.from(new Uint8Array(buffer), b=>b.toString(16).padStart(2,'0')).join('');
}
const keyListener = Interceptor.attach(wrapAddress, {
    onEnter(args) {
        if (this.threadId !== activeThread || activeLabel === null) return;
        captured.push(hex(args[1].readByteArray(16)));
    }
});
function oneKey() {
    if (captured.length !== 1) throw new Error('Expected one key capture, got '+captured.length);
    return captured[0];
}
function run(snapshot, inputBytes, initBytes, label) {
    const vm=Memory.alloc(144);vm.writeByteArray(snapshot);
    const input=Memory.alloc(16);input.writeByteArray(inputBytes);
    const init=Memory.alloc(16);init.writeByteArray(initBytes);
    const output=Memory.alloc(32);
    activeLabel=label;captured=[];
    transform(vm,input,output,init);
    const actual=oneKey();
    const raw=hex(output.readByteArray(28));
    activeLabel=null;
    return {actual:actual,raw:raw,format:'prewrap16'};
}
Interceptor.attach(transformAddress, {
    onEnter(args) {
        if (started) return;
        started=true;
        this.original=true;
        this.snapshot=args[0].readByteArray(144);
        this.input=args[1].readByteArray(16);
        this.init=args[3].readByteArray(16);
        this.output=args[2];
        activeThread=this.threadId;activeLabel='natural';captured=[];
        send({type:'captured',input:hex(this.input),init:hex(this.init)});
    },
    onLeave(retval) {
        if (!this.original) return;
        try {
            naturalKey=oneKey();
            send({type:'natural_output',actual:naturalKey,raw:hex(this.output.readByteArray(28)),format:'prewrap16'});
        } catch(e) { send({type:'fatal',error:e.toString()}); }
        activeLabel=null;
        const snapshot=this.snapshot,input=this.input,init=this.init;
        setImmediate(function(){
            activeThread=Process.getCurrentThreadId();
            try {
                const control=run(snapshot,input,init,'control');
                send({type:'control_replay',...control});
                if (control.actual!==naturalKey) throw new Error('Natural key control mismatch');
                for (let i=0;i<vectors.length;i++) {
                    const bytes=vectors[i].obfuscated.match(/../g).map(b=>parseInt(b,16));
                    const first=run(snapshot,bytes,init,'vector_'+i);
                    const second=run(snapshot,bytes,init,'repeat_'+i);
                    if (first.actual!==second.actual) throw new Error('Non-deterministic candidate for vector '+i);
                    send({type:'result',index:i,...first,repeat_actual:second.actual,repeat_raw:second.raw});
                }
            } catch(e) {send({type:'fatal',error:e.toString()});}
            finally {activeLabel=null;keyListener.detach();send({type:'done'});}
        });
    }
});
send({type:'attached',module:mod.path,transform_rva:'0x49eaa4',capture_function_rva:'0x49f854',capture_argument:1,mode:'prewrap16'});
