const pattern = "08 05 12 10"; // version 5 + token (16 bytes)
function toHex(buffer) {
    return Array.from(new Uint8Array(buffer), byte => byte.toString(16).padStart(2, '0')).join('');
}
let ranges = Process.enumerateRanges('rw-');
for (let i = 0; i < ranges.length; i++) {
    try {
        let matches = Memory.scanSync(ranges[i].base, ranges[i].size, pattern);
        for (let m of matches) {
            let dump = m.address.readByteArray(32);
            let h = toHex(dump);
            // filter out some garbage
            send({ type: 'dump', data: h, address: m.address.toString() });
        }
    } catch(e) {}
}
send({ type: 'done' });
