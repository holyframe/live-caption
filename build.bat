@echo off
REM Builds LiveCaptionView.exe with MSVC without requiring CMake.
REM Usage: build.bat [debug]

setlocal enabledelayedexpansion

set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSDEV=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not exist "%VSDEV%" (
    echo ERROR: Could not locate vcvars64.bat. Install the "Desktop development with C++" workload.
    exit /b 1
)

call "%VSDEV%" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialise the MSVC environment.
    exit /b 1
)

set "OUTDIR=%~dp0build"
set "OBJDIR=%OUTDIR%\obj"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CFLAGS=/nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I src /I resources"
set "LIBS=user32.lib gdi32.lib comctl32.lib d2d1.lib dwrite.lib ole32.lib oleaut32.lib uuid.lib dwmapi.lib shlwapi.lib uxtheme.lib"

if /i "%~1"=="debug" (
    set "CFLAGS=%CFLAGS% /MTd /Od /Zi /RTC1"
    set "LDFLAGS=/DEBUG"
) else (
    set "CFLAGS=%CFLAGS% /MT /O2 /GL"
    set "LDFLAGS=/LTCG /OPT:REF /OPT:ICF"
)

echo === Compiling resources ===
rc /nologo /I resources /fo "%OBJDIR%\app.res" resources\app.rc
if errorlevel 1 exit /b 1

echo === Compiling sources ===
cl %CFLAGS% /c ^
    src\main.cpp ^
    src\Util.cpp ^
    src\Settings.cpp ^
    src\CaptionMerger.cpp ^
    src\TranscriptStore.cpp ^
    src\CaptionSource.cpp ^
    src\CaptureEngine.cpp ^
    src\CaptionView.cpp ^
    src\WebInputPicker.cpp ^
    src\MainWindow.cpp ^
    /Fo"%OBJDIR%\\"
if errorlevel 1 exit /b 1

echo === Linking ===
link /nologo /SUBSYSTEM:WINDOWS %LDFLAGS% ^
    "%OBJDIR%\*.obj" "%OBJDIR%\app.res" %LIBS% ^
    /OUT:"%OUTDIR%\LiveCaptionView.exe"
if errorlevel 1 exit /b 1

echo.
echo Build succeeded: %OUTDIR%\LiveCaptionView.exe
endlocal
