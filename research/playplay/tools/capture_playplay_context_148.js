// Observe one successful PlayPlay response, its resource id, and VM candidate.
// No HTTP headers, account credentials, or unrelated response bodies are read.
const mod=Process.getModuleByName('Spotify.dll');
function hex(buffer){return Array.from(new Uint8Array(buffer),b=>b.toString(16).padStart(2,'0')).join('');}
function rva(p){const m=Process.findModuleByAddress(p);return m ? m.name+'+'+p.sub(m.base) : p.toString();}
const entry=mod.base.add(0x4a0494);
if(hex(entry.readByteArray(8))!=='48895c2418555657')throw new Error('Response-handler signature mismatch');
let active=null;
let seen=false;
Interceptor.attach(entry,{
    onEnter(args){
        if(seen)return;
        seen=true;this.original=true;
        const req=args[1],body=args[2];
        const size=body.add(16).readU64().toNumber();
        if(size>256)throw new Error('Unexpected PlayPlay response length');
        const data=body.add(24).readU64().toNumber()>15 ? body.readPointer() : body;
        active={thread:this.threadId,resource_kind:req.add(0xa0).readU8(),
            resource_id:hex(req.add(0x8c).readByteArray(20)),
            response_protobuf:hex(data.readByteArray(size)),
            callback:rva(req.add(0x28).readPointer())};
    },
    onLeave(){
        if(!this.original)return;
        send({type:'playplay_context',...active});active=null;send({type:'done'});
    }
});
Interceptor.attach(mod.base.add(0x49eaa4),{
    onEnter(args){
        if(active===null||active.thread!==this.threadId)return;
        this.original=true;this.output=args[2];
        active.input=hex(args[1].readByteArray(16));
        active.init=hex(args[3].readByteArray(16));
    },
    onLeave(){if(this.original)active.final_output=hex(this.output.readByteArray(28));}
});
Interceptor.attach(mod.base.add(0x49f854),{
    onEnter(args){if(active!==null&&active.thread===this.threadId)active.candidate=hex(args[1].readByteArray(16));}
});
send({type:'attached',handler_rva:'0x4a0494',mode:'observe_playplay_context'});
