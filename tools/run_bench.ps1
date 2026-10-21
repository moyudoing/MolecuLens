<#
.SYNOPSIS
  MolecuLens 渲染性能基准 —— 一键复现脚本

.DESCRIPTION
  跑三组测量并汇总为一份 markdown 报告：
    ① 引擎级基准（moleculens_bench，headless GPU）：加载分段耗时、首帧延迟、
       静止/交互/轨迹帧切三档帧时间分布、RSS、显存、CPU 占比；
    ② 结构解析规模扫掠（--parse-only）：原子数/实例数 → 解析耗时；
    ③ 应用级端到端（moleculens_app + 帧时 telemetry）：含 ImGui/Dock 的整帧
       分布，从日志的 [perf] 行解析。

  测量口径与限制见 tests/bench/bench_main.cpp 头部注释（务必先读）。

.PARAMETER Config
  构建配置，默认 Release（Debug 数字约慢 2-3 倍，仅作对照）。

.PARAMETER SkipBuild
  跳过构建（二进制已是最新时用）。

.EXAMPLE
  pwsh -File tools/run_bench.ps1
  pwsh -File tools/run_bench.ps1 -Config Debug -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Config = "Release",
    [string]$Gro = "F:/MolecuLens/data/md.gro",
    [string]$Xtc = "F:/MolecuLens/data/md.xtc",
    [string]$OutDir = "",
    [int]$AppSeconds = 20,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo "other/bench_out" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$vswhere = "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$env:PYTHONIOENCODING = "utf-8"

$benchExe = Join-Path $repo "build/tests/$Config/moleculens_bench.exe"
$appExe = Join-Path $repo "build/src/app/$Config/moleculens_app.exe"

function Write-Head($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# 0) 构建 + 运行期依赖
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Head "构建 ($Config)"
    if (-not (Test-Path $vswhere)) { throw "找不到 vcvars64.bat: $vswhere（按本机 VS 安装路径改脚本）" }
    cmd /c "call `"$vswhere`" >nul && msbuild `"$repo\build\MolecuLens.sln`" -t:moleculens_bench -t:moleculens_app -t:moleculens_tests -p:Configuration=$Config -m -v:minimal" |
        Select-String -Pattern "error C|error LNK|\.vcxproj ->" | Select-Object -First 15
}

# vcpkg 的 applocal 后处理在本机失败（见 README/DEPENDENCIES），运行期 DLL 需手动就位。
# 不做这一步会得到 0xC0000135（STATUS_DLL_NOT_FOUND），且错误信息毫无指向性。
$vcpkgBin = Join-Path $repo "build/vcpkg_installed/x64-windows/bin"
$dbgBin = Join-Path $repo "build/vcpkg_installed/x64-windows/debug/bin"
$srcBin = if ($Config -eq "Release") { $vcpkgBin } else { $dbgBin }
foreach ($d in @("build/tests/$Config", "build/src/app/$Config")) {
    Get-ChildItem $srcBin -Filter *.dll -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(fmt|spdlog|glfw3)' } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $repo $d) -Force }
}

if (-not (Test-Path $benchExe)) { throw "基准程序不存在: $benchExe" }

# ---------------------------------------------------------------------------
# 1) 引擎级基准：表示 × 分辨率扫掠
# ---------------------------------------------------------------------------
Write-Head "① 引擎级基准"
$rows = @()
# 对 md.gro（无蛋白质骨架）有意义的表示：球/棍/甘草棍/空间填充；
# cartoon/tube/ribbon/newcartoon 需要 CA 骨架，该体系下产出 0 几何 —— 一并跑，
# 数字本身就说明了"骨架类样式不适用于小分子体系"。
$reps = @("ballstick", "stick", "licorice", "vdw", "spacefill", "cartoon")
foreach ($rep in $reps) {
    $json = (& $benchExe --gro $Gro --xtc $Xtc --rep $rep --frames 60 --steady 60 --rebuild-every 10 2>&1 |
             Where-Object { $_ -like 'BENCH_JSON*' }) -join ''
    if ($json -notmatch '^BENCH_JSON (.*)$') { Write-Warning "rep=$rep 无结果"; continue }
    $o = $Matches[1] | ConvertFrom-Json
    $rows += [pscustomobject]@{
        表示        = $rep
        球          = $o.spheres
        柱          = $o.cylinders
        命令流KB    = [math]::Round($o.cmdPoolBytes / 1024, 1)
        构建ms      = [math]::Round($o.buildCmdMs, 2)
        静止ms      = [math]::Round($o.staticAvgMs, 2)
        静止FPS     = [math]::Round(1000 / $o.staticAvgMs, 0)
        交互ms      = [math]::Round($o.interactAvgMs, 2)
        交互FPS     = [math]::Round(1000 / $o.interactAvgMs, 0)
        交互p95ms   = [math]::Round($o.interactP95Ms, 2)
        帧切ms      = [math]::Round($o.trajAvgMs, 2)
        帧切FPS     = [math]::Round(1000 / $o.trajAvgMs, 0)
        写坐标ms    = [math]::Round($o.trajApplyMs, 2)
        重放ms      = [math]::Round($o.trajPrepareMs, 2)
        GPUms       = [math]::Round($o.trajExecMs, 2)
    }
}
$rows | Format-Table -AutoSize

# 分辨率扫掠（固定 ballstick）：验证 GPU 侧是否随像素数饱和
Write-Head "① b 分辨率扫掠（ballstick）"
$resRows = @()
foreach ($wh in @(@(1280, 720), @(1600, 900), @(1920, 1080), @(2560, 1440), @(3840, 2160))) {
    $json = (& $benchExe --gro $Gro --xtc $Xtc --rep ballstick --frames 30 --steady 30 `
                        --w $wh[0] --h $wh[1] 2>&1 |
             Where-Object { $_ -like 'BENCH_JSON*' }) -join ''
    if ($json -notmatch '^BENCH_JSON (.*)$') { continue }
    $o = $Matches[1] | ConvertFrom-Json
    $resRows += [pscustomobject]@{
        分辨率    = "$($wh[0])x$($wh[1])"
        静止ms    = [math]::Round($o.staticAvgMs, 2)
        静止FPS   = [math]::Round(1000 / $o.staticAvgMs, 0)
        帧切ms    = [math]::Round($o.trajAvgMs, 2)
        GPUms     = [math]::Round($o.trajExecMs, 2)
        显存MiB   = [math]::Round($o.vramUsedMiB, 1)
        RSS峰值MiB = [math]::Round($o.rssPeakMiB, 1)
    }
}
$resRows | Format-Table -AutoSize

# ---------------------------------------------------------------------------
# 2) 结构解析规模扫掠
# ---------------------------------------------------------------------------
Write-Head "② 结构解析规模扫掠"
$scaleDir = Join-Path $OutDir "gro_scale"
New-Item -ItemType Directory -Force $scaleDir | Out-Null
$srcLines = Get-Content $Gro
$scaleRows = @()
foreach ($n in @(500, 1000, 2000, 4000, 6452)) {
    if ($n -gt ($srcLines.Count - 3)) { continue }
    $p = Join-Path $scaleDir "md_$n.gro"
    @($srcLines[0], "  $n", $srcLines[2..(1 + $n)], $srcLines[-1]) |
        Set-Content -Path $p -Encoding ascii
    $json = (& $benchExe --gro $p --parse-only 2>&1 | Where-Object { $_ -like 'BENCH_JSON*' }) -join ''
    if ($json -notmatch '^BENCH_JSON (.*)$') { continue }
    $o = $Matches[1] | ConvertFrom-Json
    $scaleRows += [pscustomobject]@{
        原子   = $o.atoms
        实例   = $o.instances
        解析ms = [math]::Round($o.parseGroMs, 1)
        us每原子 = [math]::Round($o.parseGroMs * 1000 / $o.atoms, 1)
    }
}
$scaleRows | Format-Table -AutoSize

# ---------------------------------------------------------------------------
# 3) 应用级端到端（含 ImGui / Dock）
# ---------------------------------------------------------------------------
Write-Head "③ 应用级端到端（$AppSeconds s）"
$appRows = @()
if (Test-Path $appExe) {
    $logPath = Join-Path $repo "logs/moleculens.log"
    $logLenBefore = if (Test-Path $logPath) { (Get-Item $logPath).Length } else { 0 }
    $proc = Start-Process -FilePath $appExe -ArgumentList $Gro, $Xtc `
                          -WorkingDirectory $repo -PassThru
    Start-Sleep -Seconds $AppSeconds
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Start-Sleep -Milliseconds 500
    # 只取本次运行追加的那段日志（脚本可重复执行）
    $newLog = @()
    if (Test-Path $logPath) {
        $fs = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
        $fs.Seek($logLenBefore, 'Begin') | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        $newLog = $sr.ReadToEnd() -split "`r?`n"
        $sr.Close(); $fs.Close()
    }
    foreach ($line in ($newLog | Where-Object { $_ -match '\[perf\] 整帧' })) {
        if ($line -match 'samples: avg ([\d.]+) ms \(([\d.]+) FPS\) \| p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms \(最低 ([\d.]+) FPS') {
            $appRows += [pscustomobject]@{
                avg_ms = [double]$Matches[1]; avg_FPS = [double]$Matches[2]
                p50_ms = [double]$Matches[3]; p95_ms = [double]$Matches[4]
                p99_ms = [double]$Matches[5]; max_ms = [double]$Matches[6]
                最低FPS = [double]$Matches[7]
            }
        }
    }
    # 视口确实画出了几何（0 = 空白视口，见 serial 单调性那条回归）
    $exeLog = $newLog | Where-Object { $_ -match 'offscreen execute: (\d+) spheres' } | Select-Object -Last 1
    if ($exeLog -match 'offscreen execute: (\d+) spheres, (\d+) cylinders') {
        Write-Host "视口几何: $($Matches[1]) 球 / $($Matches[2]) 柱"
    } else {
        Write-Host "视口几何: 未记录（可能未加载文件）" -ForegroundColor Yellow
    }
    if ($appRows.Count -gt 1) {
        # 首窗含导入造成的尖峰，取后段稳态
        $steady = $appRows | Select-Object -Skip 1
        $appRows | Format-Table -AutoSize
        Write-Host ("稳态汇总: avg {0:N2} ms ({1:N0} FPS), p99 {2:N2} ms, 最低 {3:N0} FPS" -f `
            (($steady | Measure-Object avg_ms -Average).Average),
            (($steady | Measure-Object avg_FPS -Average).Average),
            (($steady | Measure-Object p99_ms -Maximum).Maximum),
            (($steady | Measure-Object 最低FPS -Minimum).Minimum))
    }
}

# ---------------------------------------------------------------------------
# 4) 报告
# ---------------------------------------------------------------------------
$stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
$rep = Join-Path $OutDir "bench_report.md"
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# MolecuLens 渲染性能实测（$Config）")
[void]$sb.AppendLine()
[void]$sb.AppendLine("- 时间：$stamp")
[void]$sb.AppendLine("- 结构：``$Gro``　轨迹：``$Xtc``")
[void]$sb.AppendLine("- 基准程序：``tests/bench/bench_main.cpp``（headless GPU，口径见其头部注释）")
[void]$sb.AppendLine("- 复现：``pwsh -File tools/run_bench.ps1 -Config $Config``")
[void]$sb.AppendLine()
[void]$sb.AppendLine("## ① 表示扫掠")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| " + (($rows[0].PSObject.Properties.Name) -join " | ") + " |")
[void]$sb.AppendLine("|" + (($rows[0].PSObject.Properties.Name | ForEach-Object { "---" }) -join "|") + "|")
foreach ($r in $rows) { [void]$sb.AppendLine("| " + (($r.PSObject.Properties.Value) -join " | ") + " |") }
[void]$sb.AppendLine()
[void]$sb.AppendLine("## ① b 分辨率扫掠（ballstick）")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| " + (($resRows[0].PSObject.Properties.Name) -join " | ") + " |")
[void]$sb.AppendLine("|" + (($resRows[0].PSObject.Properties.Name | ForEach-Object { "---" }) -join "|") + "|")
foreach ($r in $resRows) { [void]$sb.AppendLine("| " + (($r.PSObject.Properties.Value) -join " | ") + " |") }
[void]$sb.AppendLine()
[void]$sb.AppendLine("## ② 结构解析规模扫掠")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| " + (($scaleRows[0].PSObject.Properties.Name) -join " | ") + " |")
[void]$sb.AppendLine("|" + (($scaleRows[0].PSObject.Properties.Name | ForEach-Object { "---" }) -join "|") + "|")
foreach ($r in $scaleRows) { [void]$sb.AppendLine("| " + (($r.PSObject.Properties.Value) -join " | ") + " |") }
[void]$sb.AppendLine()
[void]$sb.AppendLine("## ③ 应用级端到端（含 ImGui/Dock）")
[void]$sb.AppendLine()
if ($appRows.Count -gt 0) {
    [void]$sb.AppendLine("| avg ms | avg FPS | p50 ms | p95 ms | p99 ms | max ms | 最低 FPS |")
    [void]$sb.AppendLine("|---|---|---|---|---|---|---|")
    foreach ($r in $appRows) {
        [void]$sb.AppendLine("| $($r.avg_ms) | $($r.avg_FPS) | $($r.p50_ms) | $($r.p95_ms) | $($r.p99_ms) | $($r.max_ms) | $($r.最低FPS) |")
    }
} else {
    [void]$sb.AppendLine("（未采集到 [perf] 行）")
}
Set-Content -Path $rep -Value $sb.ToString() -Encoding utf8
Write-Head "报告已写出"
Write-Host $rep
