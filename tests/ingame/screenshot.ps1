param([Parameter(Mandatory = $true)][string]$Path, [string]$Process = 'CivilizationV_DX11')
# Saves a picture of a process's main window as a PNG, even when other windows cover it.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
'@
[Win]::SetProcessDPIAware() | Out-Null
$window = (Get-Process -Name $Process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
if (-not $window) { throw "$Process has no window" }
$rect = New-Object Win+RECT
if (-not [Win]::GetWindowRect($window, [ref]$rect)) { throw "the window's size could not be read" }
$bitmap = New-Object System.Drawing.Bitmap ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
# 2 = PW_RENDERFULLCONTENT, which also captures DirectX content.
$printed = [Win]::PrintWindow($window, $hdc, 2)
$graphics.ReleaseHdc($hdc)
if (-not $printed) { throw "the window could not be drawn" }
$bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
