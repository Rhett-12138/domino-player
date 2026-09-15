param([string]$Hwnd, [int]$SX, [int]$SY, [int]$EX, [int]$EY)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class RD {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
$h = [IntPtr][int64]$Hwnd
[RD]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$origin = New-Object RD+POINT
[RD]::ClientToScreen($h, [ref]$origin) | Out-Null
[RD]::SetCursorPos($origin.X + $SX, $origin.Y + $SY) | Out-Null
Start-Sleep -Milliseconds 250
[RD]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
Start-Sleep -Milliseconds 200
$steps = 8
for ($i = 1; $i -le $steps; $i++) {
  $x = $SX + [int](($EX - $SX) * $i / $steps)
  $y = $SY + [int](($EY - $SY) * $i / $steps)
  [RD]::SetCursorPos($origin.X + $x, $origin.Y + $y) | Out-Null
  [RD]::mouse_event(0x0001, 0, 0, 0, [UIntPtr]::Zero)   # MOVE
  Start-Sleep -Milliseconds 60
}
Start-Sleep -Milliseconds 120
[RD]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
Start-Sleep -Milliseconds 250
Write-Output "dragged ($SX,$SY)->($EX,$EY)"
