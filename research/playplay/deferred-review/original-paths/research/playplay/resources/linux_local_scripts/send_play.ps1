Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr hWnd, int Msg, IntPtr wParam, IntPtr lParam);
    
    [DllImport("user32.dll")]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
}
"@

$hwnd = [Win32]::FindWindow("Chrome_WidgetWin_0", "Spotify Premium")
if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [Win32]::FindWindow("Chrome_WidgetWin_0", "Spotify Free") }
if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [Win32]::FindWindow("Chrome_WidgetWin_0", $null) }

if ($hwnd -ne [IntPtr]::Zero) {
    [Win32]::SendMessageW($hwnd, 0x0319, $hwnd, [IntPtr]0xE0000)
    Write-Host "Sent Play/Pause"
} else {
    Write-Host "Spotify window not found"
}
