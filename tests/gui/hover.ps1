param([string]$Hwnd, [int]$X, [int]$Y)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HV {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
$h = [IntPtr][int64]$Hwnd
function MKLP($x, $y) { return [IntPtr]((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF)) }
$origin = New-Object HV+POINT
[HV]::ClientToScreen($h, [ref]$origin) | Out-Null
[HV]::SetCursorPos($origin.X + $X, $origin.Y + $Y) | Out-Null
Start-Sleep -Milliseconds 100
[HV]::PostMessageA($h, 0x0200, [IntPtr]0, (MKLP $X $Y)) | Out-Null
Start-Sleep -Milliseconds 300
Write-Output "hovered ($X,$Y)"
