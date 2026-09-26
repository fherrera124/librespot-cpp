$csharpCode = @"
using System;
using System.Runtime.InteropServices;

public class ApiSetTest {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadLibrary(string dllToLoad);
    
    public static string Test() {
        IntPtr hModule = LoadLibrary("api-ms-win-core-winrt-error-l1-1-1.dll");
        int err = Marshal.GetLastWin32Error();
        return "hMod: " + hModule + " | Err: " + err;
    }
}
"@
Add-Type -TypeDefinition $csharpCode -Language CSharp
[ApiSetTest]::Test()
