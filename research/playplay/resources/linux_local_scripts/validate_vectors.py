import json
import frida
import sys

with open("ground-truth-vectors.json", "r") as f:
    data = json.load(f)

js_code = """
var base = Process.findModuleByName("Spotify.dll").base;
var VmRuntimeInit = new NativeFunction(base.add(0x49CB88), 'void', ['pointer', 'pointer', 'int']);
var VmObjectTransform = new NativeFunction(base.add(0x49EAA4), 'void', ['pointer', 'pointer', 'pointer', 'pointer']);

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
        try {
            var vm_obj = Memory.alloc(144);
            var vm_rt_context = Memory.alloc(16);
            VmRuntimeInit(vm_obj, vm_rt_context, 1);
            
            var initValBuffer = Memory.alloc(16);
            initValBuffer.writeByteArray(hexToBytes("8df84f8c610a1ab4c449a214fb08305e"));
            
            var inBuffer = Memory.alloc(16);
            inBuffer.writeByteArray(hexToBytes(hexToken));
            
            var outBuffer = Memory.alloc(32);
            
            VmObjectTransform(vm_obj, inBuffer, outBuffer, initValBuffer);
            
            var aesKeyBuffer = outBuffer.readByteArray(16);
            return {success: true, aes_key: bytesToHex(aesKeyBuffer)};
        } catch(e) {
            return {error: "EXCEPTION", msg: e.toString()};
        }
    }
};
"""

def main():
    processes = [p for p in frida.get_local_device().enumerate_processes() if "spotify" in p.name.lower()]
    if not processes:
        print("Spotify process not found!")
        sys.exit(1)
        
    session = frida.attach(processes[0].pid)
    script = session.create_script(js_code)
    script.load()
    
    api = script.exports_sync
    
    passed = 0
    total = 0
    
    for item in data["vectors_token_E"]:
        expected_aes = item["aes"]
        for version, obfuscated_key in item["obfuscated"].items():
            total += 1
            res = api.deobfuscate(obfuscated_key)
            if res.get('success'):
                aes = res['aes_key']
                if aes == expected_aes:
                    print(f"PASS: {obfuscated_key} -> {aes}")
                    passed += 1
                else:
                    print(f"FAIL: {obfuscated_key} -> {aes} (Expected {expected_aes})")
            else:
                print(f"FAIL: Error on {obfuscated_key} - {res}")

    print(f"Result: {passed}/{total} passed.")

if __name__ == '__main__':
    main()
