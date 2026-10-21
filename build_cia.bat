@echo off
setlocal
title QuizFabric 3DS - build CIA

call "%~dp0_devkit_env.bat"
if errorlevel 1 exit /b 1

REM makerom and bannertool are what turn the ELF into an installable title.
REM Both ship under devkitPro's tools\ tree; the shared locator adds them to
REM the bash PATH (DKP_TOOLS_MSYS), with .\tools as an extra fallback.
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; command -v makerom >/dev/null 2>&1"
if errorlevel 1 goto :nomakerom

"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; command -v bannertool >/dev/null 2>&1"
if errorlevel 1 goto :nobannertool

echo.
echo   Building QuizFabric3DS.cia ...
echo.

"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%' && make cia"
if errorlevel 1 goto :failed

echo.
echo   ------------------------------------------------------------
echo    Built:  QuizFabric3DS.cia
echo   ------------------------------------------------------------
echo.
echo    Copy it to your SD card and install with FBI.
echo.
pause
exit /b 0

:nomakerom
echo.
echo   ============================================================
echo    makerom was not found.
echo   ============================================================
echo.
echo    Try installing it through devkitPro's package manager:
echo      "%DKP_ROOT%\msys2\usr\bin\bash.exe" -l -c "pacman -S 3dstools"
echo.
echo    Or download a build and drop makerom.exe into:
echo      %~dp0tools\
echo      https://github.com/3DSGuy/Project_CTR/releases
echo.
pause
exit /b 1

:nobannertool
echo.
echo   ============================================================
echo    bannertool was not found.
echo   ============================================================
echo.
echo    bannertool is not part of devkitPro. Download bannertool.exe
echo    and drop it into:
echo      %~dp0tools\
echo.
echo      https://github.com/Steveice10/bannertool/releases
echo.
echo    (The 3DSX build does not need it - use build_3dsx.bat.)
echo.
pause
exit /b 1

:failed
echo.
echo   ************************************************************
echo    BUILD FAILED - see the output above.
echo   ************************************************************
echo.
pause
exit /b 1
