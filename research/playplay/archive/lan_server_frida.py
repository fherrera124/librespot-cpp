import frida
import sys
from flask import Flask, request, jsonify
import threading
import time

app = Flask(__name__)
script = None

def on_message(message, data):
    if message['type'] == 'send':
        print("[FRIDA]", message['payload'])
    elif message['type'] == 'error':
        print("[FRIDA ERROR]", message['stack'])

FRIDA_SCRIPT = """
var base = Process.findModuleByName("Spotify.dll").base;
var transformRva = 0x49EAA4;
var transformAddr = base.add(transformRva);

var VmObjectTransform = new NativeFunction(transformAddr, 'void', ['pointer', 'pointer', 'pointer', 'pointer']);
var saved_vm_obj = null;
var saved_arg3 = null;

Interceptor.attach(transformAddr, {
    onEnter: function(args) {
        if (saved_vm_obj == null) {
            saved_vm_obj = args[0];
            saved_arg3 = args[3];
            send("Stolen vm_obj pointer: " + saved_vm_obj);
            send("Stolen arg3 pointer: " + saved_arg3);
        }
    }
});

function buf2hex(buffer) {
    return Array.prototype.map.call(new Uint8Array(buffer), x => ('00' + x.toString(16)).slice(-2)).join('');
}

rpc.exports = {
    decode: function(obfKeyHex) {
        if (saved_vm_obj == null) {
            return {error: "Not initialized. Please play a track in Spotify first to capture vm_obj."};
        }

        var obfBuf = Memory.alloc(16);
        var obfBytes = [];
        for(var i=0; i<16; i++) obfBytes.push(parseInt(obfKeyHex.substr(i*2, 2), 16));
        obfBuf.writeByteArray(obfBytes);

        var derivedBuf = Memory.alloc(32); 

        // Call the magic function
        VmObjectTransform(saved_vm_obj, obfBuf, derivedBuf, saved_arg3);

        var derivedBytes = derivedBuf.readByteArray(24);
        var hexOut = buf2hex(derivedBytes);

        return {
            aes: hexOut.substring(0, 32)
        };
    }
};
send("Interceptor attached. Waiting for playback to capture vm_obj...");
"""

def setup_frida():
    global script
    print("[*] Looking for Spotify...")
    while True:
        try:
            processes = [p for p in frida.get_local_device().enumerate_processes() if "spotify" in p.name.lower()]
            if processes:
                break
        except Exception as e:
            pass
        time.sleep(2)
    
    target_pid = processes[0].pid
    print(f"[*] Attaching to Spotify (PID: {target_pid})...")
    
    try:
        session = frida.attach(target_pid)
        script = session.create_script(FRIDA_SCRIPT)
        script.on('message', on_message)
        script.load()
        print("[*] Frida script loaded and active.")
    except Exception as e:
        print(f"[!] Error attaching to Spotify: {e}")

@app.route('/decode', methods=['POST'])
def decode_endpoint():
    if not script:
        return jsonify({"error": "Frida script not loaded"}), 500
    
    data = request.json
    if not data or 'obf_key' not in data:
        return jsonify({"error": "Missing obf_key"}), 400
        
    obf_key = data['obf_key']
    if len(obf_key) != 32:
        return jsonify({"error": "obf_key must be 32 hex chars (16 bytes)"}), 400
        
    try:
        # Call the exported JS function
        res = script.exports.decode(obf_key)
        if 'error' in res:
            return jsonify(res), 400
        return jsonify({"aes_key": res['aes']})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

if __name__ == '__main__':
    t = threading.Thread(target=setup_frida, daemon=True)
    t.start()
    print("[*] Starting Flask LAN Server on 0.0.0.0:5000...")
    app.run(host='0.0.0.0', port=5000)
