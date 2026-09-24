// Read-only observation of one natural transform and its caller chain.
const mod = Process.getModuleByName('Spotify.dll');
function hex(buffer) {
    return Array.from(new Uint8Array(buffer), b=>b.toString(16).padStart(2,'0')).join('');
}
function location(address) {
    const owner=Process.findModuleByAddress(address);
    return owner ? owner.name+'+'+address.sub(owner.base) : address.toString();
}
let thread=null;
let seen=false;
Interceptor.attach(mod.base.add(0x49f854), {
    onEnter(args) {
        if(this.threadId!==thread) return;
        send({type:'candidate',actual:hex(args[1].readByteArray(16)),caller:location(this.returnAddress)});
    }
});
Interceptor.attach(mod.base.add(0x49eaa4), {
    onEnter(args) {
        if(seen) return;
        seen=true;this.original=true;thread=this.threadId;
        send({type:'natural_call',input:hex(args[1].readByteArray(16)),
            caller:location(this.returnAddress),
            stack:Thread.backtrace(this.context,Backtracer.ACCURATE).map(location)});
    },
    onLeave() {
        if(!this.original)return;
        thread=null;
        send({type:'done'});
    }
});
const crypto=[];
for(const m of Process.enumerateModules()) {
    const names=m.enumerateExports().filter(e=>/^(AES_|aesni_|BCrypt(Decrypt|GenerateSymmetricKey|ImportKey))/.test(e.name)).map(e=>e.name);
    if(names.length)crypto.push({module:m.name,exports:names});
}
send({type:'attached',crypto_exports:crypto});
