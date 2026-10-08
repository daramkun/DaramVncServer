@echo off
rem Configures (first time: vcpkg builds zlib, zlib-ng, libjpeg-turbo and OpenSSL) and builds everything into build\.
rem The Visual Studio install is located with vswhere, so no developer prompt is needed.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found: install Visual Studio ^(or Build Tools^) with the "Desktop development with C++" workload.
    exit /b 1
)
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo No Visual Studio with the MSVC x64 toolset was found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cmake --preset release || exit /b 1
cmake --build build || exit /b 1
echo.
echo Build finished: %CD%\build
