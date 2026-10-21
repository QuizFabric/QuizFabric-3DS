@echo off
REM ---------------------------------------------------------------------------
REM Shared devkitPro locator. Sets DKP_ROOT, DKP_BASH and MSYS_PROJ.
REM Called by the build scripts; running it on its own does nothing useful.
REM
REM devkitPro's env vars (DEVKITPRO=/opt/devkitpro) are MSYS2 paths, so builds
REM have to go through its bash. `bash -l` sources the devkit profile for us.
REM ---------------------------------------------------------------------------

set "DKP_ROOT="
if defined DEVKITPRO_WIN if exist "%DEVKITPRO_WIN%\msys2\usr\bin\bash.exe" set "DKP_ROOT=%DEVKITPRO_WIN%"
if not defined DKP_ROOT if exist "C:\devkitPro\msys2\usr\bin\bash.exe" set "DKP_ROOT=C:\devkitPro"
if not defined DKP_ROOT if exist "D:\devkitPro\msys2\usr\bin\bash.exe" set "DKP_ROOT=D:\devkitPro"
if not defined DKP_ROOT if exist "E:\devkitPro\msys2\usr\bin\bash.exe" set "DKP_ROOT=E:\devkitPro"
if not defined DKP_ROOT if exist "%SystemDrive%\devkitPro\msys2\usr\bin\bash.exe" set "DKP_ROOT=%SystemDrive%\devkitPro"

if not defined DKP_ROOT goto :nodkp

set "DKP_BASH=%DKP_ROOT%\msys2\usr\bin\bash.exe"

REM Convert this script's folder to an MSYS2 path:  E:\3DSTHINGY  ->  /e/3DSTHINGY
set "PROJ=%~dp0"
set "PROJ=%PROJ:~0,-1%"
set "DL=%PROJ:~0,1%"
for %%L in (a b c d e f g h i j k l m n o p q r s t u v w x y z) do if /i "%DL%"=="%%L" set "DL=%%L"
set "PROJREST=%PROJ:~2%"
set "PROJREST=%PROJREST:\=/%"
set "MSYS_PROJ=/%DL%%PROJREST%"

REM A bannertool.exe dropped in .\tools is picked up automatically.
set "DKP_PATH_EXTRA=%MSYS_PROJ%/tools"

REM devkitPro ships makerom and bannertool under its tools\ tree but does not
REM put them on PATH; add both so the CIA scripts find them.
set "DKP_TOOLS_MSYS=/opt/devkitpro/tools/makerom:/opt/devkitpro/tools/bannertool/windows-x86_64"
exit /b 0

:nodkp
echo.
echo   ============================================================
echo    devkitPro was not found on this machine.
echo   ============================================================
echo.
echo    Install it from:
echo      https://github.com/devkitPro/installer/releases
echo.
echo    During setup, tick the "3DS Development" package group.
echo    That provides devkitARM, libctru, citro3d and citro2d.
echo.
echo    Installed somewhere other than C:\devkitPro? Point at it:
echo      setx DEVKITPRO_WIN "D:\devkitPro"
echo    then open a new terminal and try again.
echo.
exit /b 1
