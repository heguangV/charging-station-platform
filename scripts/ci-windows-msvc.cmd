@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio locator was not found: %VSWHERE%
    exit /b 2
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL (
    echo A Visual Studio installation with the C++ toolchain was not found.
    exit /b 3
)

call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b %errorlevel%

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo cl.exe is unavailable after initializing Visual Studio.
    exit /b 4
)

cmake --preset dev -DCMAKE_CXX_COMPILER=cl.exe
if errorlevel 1 exit /b %errorlevel%

cmake --build --preset dev --parallel 2
if errorlevel 1 exit /b %errorlevel%

ctest --preset dev
if errorlevel 1 exit /b %errorlevel%
