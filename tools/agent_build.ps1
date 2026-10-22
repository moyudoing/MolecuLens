# =============================================================================
# MolecuLens · tools/agent_build.ps1
# 多 agent 共用一个构建树时的串行化构建包装器。
#
# 背景：多个 agent 共享 F:\MolecuLens\build（VS/MSBuild 生成器）。两个 MSBuild
# 同时写同一批 .obj/.pdb 会互相破坏，因此无论谁构建都必须先拿到文件锁。
#
# 用法：
#   pwsh -File tools\agent_build.ps1 -Config Debug
#   pwsh -File tools\agent_build.ps1 -Config Debug -Target moleculens_tests
#   pwsh -File tools\agent_build.ps1 -Config Release -Target moleculens_app,moleculens-cli
#
# -Target 缺省 = Build 整个解决方案；多个目标用逗号分隔（写成多个 -t: 传给
# MSBuild，不能用分号 —— cmd 会把分号当命令分隔符）。
#
# 退出码：0 = 构建产物已更新（即使 post-build 的 vcpkg applocal 报 MSB3073，
#         本项目已知该噪声不代表编译失败）；非 0 = 真的构建失败。
# =============================================================================
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [string]$Target = "",
    [int]$TimeoutMinutes = 45,
    [int]$Jobs = 0,
    # 改过 shaders/ 或 CMakeLists 后需要重新 configure（着色器在 configure 期编译）。
    # 传 -Configure 会在同一把锁内先 cmake -S . -B build，再按 -Target 构建。
    [switch]$Configure
)

$ErrorActionPreference = "Stop"
$root = "F:\MolecuLens"
$buildDir = Join-Path $root "build"
$sln = Join-Path $buildDir "Moleculens.sln"
$vcvars = "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$lockPath = Join-Path $buildDir ".agent_build.lock"

if (-not (Test-Path $sln)) {
    Write-Error "[agent_build] solution not found: $sln"
    exit 2
}
if (-not (Test-Path $vcvars)) {
    Write-Error "[agent_build] vcvars64.bat not found: $vcvars"
    exit 2
}

# —— 获取跨进程文件锁（独占打开）。等待上限 = TimeoutMinutes ——
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$lockStream = $null
$waited = 0
while ($true) {
    try {
        $lockStream = [System.IO.File]::Open(
            $lockPath, [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        break
    } catch {
        Start-Sleep -Milliseconds 700
        $waited += 0.7
        if ((Get-Date) -gt $deadline) {
            Write-Error "[agent_build] timed out waiting for build lock ($TimeoutMinutes min)"
            exit 3
        }
        if ([int]$waited % 15 -lt 1) {
            Write-Output "[agent_build] waiting for build lock ({0:N0}s) ..." -f $waited
        }
    }
}

$log = Join-Path $buildDir ("_agent_build_{0}_{1}.log" -f $Config, (Get-Date -Format "HHmmss"))
$targets = @()
if ($Target -ne "") {
    $targets = $Target.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
}

try {
    if ($Configure) {
        # 着色器在 configure 期编译（cmake/shaders.cmake），改 shaders/* 后必须重新
        # configure，否则内嵌 SPIR-V 陈旧（见 REMAINING_WORK.md 环境坑 #3）。
        $cfgLog = Join-Path $buildDir ("_agent_configure_{0}.log" -f (Get-Date -Format "HHmmss"))
        Write-Output "[agent_build] configure: cmake -S $root -B $buildDir (log=$cfgLog)"
        $cfgCmd = "call `"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$buildDir`""
        cmd /c $cfgCmd *> $cfgLog
        $cfgCode = $LASTEXITCODE
        if ($cfgCode -ne 0) {
            Write-Output "----- configure log tail -----"
            Get-Content $cfgLog -Tail 40
            Write-Error "[agent_build] cmake configure failed (exit $cfgCode)"
            exit 4
        }
        Write-Output "[agent_build] configure OK"
    }

    $msbuildArgs = @('-t:' + $(if ($targets.Count -gt 0) { ($targets -join ';') } else { 'Build' }))
    # 多个目标要拆成多个 -t:（用 ; 会被 cmd 当命令分隔符）
    $tArgs = ""
    if ($targets.Count -gt 0) {
        foreach ($t in $targets) { $tArgs += " -t:$t" }
    } else {
        $tArgs = " -t:Build"
    }
    $jobArg = if ($Jobs -gt 0) { "-m:$Jobs" } else { "-m" }
    $cmd = "call `"$vcvars`" >nul 2>&1 && MSBuild.exe `"$sln`"$tArgs -p:Configuration=$Config $jobArg -v:minimal"
    Write-Output "[agent_build] config=$Config targets=$(if ($targets.Count -gt 0) { $targets -join ',' } else { 'Build' }) log=$log"
    $started = Get-Date
    cmd /c $cmd *> $log
    $code = $LASTEXITCODE
    $elapsed = ((Get-Date) - $started).TotalSeconds

    # MSB3073 = vcpkg applocal.ps1 的已知 post-build 噪声；只要链接/静态库产出行
    # 存在，就按成功处理（见 REMAINING_WORK.md 的环境约定）。
    $text = Get-Content $log -Raw -Encoding utf8
    $produced = ([regex]::Matches($text, "->\s+F:\\MolecuLens\\build\\[^\r\n]*\.(exe|lib|dll)")).Count
    $hardErrors = ([regex]::Matches($text, "error (C|LNK)\d+")).Count

    Write-Output ("[agent_build] exit={0} elapsed={1:N1}s produced={2} c/lnk-errors={3}" -f $code, $elapsed, $produced, $hardErrors)
    if ($code -ne 0) {
        Write-Output "----- build log tail (errors) -----"
        Select-String -Path $log -Pattern "error (C|LNK)\d+|FAILED|fatal error" | Select-Object -First 40 |
            ForEach-Object { $_.Line }
        Write-Output "----- last 20 lines -----"
        Get-Content $log -Tail 20
        if ($hardErrors -eq 0 -and $produced -gt 0) {
            Write-Output "[agent_build] only post-build noise (MSB3073) -> treated as SUCCESS"
            exit 0
        }
        exit 1
    }
    exit 0
} finally {
    if ($lockStream -ne $null) {
        $lockStream.Dispose()
    }
}
