import frida
import sys
import time
from flask import Flask, request, jsonify

app = Flask(__name__)

class State:
    ready_script = None

js_code = """
var mod = Process.findModuleByName("Spotify.dll");
if (mod === null) {
    send({type: "not_found", msg: "Spotify.dll not found in this process"});
} else {
    var base = mod.base;
    var VmObjectTransformAddress = base.add(0x49EAA4);
    var VmObjectTransform = new NativeFunction(VmObjectTransformAddress, 'void', ['pointer', 'pointer', 'pointer', 'pointer']);

    var orig_vm_obj_bytes = null;
    var orig_arg3 = null;

    Interceptor.attach(VmObjectTransformAddress, {
        onEnter: function(args) {
            if (orig_vm_obj_bytes === null) {
                try {
                    orig_vm_obj_bytes = args[0].readByteArray(144);
                    orig_arg3 = args[3];
                    send({type: "stolen", msg: "vm_obj bytes stolen successfully! Ready to serve requests."});
                } catch(e) {
                    send({type: "error", msg: "Failed to read bytes: " + e.toString()});
                }
            }
        }
    });

    function hexToBytes(hex) {
        var bytes = [];
        for (var c = 0; c < hex.length; c += 2)
            bytes.push(parseInt(hex.substr(c, 2), 16));
        return bytes;
    }

    function bytesToHex(buffer) {
        return Array.prototype.map.call(new Uint8Array(buffer), x => ('00' + x.toString(16)).slice(-2)).join('');
    }

    rpc.exports = {
        deobfuscate: function(hexToken) {
            if (orig_vm_obj_bytes === null) {
                return {error: "NOT_INITIALIZED", msg: "Please play a song in the official Spotify app to initialize the VM."};
            }
            try {
                var inBuffer = Memory.alloc(16);
                var outBuffer = Memory.alloc(32);
                inBuffer.writeByteArray(hexToBytes(hexToken));
                
                var temp_vm_obj = Memory.alloc(144);
                temp_vm_obj.writeByteArray(orig_vm_obj_bytes);
                
                VmObjectTransform(temp_vm_obj, inBuffer, outBuffer, orig_arg3);
                
                var aesKeyBuffer = outBuffer.readByteArray(16);
                return {success: true, aes_key: bytesToHex(aesKeyBuffer)};
            } catch(e) {
                return {error: "EXCEPTION", msg: e.toString(), stack: e.stack};
            }
        }
    };
}
"""


@app.route('/deob', methods=['POST'])
def deobfuscate_endpoint():
    data = request.get_json()
    if not data or 'obfuscated_key' not in data:
        return jsonify({'error': 'Missing obfuscated_key'}), 400
    
    hex_key = data['obfuscated_key']
    if State.ready_script is None:
         return jsonify({'error': 'NOT_INITIALIZED', 'msg': 'Frida script not loaded or pointer not stolen yet.'}), 500
         
    res = State.ready_script.exports_sync.deobfuscate(hex_key)
    if 'error' in res:
        return jsonify(res), 500
    return jsonify(res)

import psutil
def setup_frida():
    print("[*] Looking for the main Spotify.exe process...", flush=True)
    while True:
        target_pid = None
        for p in psutil.process_iter(['pid', 'name', 'cmdline']):
            if p.info['name'] and 'spotify.exe' in p.info['name'].lower():
                cmd = p.info.get('cmdline') or []
                cmd_str = ' '.join(cmd).lower()
                if '--type=' not in cmd_str and 'crashpad' not in cmd_str:
                    target_pid = p.info['pid']
                    break
        
        if target_pid:
            try:
                session = frida.attach(target_pid)
                script = session.create_script(js_code)
                
                def make_handler(s, session_obj, pid):
                    def handler(message, data):
                        if message['type'] == 'send':
                            payload = message['payload']
                            if payload.get('type') == 'not_found':
                                try:
                                    session_obj.detach()
                                except:
                                    pass
                            elif payload.get('type') == 'stolen':
                                State.ready_script = s
                                print(f"[Frida PID {pid}]", payload.get('msg', ''), flush=True)
                        elif message['type'] == 'error':
                            print(f"[Frida PID {pid} Error]", message.get('stack', ''), flush=True)
                    return handler
                
                script.on('message', make_handler(script, session, target_pid))
                script.load()
                print(f"[*] Successfully attached to main process (PID {target_pid}). Play a song!", flush=True)
                return
            except Exception as e:
                print(f"[*] Failed to attach: {e}", flush=True)
        
        time.sleep(2)

if __name__ == '__main__':
    setup_frida()
    print("[*] Starting Flask LAN Server on 0.0.0.0:8080", flush=True)
    app.run(host='0.0.0.0', port=8080, threaded=True)
