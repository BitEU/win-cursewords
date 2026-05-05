@echo off
rem Build win-cursewords.exe with MSVC.
rem Auto-locates and invokes vcvars64.bat so this works from any shell.

setlocal EnableDelayedExpansion

set "OUT=win-cursewords.exe"
set "SRC=win-cursewords.c puz.c"

rem If cl is already on PATH (e.g. running from a VS Developer prompt), skip vcvars.
where cl.exe >nul 2>&1
if %errorlevel%==0 goto have_cl

rem Read Program Files (x86) into a delayed-expansion var so the literal
rem "(x86)" never appears inside a parenthesized block (cmd's parser would
rem treat it as a closing paren).
set "PFX=!ProgramFiles(x86)!"
set "PF=%ProgramFiles%"

set "VSWHERE=!PFX!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=!PF!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" goto no_vswhere

for /f "usebackq tokens=*" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSINSTALL=%%i"

:no_vswhere

if not defined VSINSTALL goto no_vs

set "VCVARS=!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" goto no_vs

echo Initializing MSVC environment from "!VSINSTALL!"...
call "!VCVARS!" >nul
if errorlevel 1 goto init_failed

goto have_cl

:no_vs
echo Could not find a Visual Studio installation with the C++ tools.
echo Install "Desktop development with C++" via the VS Installer, or run
echo this script from a Developer Command Prompt.
exit /b 1

:init_failed
echo Failed to initialize MSVC environment.
exit /b 1

:have_cl

cl /nologo /W3 /O2 /MT /utf-8 /D_CRT_NONSTDC_NO_DEPRECATE /Fe:%OUT% %SRC% kernel32.lib user32.lib
if errorlevel 1 goto build_failed

echo.
echo Built %OUT%.
endlocal
exit /b 0

:build_failed
echo.
echo Build failed.
endlocal
exit /b 1
