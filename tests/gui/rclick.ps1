param([string]$Hwnd, [int]$X, [int]$Y)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MC2 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
$h = [IntPtr][int64]$Hwnd
[MC2]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$origin = New-Object MC2+POINT
[MC2]::ClientToScreen($h, [ref]$origin) | Out-Null
$sx = $origin.X + $X; $sy = $origin.Y + $Y
[MC2]::SetCursorPos($sx, $sy) | Out-Null
Start-Sleep -Milliseconds 300
[MC2]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
Start-Sleep -Milliseconds 120
[MC2]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
Start-Sleep -Milliseconds 250
Write-Output "real-clicked ($X,$Y)"
