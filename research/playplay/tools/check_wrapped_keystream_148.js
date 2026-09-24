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
    stream(context,output);
    return {block:hex(output.readByteArray(16)),auxiliary:auxiliary.readU32()};
}
send({type:'attached',mode:'native_keystream_control',init_rva:'0xd9e2e4',stream_rva:'0xd9d0f0'});
setImmediate(function(){
    try{
        for(const item of diagnosticData){
            if(item.wrapped.length!==56)throw new Error('Wrapped key must have 28 bytes');
            const first=run(item),second=run(item);
            send({type:'keystream_control',file_id:item.file_id,candidate:item.candidate,
                expected:item.expected_stream,actual:first.block,repeat:second.block,
                auxiliary:first.auxiliary,match:first.block===item.expected_stream,
                deterministic:first.block===second.block});
        }
    }catch(e){send({type:'fatal',error:e.toString()});}
    finally{send({type:'done'});}
});
