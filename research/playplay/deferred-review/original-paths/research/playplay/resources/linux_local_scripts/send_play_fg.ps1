Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, int dwExtraInfo);
}
"@

$hwnd = [Win32]::FindWindow("Chrome_WidgetWin_0", "Spotify Premium")
if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [Win32]::FindWindow("Chrome_WidgetWin_0", "Spotify Free") }
if ($hwnd -eq [IntPtr]::Zero) { $hwnd = [Win32]::FindWindow("Chrome_WidgetWin_0", $null) }

if ($hwnd -ne [IntPtr]::Zero) {
    [Win32]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 500
    [Win32]::keybd_event(0xB3, 0, 0, 0) # VK_MEDIA_PLAY_PAUSE
    [Win32]::keybd_event(0xB3, 0, 2, 0)
    Start-Sleep -Milliseconds 500
    [Win32]::keybd_event(0x20, 0, 0, 0) # Spacebar
    [Win32]::keybd_event(0x20, 0, 2, 0)
    Write-Output "Sent Play to FG"
} else {
    Write-Output "Spotify window not found"
}
