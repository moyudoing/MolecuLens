# 批量采集软著材料插图。
#
# 前置：应用已启动、窗口已设为 1600x1000、工作目录 data/ 下有演示结构。
# 约定：所有坐标都是**窗口相对坐标**（_ui.ps1 内部换算成屏幕坐标）。
#
# 下拉框几何（1600x1000 实测）：点开组合框后首项位于 组合框 y+33，
# 行距 22.5，条目横向取组合框中部。
param(
    [string]$Figs = 'F:\MolecuLens\软件注册\figs',
    [string]$AppData = 'F:\MolecuLens\build\src\app\Debug\data'
)

$ErrorActionPreference = 'Stop'
$UI = 'F:\MolecuLens\软件注册\_ui.ps1'

# 控件坐标（1600x1000 实测）
$STYLE_X = 1428; $STYLE_Y = 598     # 样式下拉框
$COLOR_X = 1428; $COLOR_Y = 631     # 着色下拉框
$ITEM_X  = 1363                     # 下拉项横向位置
$FIRST_DY = 33                      # 首项相对组合框的纵向偏移
$ITEM_H = 22.5                      # 行距

# 点开下拉框并选第 idx 项（0 基）。
# 必须先按 ESC 清场：若上一轮的下拉框/弹窗还开着，这一击会被它吃掉，
# 下拉框没打开，选项目的点击就会落到面板其它控件上（实测把"球体缩放"
# 和"键半径"滑杆点成了 0.166 / 0.440，分子因此又小又暗）。
function SelectCombo {
    param([int]$ComboX, [int]$ComboY, [int]$Idx)
    & $UI -Action keys -Text '{ESC}' | Out-Null
    Start-Sleep -Milliseconds 350
    & $UI -Action click -X $ComboX -Y $ComboY
    Start-Sleep -Milliseconds 750
    $itemY = [int]($ComboY + $FIRST_DY + $Idx * $ITEM_H)
    & $UI -Action click -X $ITEM_X -Y $itemY
    Start-Sleep -Milliseconds 900
}

# 把"球体缩放/键半径"复位为 1.000：点滑杆右侧的 R 小按钮
function ResetScales {
    & $UI -Action click -X 1564 -Y 653 | Out-Null
    Start-Sleep -Milliseconds 300
    & $UI -Action click -X 1564 -Y 683 | Out-Null
    Start-Sleep -Milliseconds 500
}

# 选样式 + 复位缩放 + 截图（缩放被误点会让分子变小变暗，每次都要复位）
function SelectStyleAndShot {
    param([int]$Idx, [string]$Name)
    SelectCombo $STYLE_X $STYLE_Y $Idx
    ResetScales
    Shot $Name
}
Set-Alias selectStyle SelectStyleAndShot

# 截图：隐藏输入法条后立即抓屏（不改变前台窗口，否则输入法条会复位）
function Shot {
    param([string]$Name)
    & $UI -Action shotclean -Out (Join-Path $Figs $Name)
}

function OpenFile {
    param([string]$Path)
    # 先按 Esc 关掉上一轮可能残留的下拉框/弹窗，否则首次点击会被它吃掉
    & $UI -Action keys -Text '{ESC}' | Out-Null
    Start-Sleep -Milliseconds 400
    & $UI -Action click -X 21 -Y 13          # 文件菜单
    Start-Sleep -Milliseconds 700
    & $UI -Action click -X 60 -Y 39          # 打开
    Start-Sleep -Seconds 2
    & $UI -Action openfile -Text $Path
    Start-Sleep -Seconds 7
}

Write-Output '=== 采集开始 ==='

# 每次采集都从全新进程开始：上一次运行残留的体系/表示会让截图状态不确定
# （实测：重复运行会累积出两个重叠体系，早期截图视口为空）。
Get-Process -Name 'moleculens_app' -ErrorAction SilentlyContinue |
    ForEach-Object { taskkill /PID $_.Id /F 2>&1 | Out-Null }
Start-Sleep -Seconds 2
$appExe = Join-Path (Split-Path $AppData) 'moleculens_app.exe'
Start-Process -FilePath $appExe -WorkingDirectory (Split-Path $AppData) | Out-Null
Start-Sleep -Seconds 9
& $UI -Action resize -W 1600 -H 1000

# ------------------------------------------------------- 蛋白质 1HHO：表示方式与配色
OpenFile (Join-Path $AppData '1HHO.pdb')
Shot 'fig_main.png'
ResetScales

SelectCombo $COLOR_X $COLOR_Y 1;  Shot 'fig_color_chain.png'
SelectCombo $COLOR_X $COLOR_Y 2;  Shot 'fig_color_residue.png'
SelectCombo $COLOR_X $COLOR_Y 3;  Shot 'fig_color_bfactor.png'
SelectCombo $COLOR_X $COLOR_Y 4;  Shot 'fig_color_uniform.png'
SelectCombo $COLOR_X $COLOR_Y 0;  Shot 'fig_color_element.png'

selectStyle 6  'fig_style_cartoon.png'
selectStyle 7  'fig_style_newcartoon.png'
selectStyle 8  'fig_style_ribbon.png'
selectStyle 9  'fig_style_tube.png'
selectStyle 10 'fig_style_surface.png'
selectStyle 11 'fig_style_quicksurf.png'
selectStyle 5  'fig_style_licorice.png'

# ------------------------------------------------------- 小分子 EPI：半径类表示
OpenFile (Join-Path $AppData 'EPI.pdb')
SelectStyleAndShot 2 'fig_small_ballstick.png'
SelectStyleAndShot 1 'fig_small_stick.png'
SelectStyleAndShot 3 'fig_small_cpk.png'
SelectStyleAndShot 4 'fig_small_vdw.png'
SelectStyleAndShot 0 'fig_small_wire.png'

Write-Output '=== 采集结束 ==='
Get-ChildItem $Figs -Filter 'fig_*.png' | Sort-Object Name |
    ForEach-Object { '  {0,-30} {1}' -f $_.Name, $_.Length }

