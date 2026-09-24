// Observe one block during playback, then replay its private context copy.
const mod=Process.getModuleByName('Spotify.dll');
const address=mod.base.add(0xd9d0f0);
function hex(buffer){return Array.from(new Uint8Array(buffer),b=>b.toString(16).padStart(2,'0')).join('');}
if(hex(address.readByteArray(16))!=='405355565741544155415641574883ec')throw new Error('Stream signature mismatch');
const stream=new NativeFunction(address,'void',['pointer','pointer'],{exceptions:'propagate',traps:'all'});
let started=false;
const listener=Interceptor.attach(address,{
    onEnter(args){
        if(started)return;
        started=true;this.original=true;
        this.snapshot=args[0].readByteArray(0x2e4);this.output=args[1];
    },
    onLeave(){
        if(!this.original)return;
        const natural=hex(this.output.readByteArray(16)),snapshot=this.snapshot;
        listener.detach();
        setImmediate(function(){
            try{
                const context=Memory.alloc(4096),output=Memory.alloc(32);
                context.writeByteArray(new Uint8Array(4096));context.writeByteArray(snapshot);
                const blocks=[];
                for(let i=0;i<4;i++){stream(context,output);blocks.push(hex(output.readByteArray(16)));}
                send({type:'natural_stream_control',natural:natural,replay:blocks[0],
                    match:natural===blocks[0],blocks:blocks,context:hex(snapshot)});
                if(natural!==blocks[0])throw new Error('Native stream replay failed');
            }catch(e){send({type:'fatal',error:e.toString()});}
            finally{send({type:'done'});}
        });
    }
});
send({type:'attached',mode:'natural_stream_control',stream_rva:'0xd9d0f0'});
