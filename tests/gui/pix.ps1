param([string]$Hwnd, [int]$X, [int]$Y)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PX {
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
$h = [IntPtr][int64]$Hwnd
$origin = New-Object PX+POINT
[PX]::ClientToScreen($h, [ref]$origin) | Out-Null
$scr = $origin.X + $X; $scy = $origin.Y + $Y
$bmp = New-Object System.Drawing.Bitmap(1, 1)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($scr, $scy, 0, 0, (New-Object System.Drawing.Size(1, 1)))
$c = $bmp.GetPixel(0, 0)
$g.Dispose(); $bmp.Dispose()
Write-Output "pix($X,$Y)=($($c.R),$($c.G),$($c.B))"
