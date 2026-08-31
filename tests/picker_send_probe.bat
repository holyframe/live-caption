@echo off
setlocal
set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" exit /b 2
call "%VSDEV%" >nul
if errorlevel 1 exit /b 2
pushd "%~dp0.."
if not exist "build\pickertestobj" mkdir "build\pickertestobj"
cl /nologo /std:c++20 /W4 /permissive- /utf-8 /EHsc /MT /O2 ^
 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I src ^
 tests\picker_send_probe.cpp /Fo"build\pickertestobj\\" /Fe"build\picker_send_probe.exe" ^
 /link user32.lib ole32.lib oleaut32.lib uuid.lib dwmapi.lib
if errorlevel 1 (popd & exit /b 1)
popd
exit /b 0
