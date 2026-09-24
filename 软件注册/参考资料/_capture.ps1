# 只截取 GUI 窗口本身（不是整屏），用于软著材料的插图。
param(
    [Parameter(Mandatory)][string]$Scene,
    [string]$Qid = 'q1',
    [Parameter(Mandatory)][string]$Out,
    [int]$WaitSec = 5,
    [switch]$Watch     # 该场景会真的跑求解，等久一点
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Win {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after,
        int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("shcore.dll")] public static extern int SetProcessDpiAwareness(int v);

    // 按**完整且独特**的标题找窗口。
    // ⚠ 不按 PID 找：`.venv\Scripts\pythonw.exe` 是 venv 的转发器，它会再起一个
    //   真正的解释器子进程 —— 窗口属于**子进程**，与我拿到的 $p.Id 不同（踩过）。
    //   tkinter 的根窗口标题是全局唯一的，用完整标题匹配即可，但 marker 必须够长，
    //   否则会撞到别处碰巧同名的窗口。
    public static IntPtr FindByTitle(string marker) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512);
            GetWindowTextW(h, sb, sb.Capacity);
            if (sb.ToString().Contains(marker)) { found = h; return false; }
            return true;              // 继续枚举
        }, IntPtr.Zero);
        return found;
    }
}
'@

try { [Win]::SetProcessDpiAwareness(2) | Out-Null } catch {}

# 先清掉上一次残留的驱动进程。不清的话 FindByTitle 会命中**旧的**窗口
# （标题一模一样），于是截到一张过时的图、或者截到挡住它的别的窗口。
Get-CimInstance Win32_Process -Filter "Name like '%python%'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match '_shot_driver' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Get-Process cpp_sim -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

$pyw = 'H:\2026数学建模\.venv\Scripts\pythonw.exe'
$drv = 'H:\2026数学建模\cpp\软著注册\_shot_driver.py'
$p = Start-Process -FilePath $pyw -ArgumentList $drv, $Scene, $Qid `
                   -WorkingDirectory 'H:\2026数学建模' -PassThru

$wait = if ($Watch) { [Math]::Max($WaitSec, 28) } else { $WaitSec }
Start-Sleep -Seconds $wait

# pythonw 是 GUI 子系统进程，MainWindowHandle 取不到 —— 按窗口标题找
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
    if ($p.HasExited) {
        throw "界面进程已退出（退出码 $($p.ExitCode)）—— 驱动脚本自身报错了"
    }
    $h = [Win]::FindByTitle('药材烘干过程数值模拟软件')
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 400
}
if ($h -eq [IntPtr]::Zero) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "没找到窗口（scene=$Scene）"
}

# ⚠ CopyFromScreen 抓的是**屏幕**，不是窗口 —— 目标窗口只要被别的窗口（比如 IDE）
#   挡住，截到的就是遮挡者（踩过：整个截图是 IDE）。所以要先把它真正顶到最前。
#
# 单靠 SetForegroundWindow 不够：Windows 的前台锁会拒绝后台进程的请求，
# 调用会静默失败。先用 SW_MINIMIZE + SW_RESTORE 走一遍，这个组合不放前台锁的
# 名下，之后 SetForegroundWindow 才生效；最后校验 GetForegroundWindow 是不是它。
[Win]::ShowWindow($h, 6) | Out-Null      # SW_MINIMIZE
Start-Sleep -Milliseconds 250
[Win]::ShowWindow($h, 9) | Out-Null      # SW_RESTORE
[Win]::BringWindowToTop($h) | Out-Null
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 1200

$fg = [Win]::GetForegroundWindow()
if ($fg -ne $h) {
    Write-Host "  ⚠ 目标窗口未取得前台（可能被其它窗口遮挡），截图可能不准" -ForegroundColor DarkYellow
}
Start-Sleep -Milliseconds 600

$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L
$ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "窗口尺寸异常: ${w}x${ht}"
}

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
# ⚠ 不用 PrintWindow：tkinter 窗口走 GDI，WM_PRINT 拿不到内容，截出来是全黑。
#   CopyFromScreen 抓的是屏幕像素，所以必须先确保目标窗口真的在最前面。
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Process cpp_sim -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Output "OK $Out  ${w}x${ht}"
