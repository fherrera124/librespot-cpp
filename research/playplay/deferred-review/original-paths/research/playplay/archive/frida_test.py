import frida
import sys
import time

def on_message(message, data):
    if message['type'] == 'send':
        payload = message['payload']
        if 'msg' in payload:
            print("[*]", payload['msg'])
        elif 'error' in payload:
            # print("[!]", payload['error'])
            pass
        elif 'derived' in payload:
            derived = payload['derived']
            vm_obj = payload['vm_obj']
            expected = "a503a84c1dc9271460cc13f142e0bae2"
            
            if expected in derived:
                print(f"[+] AES found in derived_buf at {derived.index(expected)//2}")
                with open("AES_FOUND.txt", "w") as f: f.write("YES")
            elif expected in vm_obj:
                print(f"[+] AES found in vm_obj at {vm_obj.index(expected)//2}")
                with open("AES_FOUND.txt", "w") as f: f.write("YES")
            else:
                print("[-] AES not found!")
                print("derived:", derived)
                print("vm_obj[:64]:", vm_obj[:64])
    else:
        print(message)

processes = [p for p in frida.get_local_device().enumerate_processes() if "spotify" in p.name.lower()]
if not processes:
    print("Spotify not running")
    sys.exit(1)

script_code = """
var base = Process.findModuleByName("Spotify.dll").base;
var transformRva = 0x49EAA4;

Interceptor.attach(base.add(transformRva), {
    onEnter: function(args) {
        send({msg: "VmObjectTransform called!"});
        try {
            this.obf = args[1].readByteArray(16);
            send({msg: "obf_buf in: " + buf2hex(this.obf)});
        } catch(e) {}
        this.derived = args[2];
        this.vm_obj = args[0];
    },
    onLeave: function(retval) {
        send({msg: "VmObjectTransform returned!"});
        try {
            var derived_content = this.derived.readByteArray(24);
            send({msg: "derived_buf out: " + buf2hex(derived_content)});
            var vm_content = this.vm_obj.readByteArray(64);
            send({msg: "vm_obj out: " + buf2hex(vm_content)});
        } catch(e) {}
    }
});

function buf2hex(buffer) {
    return Array.prototype.map.call(new Uint8Array(buffer), x => ('00' + x.toString(16)).slice(-2)).join('');
}
send({msg: "Interceptor attached, waiting for playback..."});
"""

scripts = []
for p in processes:
    try:
        session = frida.attach(p.pid)
        script = session.create_script(script_code)
        script.on('message', on_message)
        script.load()
        scripts.append((session, script))
    except Exception as e:
        pass

print("Waiting 120 seconds... PLAY A SONG IN SPOTIFY NOW!")
import time
time.sleep(120)
for session, script in scripts:
    session.detach()
