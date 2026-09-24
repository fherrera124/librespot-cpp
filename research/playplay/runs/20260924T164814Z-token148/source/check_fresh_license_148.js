// Fresh-license diagnostic derived from check_key_pipeline_148.js.
// No account credentials; pass the captured response b4_seq explicitly.
// Compare the native initial stream block with AES(candidate, standard audio IV).
// Only freshly allocated context/output buffers and captured 28-byte blobs are used.
const mod=Process.getModuleByName('Spotify.dll');
function hex(buffer){return Array.from(new Uint8Array(buffer),b=>b.toString(16).padStart(2,'0')).join('');}
function allocateHex(value){const p=Memory.alloc(value.length/2);p.writeByteArray(value.match(/../g).map(b=>parseInt(b,16)));return p;}
const initAddress=mod.base.add(0xd9e2e4),streamAddress=mod.base.add(0xd9d0f0);
if(hex(initAddress.readByteArray(16))!=='48895c240848896c2410488974241857' ||
   hex(streamAddress.readByteArray(16))!=='405355565741544155415641574883ec')throw new Error('Stream function signature mismatch');
const options={exceptions:'propagate',traps:'all'};
const init=new NativeFunction(initAddress,'void',['pointer','pointer','pointer'],options);
const stream=new NativeFunction(streamAddress,'void',['pointer','pointer'],options);
function run(item){
    const context=Memory.alloc(4096),wrapped=allocateHex(item.wrapped);
    const auxiliary=Memory.alloc(4),output=Memory.alloc(32);
    context.writeByteArray(new Uint8Array(4096));auxiliary.writeU32(0);
    init(context,wrapped,auxiliary);
    const count=item.block_count===undefined?4:item.block_count;
    if(!Number.isInteger(count)||count<1||count>256)throw new Error('Invalid block count');
    const blocks=[];
    for(let i=0;i<count;i++){stream(context,output);blocks.push(hex(output.readByteArray(16)));}
    return {block:blocks[0],blocks:blocks,auxiliary:auxiliary.readU32()};
}

const pipelineAddress=mod.base.add(0x4a0268);
if(hex(pipelineAddress.readByteArray(16))!=='4055535657415441564157488dac24d0')throw new Error('Pipeline signature mismatch');
const pipeline=new NativeFunction(pipelineAddress,'void',['pointer','pointer','pointer','pointer'],options);
let thread=null,generated=null,candidate=null;
Interceptor.attach(mod.base.add(0x49f854),{onEnter(args){
    if(this.threadId===thread)candidate=hex(args[1].readByteArray(16));
}});
Interceptor.attach(mod.base.add(0x49eaa4),{
    onEnter(args){if(this.threadId===thread){this.ours=true;this.output=args[2];}},
    onLeave(){if(this.ours)generated=hex(this.output.readByteArray(28));}
});
function generate(item){
    const request=Memory.alloc(256),input=allocateHex(item.obfuscated),auxiliary=Memory.alloc(4);
    request.writeByteArray(new Uint8Array(256));
    // The caller checks [request+0x70] before dereferencing its callback.
    // Mark that callback disabled; no client request object or queue is used.
    request.add(0x70).writeU8(1);
    if(!/^[0-9a-f]{8}$/.test(item.b4_seq))throw new Error('Expected four-byte b4_seq');
    auxiliary.writeByteArray(item.b4_seq.match(/../g).map(b=>parseInt(b,16)));
    generated=null;candidate=null;thread=Process.getCurrentThreadId();
    try{pipeline(request,input,auxiliary,ptr(0));}finally{thread=null;}
    if(generated===null || candidate===null)throw new Error('Missing pipeline output');
    if(item.expected_candidate!==undefined && candidate!==item.expected_candidate)throw new Error('Fresh runtime differs from natural snapshot for '+item.label);
    return {wrapped:generated,candidate:candidate};
}
send({type:'attached',mode:'fresh_license_pipeline_control',pipeline_rva:'0x4a0268'});
setImmediate(function(){
    try{
        for(const item of diagnosticData){
            const firstGenerated=generate(item);
            const wrapped=firstGenerated.wrapped;
            const first=run({wrapped:wrapped,block_count:item.block_count});
            const secondGenerated=generate(item);
            const wrappedRepeat=secondGenerated.wrapped;
            const second=run({wrapped:wrappedRepeat,block_count:item.block_count});
            send({type:'pipeline_control',label:item.label,file_id:item.file_id,
                candidate:firstGenerated.candidate,repeat_candidate:secondGenerated.candidate,
                candidate_match:item.aes_known?firstGenerated.candidate===item.aes:null,
                b4_seq:item.b4_seq,wrapped:wrapped,wrapped_repeat:wrappedRepeat,
                expected:item.expected_stream,actual:first.block,repeat:second.block,
                blocks:first.blocks,repeat_blocks:second.blocks,
                auxiliary:first.auxiliary,repeat_auxiliary:second.auxiliary,
                aes_known:item.aes_known,match:first.block===item.expected_stream,
                deterministic:JSON.stringify(first.blocks)===JSON.stringify(second.blocks) && firstGenerated.candidate===secondGenerated.candidate});
        }
    }catch(e){send({type:'fatal',error:e.toString()});}
    finally{send({type:'done'});}
});
