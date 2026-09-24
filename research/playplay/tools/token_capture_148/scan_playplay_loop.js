const pattern = "70 6c 61 79 70 6c 61 79 2f 76 31 2f 6b 65 79"; // "playplay/v1/key"
let foundAddresses = new Set();
let scanning = true;

function toHex(buffer) {
    return Array.from(new Uint8Array(buffer), byte => byte.toString(16).padStart(2, '0')).join('');
}

function doScan() {
    if (!scanning) return;
    
    const ranges = Process.enumerateRanges('rw-');
    for (let i = 0; i < ranges.length; i++) {
        let range = ranges[i];
        try {
            let matches = Memory.scanSync(range.base, range.size, pattern);
            for (let m of matches) {
                let addrStr = m.address.toString();
                if (!foundAddresses.has(addrStr)) {
                    foundAddresses.add(addrStr);
                    let dump = m.address.readByteArray(256);
                    send({ type: 'dump', pid: Process.id, address: addrStr, data: toHex(dump) });
                }
            }
        } catch(e) {}
    }
    
    setTimeout(doScan, 1000);
}

doScan();
send({ type: 'ready', pid: Process.id, msg: "Scanning memory continuously..." });
