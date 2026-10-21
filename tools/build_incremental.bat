@echo off
rem Incremental build (Debug). Usage: build_incremental.bat [extra MSBuild args]
rem NOTE: keep this file ASCII-only with CRLF line endings. cmd.exe parses .bat
rem files with the OEM code page; UTF-8 Chinese comments used to split lines and
rem made the script fail with "'d' is not recognized as an internal command".
setlocal
call "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build] failed to initialize MSVC environment
    exit /b 1
)
MSBuild.exe "F:\MolecuLens\build\MolecuLens.sln" -t:Build -p:Configuration=Debug -m -v:minimal %*
exit /b %ERRORLEVEL%
