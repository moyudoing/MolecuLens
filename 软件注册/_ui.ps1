# 界面驱动辅助脚本：定位/聚焦 MolecuLens 窗口，点击、拖拽、发键、截图。
# 供软著材料插图采集使用。
#
# ⚠ PowerShell 变量名不区分大小写：局部变量绝不能叫 $h/$w/$ht（会覆盖
#   参数 $H/$W），否则传进来的坐标被窗口句柄顶掉、SetWindowPos 收到垃圾尺寸。
#   本脚本统一用 $win/$rect/$shotW/$shotH。
param(
    [Parameter(Mandatory)][ValidateSet('find','focus','resize','click','dblclick',
                                       'move','keys','paste','shot','hideime',
                                       'restoreime','kill','list','drag','openfile',
                                       'shotclean')]
    [string]$Action,
    [int]$X = 0,
    [int]$Y = 0,
    [int]$W = 0,
    [int]$H = 0,
    [string]$Text = '',
    [string]$Out = '',
    [switch]$Quick     # shot：不做最小化/还原（该动作会触发输入法状态条重显示）
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ML {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();

    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;

    // 找 MolecuLens 主窗口：可见、标题**精确等于** marker、面积最大者。
    // 精确匹配是必须的：控制台窗口/日志文件窗口标题里也可能含 MolecuLens。
    public static IntPtr FindMain(string marker) {
        IntPtr best = IntPtr.Zero; int bestArea = 0;
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512); GetWindowTextW(h, sb, sb.Capacity);
            if (sb.ToString() != marker) return true;
            RECT r; GetWindowRect(h, out r);
            int a = (r.R - r.L) * (r.B - r.T);
            if (a > bestArea) { bestArea = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    public static string[] ListWindows() {
        var list = new System.Collections.Generic.List<string>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            var t = new StringBuilder(512); GetWindowTextW(h, t, t.Capacity);
            var c = new StringBuilder(256); GetClassNameW(h, c, c.Capacity);
            RECT r; GetWindowRect(h, out r);
            int w = r.R - r.L, ht = r.B - r.T;
            if (w <= 0 || ht <= 0) return true;
            int pid; GetWindowThreadProcessId(h, out pid);
            list.Add(string.Format("{0}|{1}|{2}|{3},{4},{5}x{6}|{7}",
                h, pid, c.ToString(), r.L, r.T, w, ht, t.ToString()));
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }
}
'@

try { [ML]::SetProcessDPIAware() | Out-Null } catch {}

$MARKER = 'MolecuLens'

function Get-MainWin {
    $win = [ML]::FindMain($MARKER)
    if ($win -eq [IntPtr]::Zero) {
        throw "未找到 MolecuLens 主窗口（标题必须精确为 '$MARKER'）"
    }
    return $win
}

# 确保窗口在前台。
#
# ⚠ 已经在前台时**必须直接返回**，不能走下面的最小化/还原：那个动作会销毁
#   ImGui 刚打开的下拉框弹窗，于是"点组合框 → 点条目"的第二次点击落回面板，
#   打到滑杆上（实测把雾近/雾远/键半径都点歪了，样式则始终切不过去）。
#
# 需要强制置前时（窗口在后台）：Windows 前台锁会拒绝后台进程的直接请求，
# 先用 SW_MINIMIZE+SW_RESTORE 绕过（该组合不受前台锁约束），再 SetForegroundWindow。
function Set-MainForeground($win) {
    if ([ML]::GetForegroundWindow() -eq $win) {
        return $true
    }
    [ML]::ShowWindow($win, 6) | Out-Null          # SW_MINIMIZE
    Start-Sleep -Milliseconds 220
    [ML]::ShowWindow($win, 9) | Out-Null          # SW_RESTORE
    [ML]::BringWindowToTop($win) | Out-Null
    [ML]::SetForegroundWindow($win) | Out-Null
    Start-Sleep -Milliseconds 700
    return ([ML]::GetForegroundWindow() -eq $win)
}

function Get-WinRect($win) {
    $rect = New-Object ML+RECT
    [ML]::GetWindowRect($win, [ref]$rect) | Out-Null
    return $rect
}

# 隐藏输入法悬浮条：仅 SW_HIDE 不够（随前台窗口的输入上下文反复重显示），
# 再挪到屏幕外。每次截图前都要调一次。
function Hide-Ime {
    $count = 0
    foreach ($line in [ML]::ListWindows()) {
        $parts = $line -split '\|'
        if ($parts[2] -match 'SoPY|Sogou|IME' -or $parts[7] -match '搜狗|输入法') {
            $imeWin = [IntPtr][long]$parts[0]
            [ML]::ShowWindow($imeWin, 0) | Out-Null                     # SW_HIDE
            # 0x0015 = SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE（只挪位置）
            [ML]::SetWindowPos($imeWin, [IntPtr]::Zero, -32000, -32000,
                               0, 0, 0x0015) | Out-Null
            Write-Output "ime hidden: class=$($parts[2])"
            $count++
        }
    }
    if ($count -eq 0) { Write-Output "未发现输入法悬浮条" }
}

switch ($Action) {

    'find' {
        $win = Get-MainWin
        $rect = Get-WinRect $win
        Write-Output "handle=$win rect=$($rect.L),$($rect.T) size=$($rect.R - $rect.L)x$($rect.B - $rect.T)"
    }

    'focus' {
        $win = Get-MainWin
        Write-Output "focused=$(Set-MainForeground $win) handle=$win"
    }

    'resize' {
        $win = Get-MainWin
        [ML]::SetWindowPos($win, [IntPtr]::Zero, 0, 0, $W, $H, 0x0040) | Out-Null
        Start-Sleep -Milliseconds 1800
        $rect = Get-WinRect $win
        Write-Output "resized to $($rect.R - $rect.L)x$($rect.B - $rect.T)"
    }

    'click' {
        $win = Get-MainWin
        Set-MainForeground $win | Out-Null
        $rect = Get-WinRect $win
        [ML]::SetCursorPos($rect.L + $X, $rect.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 180
        [ML]::mouse_event([ML]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [ML]::mouse_event([ML]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
        Write-Output "clicked ($X,$Y)"
    }

    'dblclick' {
        $win = Get-MainWin
        Set-MainForeground $win | Out-Null
        $rect = Get-WinRect $win
        [ML]::SetCursorPos($rect.L + $X, $rect.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 150
        1..2 | ForEach-Object {
            [ML]::mouse_event([ML]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 40
            [ML]::mouse_event([ML]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 60
        }
        Write-Output "double-clicked ($X,$Y)"
    }

    'drag' {
        # 从 (X,Y) 拖到 (W,H)（窗口相对坐标），用于拖 Dock 分隔条
        $win = Get-MainWin
        Set-MainForeground $win | Out-Null
        $rect = Get-WinRect $win
        [ML]::SetCursorPos($rect.L + $X, $rect.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 250
        [ML]::mouse_event([ML]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        # 分步移动：ImGui 靠连续位移判定进入拖拽，一步到位会被当成点击
        $steps = 14
        for ($i = 1; $i -le $steps; $i++) {
            $nx = $rect.L + [int]($X + ($W - $X) * $i / $steps)
            $ny = $rect.T + [int]($Y + ($H - $Y) * $i / $steps)
            [ML]::SetCursorPos($nx, $ny) | Out-Null
            Start-Sleep -Milliseconds 35
        }
        Start-Sleep -Milliseconds 150
        [ML]::mouse_event([ML]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 500
        Write-Output "dragged ($X,$Y) -> ($W,$H)"
    }

    'move' {
        $win = Get-MainWin
        $rect = Get-WinRect $win
        [ML]::SetCursorPos($rect.L + $X, $rect.T + $Y) | Out-Null
        Write-Output "moved to ($X,$Y)"
    }

    'keys' {
        $win = Get-MainWin
        Set-MainForeground $win | Out-Null
        [System.Windows.Forms.SendKeys]::SendWait($Text)
        Write-Output "sent keys: $Text"
    }

    'paste' {
        # 剪贴板 + Ctrl+V：中文、括号、反斜杠等任意字符都能安全输入
        $win = Get-MainWin
        Set-MainForeground $win | Out-Null
        Set-Clipboard -Value $Text
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait('^v')
        Write-Output "pasted: $Text"
    }

    'shot' {
        $win = Get-MainWin
        if ($Quick) {
            [ML]::BringWindowToTop($win) | Out-Null
            [ML]::SetForegroundWindow($win) | Out-Null
            Start-Sleep -Milliseconds 400
        } else {
            Set-MainForeground $win | Out-Null
        }
        Start-Sleep -Milliseconds 500
        $rect = Get-WinRect $win
        $shotW = $rect.R - $rect.L
        $shotH = $rect.B - $rect.T
        if ($shotW -le 0 -or $shotH -le 0) { throw "窗口尺寸异常: ${shotW}x${shotH}" }
        $bmp = New-Object System.Drawing.Bitmap $shotW, $shotH
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        # CopyFromScreen 抓屏幕合成结果：Vulkan 窗口走合成器，PrintWindow 拿不到内容
        $g.CopyFromScreen($rect.L, $rect.T, 0, 0,
                          (New-Object System.Drawing.Size $shotW, $shotH))
        New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Output "OK $Out ${shotW}x${shotH}"
    }

    'hideime' { Hide-Ime }

    'shotclean' {
        # 隐藏输入法条后**立即**截图，且全程不改变前台窗口：
        # 输入法状态条随前台窗口的输入上下文刷出，任何 SetForegroundWindow/
        # 最小化还原都会让它复位回屏幕内，所以必须"隐藏→抓屏"一气呵成。
        Hide-Ime | Out-Null
        Start-Sleep -Milliseconds 250
        $win = Get-MainWin
        $rect = Get-WinRect $win
        $shotW = $rect.R - $rect.L
        $shotH = $rect.B - $rect.T
        $bmp = New-Object System.Drawing.Bitmap $shotW, $shotH
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($rect.L, $rect.T, 0, 0,
                          (New-Object System.Drawing.Size $shotW, $shotH))
        New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Output "OK $Out ${shotW}x${shotH}"
    }

    'restoreime' {
        foreach ($line in [ML]::ListWindows()) {
            $parts = $line -split '\|'
            if ($parts[2] -match 'SoPY|Sogou|IME' -or $parts[7] -match '搜狗|输入法') {
                [ML]::ShowWindow([IntPtr][long]$parts[0], 5) | Out-Null   # SW_SHOW
                Write-Output "restored: class=$($parts[2])"
            }
        }
    }

    'openfile' {
        # 走"文件 → 打开"的原生对话框载入分子：对话框的文件名框初始即有焦点，
        # 直接粘贴全路径再回车即可（比逐字符 SendKeys 稳，中文/括号/反斜杠都不怕）。
        $dlg = [IntPtr]::Zero
        for ($i = 0; $i -lt 40 -and $dlg -eq [IntPtr]::Zero; $i++) {
            foreach ($line in [ML]::ListWindows()) {
                $parts = $line -split '\|'
                if ($parts[2] -eq '#32770') { $dlg = [IntPtr][long]$parts[0]; break }
            }
            if ($dlg -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
        }
        if ($dlg -eq [IntPtr]::Zero) { throw "未出现文件对话框" }
        [ML]::BringWindowToTop($dlg) | Out-Null
        [ML]::SetForegroundWindow($dlg) | Out-Null
        Start-Sleep -Milliseconds 700
        Set-Clipboard -Value $Text
        Start-Sleep -Milliseconds 350
        [System.Windows.Forms.SendKeys]::SendWait('^v')
        Start-Sleep -Milliseconds 600
        [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
        Start-Sleep -Milliseconds 500
        Write-Output "已提交文件路径: $Text"
    }

    'list' {
        foreach ($line in [ML]::ListWindows()) { Write-Output $line }
    }

    'kill' {
        $win = Get-MainWin
        $procId = 0
        [ML]::GetWindowThreadProcessId($win, [ref]$procId) | Out-Null
        Write-Output "handle=$win pid=$procId"
    }
}
