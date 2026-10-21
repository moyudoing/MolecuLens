# 截取指定窗口客户区为 PNG（用于文档配图）
# 用法: pwsh -File tools\capture_window.ps1 -Hwnd 123456 -Out docs\figs_v2\overview.png
param(
    [Parameter(Mandatory = $true)][int64]$Hwnd,
    [Parameter(Mandatory = $true)][string]$Out
)

Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
public static class WinCap {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
}
"@

[void][WinCap]::SetProcessDPIAware()
$h = [IntPtr]$Hwnd
[void][WinCap]::ShowWindow($h, 3)      # SW_MAXIMIZE
[void][WinCap]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 1200

$rc = New-Object RECT
[void][WinCap]::GetClientRect($h, [ref]$rc)
$pt = New-Object POINT
[void][WinCap]::ClientToScreen($h, [ref]$pt)
$w = $rc.Right - $rc.Left
$ht = $rc.Bottom - $rc.Top
if ($w -le 0 -or $ht -le 0) { throw "窗口客户区尺寸无效: ${w}x${ht}" }

$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$g.Dispose()

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"captured ${w}x${ht} -> $Out"
