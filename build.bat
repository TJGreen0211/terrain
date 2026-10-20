@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="

if not exist "%VSWHERE%" goto :try_fallback
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\vs_root.txt" 2>nul
set /p VSROOT=<"%TEMP%\vs_root.txt"
del "%TEMP%\vs_root.txt" 2>nul
if defined VSROOT if exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"

:try_fallback
if defined VCVARS goto :env
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS goto :env
if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS goto :env

echo Error: could not locate vcvars64.bat. Install VS 2022 with the C++ workload. 1>&2
exit /b 1

:env
call "%VCVARS%" >nul
if errorlevel 1 (
  echo Error: failed to initialize MSVC x64 environment. 1>&2
  exit /b 1
)

pushd "%~dp0src"
nmake %*
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
  echo Build failed with exit code %RC%. 1>&2
  exit /b %RC%
)
echo Build succeeded: %~dp0src\cube.exe
exit /b 0
