@echo off
REM Builds and runs the repaint-cost diagnostic.
setlocal

set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSDEV=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not exist "%VSDEV%" (
    echo ERROR: Could not locate vcvars64.bat.
    exit /b 1
)
call "%VSDEV%" >nul

pushd "%~dp0.."
set "OBJDIR=build\probeobj"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

cl /nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /MT /O2 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   tests\render_probe.cpp ^
   /Fo"%OBJDIR%\\" /Fe"build\render_probe.exe" ^
   /link user32.lib gdi32.lib d2d1.lib dwrite.lib ole32.lib
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
build\render_probe.exe
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
