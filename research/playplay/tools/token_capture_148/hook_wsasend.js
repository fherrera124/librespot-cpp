const ws2_32 = Process.getModuleByName('ws2_32.dll');
const sendFunc = ws2_32.getExportByName('send');
const wsaSendFunc = ws2_32.getExportByName('WSASend');

function toHex(buffer) {
    return Array.from(new Uint8Array(buffer), byte => byte.toString(16).padStart(2, '0')).join('');
}

function hexDump(buffer) {
    let bytes = new Uint8Array(buffer);
    let result = '';
    for (let i = 0; i < bytes.length; i += 16) {
        let chunk = bytes.slice(i, i + 16);
        let hex = Array.from(chunk).map(b => b.toString(16).padStart(2, '0')).join(' ');
        let ascii = Array.from(chunk).map(b => (b >= 32 && b <= 126) ? String.fromCharCode(b) : '.').join('');
        result += i.toString(16).padStart(4, '0') + '  ' + hex.padEnd(48, ' ') + '  ' + ascii + '\n';
    }
    return result;
}


Interceptor.attach(sendFunc, {
    onEnter(args) {
        const num = args[2].toInt32();
        if (num > 0) {
            const data = args[1].readByteArray(num);
            send({ type: 'send', pid: Process.id, size: num, dump: hexDump(data) });
        }
    }
});

Interceptor.attach(wsaSendFunc, {
    onEnter(args) {
        const buffers = args[1];
        const count = args[2].toInt32();
        let size = 0;
        let dumpStr = "";
        for (let i = 0; i < count; i++) {
            const bufLen = buffers.add(i * Process.pointerSize * 2).readUInt();
            const bufPtr = buffers.add(i * Process.pointerSize * 2 + Process.pointerSize).readPointer();
            size += bufLen;
            if (bufLen > 0) {
                try {
                    let data = bufPtr.readByteArray(bufLen);
                    dumpStr += hexDump(data) + "\n";
                } catch(e) {}
            }
        }
        
        if (size > 0) {
            send({ type: 'wsasend', pid: Process.id, size: size, dump: dumpStr });
        }
    }
});

send({ type: 'ready', pid: Process.id, msg: "Hooked ws2_32.dll send and WSASend" });
