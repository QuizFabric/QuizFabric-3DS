@echo off
setlocal
title QuizFabric 3DS - build 3DSX + CIA

call "%~dp0_devkit_env.bat"
if errorlevel 1 exit /b 1

echo.
echo   Building QuizFabric3DS.3dsx ...
echo.
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%' && make 3dsx"
if errorlevel 1 goto :failed

echo.
echo   Building QuizFabric3DS.cia ...
echo.
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; command -v makerom >/dev/null 2>&1 && command -v bannertool >/dev/null 2>&1"
if errorlevel 1 goto :ciaskipped

"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%' && make cia"
if errorlevel 1 goto :failed

echo.
echo   ------------------------------------------------------------
echo    Built:  QuizFabric3DS.3dsx  and  QuizFabric3DS.cia
echo   ------------------------------------------------------------
echo.
pause
exit /b 0

:ciaskipped
echo.
echo   ------------------------------------------------------------
echo    Built:  QuizFabric3DS.3dsx
echo    Skipped CIA - makerom and/or bannertool are missing.
echo    Run build_cia.bat for the exact download links.
echo   ------------------------------------------------------------
echo.
pause
exit /b 0

:failed
echo.
echo   ************************************************************
echo    BUILD FAILED - see the output above.
echo   ************************************************************
echo.
pause
exit /b 1
