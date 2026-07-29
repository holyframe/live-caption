@echo off
REM Builds and runs the CaptionMerger checks.
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

REM /O2 matters here: the timing checks are meaningless against an unoptimised
REM build, since every inlined helper becomes a real call.
cl /nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /MT /O2 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I src ^
   tests\merge_test.cpp src\CaptionMerger.cpp src\Util.cpp ^
   /Fo"%OBJDIR%\\" /Fe"build\merge_test.exe" ^
   /link user32.lib
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
build\merge_test.exe
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
    popd
    exit /b %RESULT%
)

echo.
cl /nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /MT /O2 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I src ^
   tests\source_filter_test.cpp ^
   /Fo"%OBJDIR%\\" /Fe"build\source_filter_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
build\source_filter_test.exe
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
    popd
    exit /b %RESULT%
)

echo.
cl /nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /MT /O2 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I src ^
   tests\caption_save_test.cpp src\CaptionSave.cpp ^
   /Fo"%OBJDIR%\\" /Fe"build\caption_save_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
build\caption_save_test.exe
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
