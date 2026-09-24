# Saves a screenshot of Ballest's window client area to the given PNG path.
param([string]$Path)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$signature = @'
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
public class NativeShot {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
Add-Type -TypeDefinition $signature

# Without this the window rects come back in logical pixels (1536x864 at 125% scaling) while PrintWindow draws
# physical pixels, and the capture is a crop of the window's top-left corner.
[void][NativeShot]::SetProcessDPIAware()

$game = Get-Process -Name "Ballest-Win64-Shipping" | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $game) { throw "no Ballest window" }
$handle = $game.MainWindowHandle

$rect = New-Object RECT
[void][NativeShot]::GetClientRect($handle, [ref]$rect)
$origin = New-Object POINT
[void][NativeShot]::ClientToScreen($handle, [ref]$origin)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top

# PrintWindow with PW_RENDERFULLCONTENT (2) draws the window even when other windows cover it, so the game is
# never brought to the front. It renders the whole window including the title bar; the client area is cropped out.
$window = New-Object RECT
[void][NativeShot]::GetWindowRect($handle, [ref]$window)
$full = New-Object System.Drawing.Bitmap(($window.Right - $window.Left), ($window.Bottom - $window.Top))
$graphics = [System.Drawing.Graphics]::FromImage($full)
$hdc = $graphics.GetHdc()
[void][NativeShot]::PrintWindow($handle, $hdc, 2)
$graphics.ReleaseHdc($hdc)
$graphics.Dispose()
$crop = New-Object System.Drawing.Rectangle(($origin.X - $window.Left), ($origin.Y - $window.Top), $width, $height)
$bitmap = $full.Clone($crop, $full.PixelFormat)
$full.Dispose()
$bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Output "saved $Path (${width}x${height})"
