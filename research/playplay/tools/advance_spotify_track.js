// One asynchronous Next Track command to a visible window owned by this PID.
// Loaded only when validate_windows_vm.py is invoked with --advance-track.
setTimeout(function () {
    try {
        const user32 = Process.getModuleByName('user32.dll');
        const native = (name, ret, args) => new NativeFunction(user32.getExportByName(name), ret, args);
        const enumWindows = native('EnumWindows', 'int', ['pointer', 'pointer']);
        const getPid = native('GetWindowThreadProcessId', 'uint32', ['pointer', 'pointer']);
        const visible = native('IsWindowVisible', 'int', ['pointer']);
        const getClass = native('GetClassNameW', 'int', ['pointer', 'pointer', 'int']);
        const post = native('PostMessageW', 'int', ['pointer', 'uint32', 'pointer', 'pointer']);
        let target = null;
        const callback = new NativeCallback(function (hwnd, unused) {
            const owner = Memory.alloc(4);
            getPid(hwnd, owner);
            if (owner.readU32() !== Process.id || visible(hwnd) === 0) return 1;
            const name = Memory.alloc(256);
            if (getClass(hwnd, name, 128) > 0 && name.readUtf16String().startsWith('Chrome_WidgetWin_')) {
                target = hwnd;
                return 0;
            }
            return 1;
        }, 'int', ['pointer', 'pointer']);
        enumWindows(callback, ptr(0));
        if (target === null) throw new Error('No visible Spotify window owned by this PID');
        const posted = post(target, 0x0319, target, ptr(11 << 16));
        send({type: 'playback_trigger', pid: Process.id, hwnd: target.toString(), posted: posted !== 0});
    } catch (e) {
        send({type: 'playback_trigger_error', error: e.toString()});
    }
}, 1000);
