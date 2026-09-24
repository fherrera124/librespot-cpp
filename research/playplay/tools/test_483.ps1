$csharpCode = @"
using System;
using System.Runtime.InteropServices;

public class PlayPlayHarness2 {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadLibrary(string dllToLoad);
    
    public static string RunHarness(string dllPath) {
        IntPtr hModule = LoadLibrary(dllPath);
        int err = Marshal.GetLastWin32Error();
        return "Is64: " + Environment.Is64BitProcess + " | PtrSize: " + IntPtr.Size + " | Err: " + err + " | hMod: " + hModule;
    }
}
"@
Add-Type -TypeDefinition $csharpCode -Language CSharp
[PlayPlayHarness2]::RunHarness("C:\Users\francisco.herrera\Spotify_1.2.88.483_g8aa8628e.dll")
