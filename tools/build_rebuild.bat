@echo off
rem Full rebuild (Debug). Usage: build_rebuild.bat [extra MSBuild args]
rem NOTE: keep this file ASCII-only with CRLF line endings (see build_incremental.bat).
setlocal
call "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build] failed to initialize MSVC environment
    exit /b 1
)
MSBuild.exe "F:\MolecuLens\build\MolecuLens.sln" -t:Rebuild -p:Configuration=Debug -m -v:minimal %*
exit /b %ERRORLEVEL%
