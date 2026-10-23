# =============================================================================
# MolecuLens · tools/package_release.ps1 —— 一键构建 Windows 发布安装包
# -----------------------------------------------------------------------------
# 流程：配置（Ninja/Release）→ 编译 GUI + CLI → cmake --install 到 staging
#       → 生成安装器品牌图 → NSIS 编译安装程序 → cpack 打免安装 ZIP → SHA256
#
# 用法（任意 PowerShell 窗口，无需预先进入 VS 开发者环境）：
#   powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
#   powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version 2.20.0 -SkipBuild
#
# 输出：dist\MolecuLens-<版本>-win64-Setup.exe（安装程序）
#       dist\MolecuLens-<版本>-win64-portable.zip（绿色版）
#       dist\SHA256SUMS.txt
# =============================================================================
[CmdletBinding()]
param(
    [string]$Version   = '2.19.0',
    [string]$BuildDir  = 'build_pkg',
    [string]$OutDir    = 'dist',
    [string]$Config    = 'Release',
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [switch]$SkipZip
)

$ErrorActionPreference = 'Continue'   # 原生命令的 stderr 不算失败；退出码逐条显式检查
$script:Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $script:Root

function Write-Step([string]$msg) {
    Write-Host ''
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

function Fail([string]$msg) {
    Write-Host "错误: $msg" -ForegroundColor Red
    exit 1
}

# ————————————————————————————————————————————————————————————————
# 0. 版本号校验
# ————————————————————————————————————————————————————————————————
if ($Version -notmatch '^\d+\.\d+\.\d+$') { Fail "版本号必须是 MAJOR.MINOR.PATCH，当前 '$Version'" }

# ————————————————————————————————————————————————————————————————
# 1. 定位工具链
# ————————————————————————————————————————————————————————————————
Write-Step '定位工具链'

function Find-VcVars {
    if ($env:VCVARS64 -and (Test-Path $env:VCVARS64)) { return $env:VCVARS64 }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                           -property installationPath 2>$null
        if ($path) {
            $cand = Join-Path $path.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $cand) { return $cand }
        }
    }
    foreach ($p in @('D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
                     'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
                     'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat')) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Find-Tool([string[]]$candidates, [string]$fallbackName) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    $cmd = Get-Command $fallbackName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$vcvars = Find-VcVars
if (-not $vcvars) { Fail '未找到 vcvars64.bat，请安装 Visual Studio 2022 的 C++ 桌面开发工作负载' }

$cmake = Find-Tool @('E:\CMake\bin\cmake.exe', 'C:\Program Files\CMake\bin\cmake.exe') 'cmake'
$cpack = Find-Tool @('E:\CMake\bin\cpack.exe', 'C:\Program Files\CMake\bin\cpack.exe') 'cpack'
$ninja = Find-Tool @('D:\JetBrains\CLion 2024.1.4\bin\ninja\win\x64\ninja.exe',
                     (Join-Path $script:Root 'tools\ninja\ninja.exe')) 'ninja'
$makensis = Find-Tool @((Join-Path $script:Root '.tmp\pkg\nsis\nsis-bundle\windows\makensis.exe'),
                        (Join-Path $script:Root 'tools\nsis\makensis.exe')) 'makensis'

if (-not $cmake) { Fail '未找到 cmake' }
if (-not $ninja) { Fail '未找到 ninja（可设置 PATH，或安装 CLion / 放入 tools\ninja\ninja.exe）' }
Write-Host "vcvars64 : $vcvars"
Write-Host "cmake    : $cmake"
Write-Host "ninja    : $ninja"
Write-Host "makensis : $(if ($makensis) { $makensis } else { '(缺失)' })"

# —— 导入 MSVC 环境（INCLUDE/LIB 是命令行编译 Ninja 工程的前提）——
function Import-VcEnv([string]$vcvarsPath) {
    $lines = & cmd /c "call `"$vcvarsPath`" >nul 2>&1 && set"
    foreach ($line in $lines) {
        $idx = $line.IndexOf('=')
        if ($idx -gt 0) {
            $name = $line.Substring(0, $idx)
            $value = $line.Substring($idx + 1)
            Set-Item -Path "env:$name" -Value $value
        }
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host '导入 MSVC 环境变量...'
    Import-VcEnv $vcvars
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fail '导入 MSVC 环境失败（cl.exe 不可用）' }

# ————————————————————————————————————————————————————————————————
# 2. 配置 + 编译
# ————————————————————————————————————————————————————————————————
$buildPath = Join-Path $script:Root $BuildDir
$vcpkgInstalled = Join-Path $script:Root 'build\vcpkg_installed'
$vcpkgToolchain = 'F:\vcpkg\scripts\buildsystems\vcpkg.cmake'
if ($env:VCPKG_ROOT) { $vcpkgToolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake' }

if (-not $SkipBuild) {
    Write-Step "配置 $Config 构建（$buildPath）"
    if (-not (Test-Path (Join-Path $buildPath 'build.ninja'))) {
        $cfgArgs = @(
            '-S', $script:Root, '-B', $buildPath,
            '-G', 'Ninja',
            "-DCMAKE_BUILD_TYPE=$Config",
            "-DCMAKE_MAKE_PROGRAM=$ninja",
            "-DCMAKE_INSTALL_PREFIX=$(Join-Path $script:Root 'out\stage')",
            "-DMOLECULENS_PRODUCT_VERSION=$Version",
            '-DMOLECULENS_BUILD_TESTS=OFF'
        )
        if (Test-Path $vcpkgToolchain) { $cfgArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain" }
        if (Test-Path $vcpkgInstalled) {
            # 复用已有 vcpkg 安装树，避免重复编译依赖
            $cfgArgs += "-DVCPKG_INSTALLED_DIR=$vcpkgInstalled"
        }
        & $cmake @cfgArgs
        if ($LASTEXITCODE -ne 0) { Fail 'CMake 配置失败' }
    }

    Write-Step '编译 moleculens_app / moleculens-cli'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $cmake --build $buildPath --config $Config --target moleculens_app moleculens-cli -j 12
    if ($LASTEXITCODE -ne 0) { Fail '编译失败' }
    $sw.Stop()
    Write-Host ("编译完成，用时 {0:N1} 分钟" -f ($sw.Elapsed.TotalMinutes))
}

$appExe = Join-Path $buildPath "src\app\moleculens_app.exe"
$cliExe = Join-Path $buildPath "src\cli\moleculens-cli.exe"
if (-not (Test-Path $appExe)) { Fail "缺少 $appExe" }

# ————————————————————————————————————————————————————————————————
# 3. 安装到 staging（CMake install 规则见 cmake/packaging.cmake）
# ————————————————————————————————————————————————————————————————
Write-Step '生成安装目录（staging）'
$stage = Join-Path $script:Root 'out\stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
& $cmake --install $buildPath --prefix $stage --config $Config
if ($LASTEXITCODE -ne 0) { Fail 'cmake --install 失败' }

# 完整性自检：缺 DLL 会在用户机器上表现为 0xc0000135，必须在打包前拦住
$required = @('moleculens_app.exe', 'glfw3.dll', 'spdlog.dll', 'fmt.dll', 'font\NotoSansSC-Medium-subset.ttf', 'LICENSE')
foreach ($r in $required) {
    if (-not (Test-Path (Join-Path $stage $r))) { Fail "staging 缺少 $r" }
}
if (Test-Path $cliExe) {
    if (-not (Test-Path (Join-Path $stage 'moleculens-cli.exe'))) { Fail 'staging 缺少 moleculens-cli.exe' }
}
$stageFiles = Get-ChildItem $stage -Recurse -File
$stageMB = [math]::Round((($stageFiles | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "staging: $($stageFiles.Count) 个文件，$stageMB MB → $stage"

# ————————————————————————————————————————————————————————————————
# 4. 安装器品牌图（欢迎页 164x314 / 页眉 150x57）
# ————————————————————————————————————————————————————————————————
function New-BrandingBitmaps([string]$dir) {
    $welcome = Join-Path $dir 'welcome.bmp'
    $header  = Join-Path $dir 'header.bmp'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }

    Add-Type -AssemblyName System.Drawing
    $logoPath = Join-Path $script:Root 'docs\figs_v2\logo.png'

    $bg = [System.Drawing.Color]::FromArgb(255, 250, 248, 245)
    $accent = [System.Drawing.Color]::FromArgb(255, 122, 82, 48)

    # —— 欢迎页（竖版）——
    $bmp = New-Object System.Drawing.Bitmap 164, 314
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear($bg)
    if (Test-Path $logoPath) {
        $logo = [System.Drawing.Image]::FromFile($logoPath)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($logo, 22, 26, 120, 120)
        $logo.Dispose()
    }
    $fontBig = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold)
    $fontMid = New-Object System.Drawing.Font('Segoe UI', 9)
    $fontSmall = New-Object System.Drawing.Font('Segoe UI', 7.5)
    $dark = [System.Drawing.Color]::FromArgb(255, 60, 52, 46)
    $muted = [System.Drawing.Color]::FromArgb(255, 128, 118, 110)
    $brushDark = New-Object System.Drawing.SolidBrush $dark
    $brushMuted = New-Object System.Drawing.SolidBrush $muted
    $brushAccent = New-Object System.Drawing.SolidBrush $accent
    $g.DrawString('MolecuLens', $fontBig, $brushAccent, 22, 160)
    $g.DrawString('分子视镜', $fontMid, $brushDark, 23, 185)
    $g.DrawString("v$Version", $fontMid, $brushMuted, 23, 205)
    $g.DrawString('结构生物学 / 计算化学', $fontSmall, $brushMuted, 23, 240)
    $g.DrawString('Vulkan 实时渲染引擎', $fontSmall, $brushMuted, 23, 254)
    $g.Dispose()
    $bmp.Save($welcome, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp.Dispose()

    # —— 页眉（横版）——
    $bmp2 = New-Object System.Drawing.Bitmap 150, 57
    $g2 = [System.Drawing.Graphics]::FromImage($bmp2)
    $g2.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g2.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g2.Clear([System.Drawing.Color]::White)
    if (Test-Path $logoPath) {
        $logo2 = [System.Drawing.Image]::FromFile($logoPath)
        $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g2.DrawImage($logo2, 6, 8, 41, 41)
        $logo2.Dispose()
    }
    $fontH = New-Object System.Drawing.Font('Segoe UI', 9.5, [System.Drawing.FontStyle]::Bold)
    $g2.DrawString('MolecuLens', $fontH, $brushAccent, 52, 12)
    $g2.DrawString("Install v$Version", $fontSmall, $brushMuted, 53, 30)
    $g2.Dispose()
    $bmp2.Save($header, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp2.Dispose()

    $brushDark.Dispose(); $brushMuted.Dispose(); $brushAccent.Dispose()
    $fontBig.Dispose(); $fontMid.Dispose(); $fontSmall.Dispose(); $fontH.Dispose()
    return @{ Welcome = $welcome; Header = $header }
}

# ————————————————————————————————————————————————————————————————
# 5. NSIS 安装程序
# ————————————————————————————————————————————————————————————————
$dist = Join-Path $script:Root $OutDir
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Force $dist | Out-Null }

if (-not $SkipInstaller) {
    Write-Step '编译 NSIS 安装程序'
    if (-not $makensis) { Fail '未找到 makensis.exe（可放入 tools\nsis\ 或 .tmp\pkg\nsis\nsis-bundle\windows\）' }

    $nsisDir = Split-Path -Parent $makensis
    $env:NSISDIR = $nsisDir      # 本工具链的 Stubs/Contrib 在 exe 同级，必须显式指定
    $branding = New-BrandingBitmaps (Join-Path $buildPath 'packaging\branding')
    $icon = Join-Path $script:Root 'src\app\app.ico'

    $setupName = "MolecuLens-$Version-win64-Setup.exe"
    $setupPath = Join-Path $dist $setupName
    if (Test-Path $setupPath) { Remove-Item $setupPath -Force }

    $nsiArgs = @(
        "/DVERSION=$Version",
        "/DSTAGE=$stage",
        "/DOUTFILE=$setupPath",
        "/DAPPICON=$icon",
        "/DWELCOMEBMP=$($branding.Welcome)",
        "/DHEADERBMP=$($branding.Header)",
        (Join-Path $script:Root 'installer\moleculens.nsi')
    )
    & $makensis @nsiArgs
    if ($LASTEXITCODE -ne 0) { Fail 'makensis 编译失败' }
    if (-not (Test-Path $setupPath)) { Fail "安装程序未生成: $setupPath" }
    Write-Host ("安装程序: {0} ({1:N2} MB)" -f $setupPath, ((Get-Item $setupPath).Length / 1MB))
}

# ————————————————————————————————————————————————————————————————
# 6. 免安装 ZIP（cpack）
# ————————————————————————————————————————————————————————————————
if (-not $SkipZip) {
    Write-Step '生成免安装 ZIP'
    $cpackCfg = Join-Path $buildPath 'CPackConfig.cmake'
    if (-not (Test-Path $cpackCfg)) { Fail "缺少 $cpackCfg（请先配置构建树）" }
    & $cpack --config $cpackCfg -G ZIP -B $dist
    if ($LASTEXITCODE -ne 0) { Fail 'cpack 打包失败' }
    # CPack 的中间目录不属于发布物
    Remove-Item (Join-Path $dist '_CPack_Packages') -Recurse -Force -ErrorAction SilentlyContinue
}

# ————————————————————————————————————————————————————————————————
# 7. 校验和
# ————————————————————————————————————————————————————————————————
# 说明：部分受限环境（精简/加固的 PowerShell）里 Get-FileHash 所在模块不可用，
#       这里直接用 .NET 的 SHA256，不依赖额外模块。
function Get-Sha256Hex([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try {
        $bytes = $sha.ComputeHash($fs)
    } finally {
        $fs.Dispose()
        $sha.Dispose()
    }
    return ([System.BitConverter]::ToString($bytes) -replace '-', '').ToLower()
}

Write-Step '生成 SHA256 校验和'
$checksumFile = Join-Path $dist 'SHA256SUMS.txt'
$artifacts = Get-ChildItem $dist -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' }
$lines = foreach ($f in $artifacts) {
    "$(Get-Sha256Hex $f.FullName)  $($f.Name)"
}
$lines | Set-Content -Path $checksumFile -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }

Write-Step '完成'
Write-Host "产物目录: $dist"
Get-ChildItem $dist -File | Select-Object Name, @{n='Size(MB)'; e={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize
