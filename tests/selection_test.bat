@echo off
REM Builds and runs the CaptionView selection checks, then converts the probe's
REM screenshots to PNG so they can be viewed.
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
set "OBJDIR=build\testobj"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

cl /nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /MT /O2 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I src /I resources ^
   tests\selection_test.cpp src\CaptionView.cpp ^
   /Fo"%OBJDIR%\\" /Fe"build\selection_test.exe" ^
   /link user32.lib gdi32.lib d2d1.lib dwrite.lib ole32.lib
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
build\selection_test.exe
set "RESULT=%ERRORLEVEL%"

for %%B in (selection_text selection_gutter) do (
    if exist "build\%%B.bmp" (
        powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; $b=[System.Drawing.Image]::FromFile((Resolve-Path 'build\%%B.bmp')); $b.Save((Join-Path (Get-Location) 'build\%%B.png'), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); Remove-Item 'build\%%B.bmp'"
    )
)

popd
exit /b %RESULT%
