@echo off
rem Builds Trials Evolution with the Visual Studio LLVM clang first on PATH.
rem Usage: build.bat [te-relwithdebinfo|te-release|te-debug|te-dev]
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=te-relwithdebinfo
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
