@echo off
rem Runs a command inside the latest Visual Studio x64 developer environment (cl, link, INCLUDE, LIB).
rem Usage: tools\vsdev.cmd cl /nologo foo.cpp
rem        tools\vsdev.cmd cmake -G Ninja -B build && tools\vsdev.cmd cmake --build build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio with the C++ workload & exit /b 1)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo no Visual Studio with the MSVC x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
%*
