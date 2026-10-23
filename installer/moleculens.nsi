; =============================================================================
; MolecuLens · installer/moleculens.nsi —— Windows 安装程序（NSIS 3 / Unicode）
; -----------------------------------------------------------------------------
; 设计要点
;   · 按用户安装（RequestExecutionLevel user）：装到 %LOCALAPPDATA%\Programs，
;     不需要管理员权限，也不触发 UAC；程序运行时会往安装目录写 logs/ 与
;     imgui.ini，按用户安装天然可写，避免 Program Files 的权限问题。
;   · 组件化：主程序（必需）/ 命令行工具 / 示例数据 / 开始菜单 / 桌面快捷方式。
;   · 卸载走 HKCU 的 Uninstall 键，控制面板「应用和功能」可正常卸载与静默卸载。
;   · 安装前探测 64 位系统与 Vulkan 运行库（缺失只提示，不阻断安装）。
;
; 编译（由 tools/package_release.ps1 调用）：
;   makensis /DVERSION=2.19.0 /DSTAGE=<stage> /DOUTFILE=<out.exe> \
;            /DAPPICON=<ico> /DWELCOMEBMP=<bmp> /DHEADERBMP=<bmp> moleculens.nsi
;
; 注意：本文件必须保存为 **UTF-8 with BOM**，否则 NSIS 3 会按系统代码页解析中文。
; =============================================================================

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 32
SetDatablockOptimize on
CRCCheck on

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

; —— 编译期参数（缺省值用于手工调试）——
!ifndef VERSION
  !define VERSION "2.19.0"
!endif
!ifndef STAGE
  !error "STAGE 未定义：请传入 /DSTAGE=<install staging dir>"
!endif
!ifndef OUTFILE
  !define OUTFILE "MolecuLens-${VERSION}-win64-Setup.exe"
!endif
!ifndef APPICON
  !define APPICON ""
!endif
!ifndef WELCOMEBMP
  !define WELCOMEBMP ""
!endif
!ifndef HEADERBMP
  !define HEADERBMP ""
!endif

!define APP_NAME   "MolecuLens"
!define APP_EXE    "moleculens_app.exe"
!define CLI_EXE    "moleculens-cli.exe"
!define APP_REG    "Software\MolecuLens"
!define UNINST_REG "Software\Microsoft\Windows\CurrentVersion\Uninstall\MolecuLens"
!define APP_URL    "https://github.com/moyudoing/MolecuLens"

; =============================================================================
; 基本信息
; =============================================================================
Name "${APP_NAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\MolecuLens"
InstallDirRegKey HKCU "${APP_REG}" "InstallDir"
RequestExecutionLevel user
BrandingText "${APP_NAME} ${VERSION} · 分子视镜"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=2052 "ProductName"     "${APP_NAME}"
VIAddVersionKey /LANG=2052 "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion"     "${VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion"  "${VERSION}"
VIAddVersionKey /LANG=2052 "LegalCopyright"  "Apache License 2.0"
VIAddVersionKey /LANG=2052 "CompanyName"     "MolecuLens Project"

; —— 图标与横幅（打包脚本生成后传入；缺失时用 NSIS 默认）——
!if "${APPICON}" != ""
  !define MUI_ICON          "${APPICON}"
  !define MUI_UNICON        "${APPICON}"
  Icon          "${APPICON}"
  UninstallIcon "${APPICON}"
!endif
!if "${WELCOMEBMP}" != ""
  !define MUI_WELCOMEFINISHPAGE_BITMAP    "${WELCOMEBMP}"
  !define MUI_UNWELCOMEFINISHPAGE_BITMAP "${WELCOMEBMP}"
!endif
!if "${HEADERBMP}" != ""
  !define MUI_HEADERIMAGE
  !define MUI_HEADERIMAGE_BITMAP "${HEADERBMP}"
  !define MUI_HEADERIMAGE_RIGHT
!endif
!define MUI_ABORTWARNING

; =============================================================================
; 页面
; =============================================================================
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_LINK "项目主页 / Project page"
!define MUI_FINISHPAGE_LINK_LOCATION "${APP_URL}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; —— 语言（必须在 LangString / 页面文本之前插入）——
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; =============================================================================
; 本地化文案（组件名 / 提示）
; =============================================================================
LangString SEC_CORE_N ${LANG_SIMPCHINESE} "MolecuLens 主程序（必需）"
LangString SEC_CORE_N ${LANG_ENGLISH}     "MolecuLens application (required)"
LangString SEC_CORE_D ${LANG_SIMPCHINESE} "图形界面、Vulkan 渲染引擎、字体与许可文件。"
LangString SEC_CORE_D ${LANG_ENGLISH}     "GUI, Vulkan renderer, bundled font and licenses."

LangString SEC_CLI_N ${LANG_SIMPCHINESE} "命令行工具 moleculens-cli"
LangString SEC_CLI_N ${LANG_ENGLISH}     "Command-line tool moleculens-cli"
LangString SEC_CLI_D ${LANG_SIMPCHINESE} "无头渲染 / 格式转换 / 结构信息，可与 GUI 使用同一套参数复现出图。"
LangString SEC_CLI_D ${LANG_ENGLISH}     "Headless render, format conversion and structure info."

LangString SEC_DATA_N ${LANG_SIMPCHINESE} "示例分子数据"
LangString SEC_DATA_N ${LANG_ENGLISH}     "Sample molecular data"
LangString SEC_DATA_D ${LANG_SIMPCHINESE} "EPI.pdb、md.gro 等 9 个演示文件，用于首次启动验证与文档复现。"
LangString SEC_DATA_D ${LANG_ENGLISH}     "Nine demo files (EPI.pdb, md.gro, ...) for first-run checks."

LangString SEC_LINK_N ${LANG_SIMPCHINESE} "开始菜单快捷方式"
LangString SEC_LINK_N ${LANG_ENGLISH}     "Start Menu shortcuts"
LangString SEC_LINK_D ${LANG_SIMPCHINESE} "在开始菜单创建 MolecuLens 程序组（主程序 / 使用说明 / 卸载）。"
LangString SEC_LINK_D ${LANG_ENGLISH}     "Creates a MolecuLens Start Menu group."

LangString SEC_DESK_N ${LANG_SIMPCHINESE} "桌面快捷方式"
LangString SEC_DESK_N ${LANG_ENGLISH}     "Desktop shortcut"
LangString SEC_DESK_D ${LANG_SIMPCHINESE} "在桌面创建 MolecuLens 图标。"
LangString SEC_DESK_D ${LANG_ENGLISH}     "Creates a MolecuLens icon on the Desktop."

LangString VULKAN_MISSING ${LANG_SIMPCHINESE} "未检测到 Vulkan 运行库（%SystemRoot%\System32\vulkan-1.dll）。$\r$\n$\r$\nMolecuLens 使用 Vulkan 渲染。请安装显卡厂商的最新驱动（NVIDIA / AMD / Intel 驱动自带 Vulkan 运行库）后重试。"
LangString VULKAN_MISSING ${LANG_ENGLISH}     "Vulkan runtime (vulkan-1.dll) was not found.$\r$\n$\r$\nMolecuLens renders with Vulkan. Install the latest GPU driver (NVIDIA/AMD/Intel include the loader) and try again."
LangString NOT_X64 ${LANG_SIMPCHINESE} "此安装包为 64 位版本，当前系统不支持。"
LangString NOT_X64 ${LANG_ENGLISH}     "This package is 64-bit only and cannot run on this system."
LangString APP_RUNNING ${LANG_SIMPCHINESE} "MolecuLens 正在运行。安装程序需要先关闭它才能覆盖程序文件。$\r$\n$\r$\n是否立即关闭 MolecuLens 并继续安装？"
LangString APP_RUNNING ${LANG_ENGLISH}     "MolecuLens is currently running. It must be closed to update its files.$\r$\n$\r$\nClose MolecuLens now and continue?"

; =============================================================================
; 初始化：64 位 / Vulkan 检查
; =============================================================================
Function .onInit
  SetRegView 64            ; HKCU 也受 WOW64 重定向影响，统一写 64 位视图
  SetShellVarContext current

  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(NOT_X64)"
    Abort
  ${EndIf}

  ; Vulkan 只提示、不阻断（静默安装不弹窗）
  ${IfNot} ${Silent}
    ${IfNot} ${FileExists} "$SYSDIR\vulkan-1.dll"
      MessageBox MB_ICONINFORMATION|MB_OK "$(VULKAN_MISSING)"
    ${EndIf}
  ${EndIf}

  ; 旧版本仍在运行 → 文件被占用会让 File 解压失败，先关闭
  nsProcessW::_FindProcess "${APP_EXE}"
  Pop $0
  ${If} $0 == 0
    IfSilent ml_kill_running
    MessageBox MB_OKCANCEL|MB_ICONQUESTION "$(APP_RUNNING)" IDOK ml_kill_running IDCANCEL ml_abort_running
    ml_kill_running:
      nsProcessW::_KillProcess "${APP_EXE}"
      Pop $0
      Sleep 800
      Goto ml_running_done
    ml_abort_running:
      Abort
  ${EndIf}
  ml_running_done:
FunctionEnd

; =============================================================================
; 安装区段
; =============================================================================
Section "$(SEC_CORE_N)" SEC_CORE
  SectionIn RO
  SetOutPath "$INSTDIR"

  File "${STAGE}\${APP_EXE}"
  ; 运行库：glfw3.dll / spdlog.dll / fmt.dll + app-local MSVC CRT
  File "${STAGE}\*.dll"

  SetOutPath "$INSTDIR\font"
  File /r "${STAGE}\font\*.*"

  SetOutPath "$INSTDIR\docs"
  File "${STAGE}\docs\UserGuide-zh.md"
  File "${STAGE}\README.zh-CN.md"
  File "${STAGE}\CHANGELOG.md"
  File "${STAGE}\THIRD_PARTY_NOTICES.md"

  SetOutPath "$INSTDIR"
  File "${STAGE}\LICENSE"
  File "${STAGE}\NOTICE"

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; 控制面板「应用和功能」条目（HKCU，按用户卸载）
  WriteRegStr   HKCU "${APP_REG}" "InstallDir" "$INSTDIR"
  WriteRegStr   HKCU "${APP_REG}" "Version"    "${VERSION}"
  WriteRegStr   HKCU "${UNINST_REG}" "DisplayName"          "${APP_NAME}"
  WriteRegStr   HKCU "${UNINST_REG}" "DisplayVersion"       "${VERSION}"
  WriteRegStr   HKCU "${UNINST_REG}" "Publisher"            "MolecuLens Project"
  WriteRegStr   HKCU "${UNINST_REG}" "DisplayIcon"          "$INSTDIR\${APP_EXE},0"
  WriteRegStr   HKCU "${UNINST_REG}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_REG}" "URLInfoAbout"         "${APP_URL}"
  WriteRegStr   HKCU "${UNINST_REG}" "UninstallString"      '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKCU "${UNINST_REG}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_REG}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_REG}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${UNINST_REG}" "EstimatedSize" $0
SectionEnd

Section "$(SEC_CLI_N)" SEC_CLI
  SetOutPath "$INSTDIR"
  File "${STAGE}\${CLI_EXE}"
SectionEnd

Section "$(SEC_DATA_N)" SEC_DATA
  SetOutPath "$INSTDIR\data"
  File /r "${STAGE}\data\*.*"
SectionEnd

Section "$(SEC_LINK_N)" SEC_LINK
  CreateDirectory "$SMPROGRAMS\MolecuLens"
  CreateShortCut "$SMPROGRAMS\MolecuLens\MolecuLens.lnk" "$INSTDIR\${APP_EXE}" "" \
                 "$INSTDIR\${APP_EXE}" 0 SW_SHOWNORMAL "" "MolecuLens 分子视镜"
  CreateShortCut "$SMPROGRAMS\MolecuLens\使用说明.lnk" "$INSTDIR\docs\UserGuide-zh.md"
  CreateShortCut "$SMPROGRAMS\MolecuLens\卸载 MolecuLens.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "$(SEC_DESK_N)" SEC_DESK
  CreateShortCut "$DESKTOP\MolecuLens.lnk" "$INSTDIR\${APP_EXE}" "" \
                 "$INSTDIR\${APP_EXE}" 0 SW_SHOWNORMAL "" "MolecuLens 分子视镜"
SectionEnd

; —— 组件说明 ——
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE} "$(SEC_CORE_D)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CLI}  "$(SEC_CLI_D)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DATA} "$(SEC_DATA_D)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_LINK} "$(SEC_LINK_D)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESK} "$(SEC_DESK_D)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; =============================================================================
; 卸载
; =============================================================================
Function un.onInit
  SetRegView 64
  SetShellVarContext current
FunctionEnd

Section "Uninstall"
  SetShellVarContext current

  ; 快捷方式
  Delete "$SMPROGRAMS\MolecuLens\MolecuLens.lnk"
  Delete "$SMPROGRAMS\MolecuLens\使用说明.lnk"
  Delete "$SMPROGRAMS\MolecuLens\卸载 MolecuLens.lnk"
  RMDir  "$SMPROGRAMS\MolecuLens"
  Delete "$DESKTOP\MolecuLens.lnk"

  ; 程序文件（%APPDATA%\MolecuLens\settings.json 保留：用户偏好不随卸载删除）
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\${CLI_EXE}"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\NOTICE"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.md"
  Delete "$INSTDIR\*.log"
  Delete "$INSTDIR\imgui.ini"
  Delete "$INSTDIR\settings.json"
  Delete "$INSTDIR\terminate.txt"
  RMDir /r "$INSTDIR\font"
  RMDir /r "$INSTDIR\docs"
  RMDir /r "$INSTDIR\data"
  ; 运行期生成的日志目录（程序把 logs/ 写在安装目录，按用户安装天然可写）
  RMDir /r "$INSTDIR\logs"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${UNINST_REG}"
  DeleteRegKey HKCU "${APP_REG}"
SectionEnd
