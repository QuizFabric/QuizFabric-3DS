@echo off
setlocal
title QuizFabric 3DS - build PRODUCTION 3DSX + CIA

call "%~dp0_devkit_env.bat"
if errorlevel 1 exit /b 1

echo.
echo   Building QuizFabric3DSProd.3dsx (PRODUCTION) ...
echo.
REM Wipe the prod object dir so flag changes always take effect.
"%DKP_BASH%" -l -c "cd '%MSYS_PROJ%' && rm -fr build_prod"
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%' && make PROD=1 3dsx"
if errorlevel 1 goto :failed

echo.
echo   Building QuizFabric3DSProd.cia ...
echo.
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; command -v makerom >/dev/null 2>&1 && command -v bannertool >/dev/null 2>&1"
if errorlevel 1 goto :ciaskipped

"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%' && make PROD=1 cia"
if errorlevel 1 goto :failed

echo.
echo   ------------------------------------------------------------
echo    Built:  QuizFabric3DSProd.3dsx  and  QuizFabric3DSProd.cia
echo            PRODUCTION - hardcoded server, own title ID
echo   ------------------------------------------------------------
echo.
pause
exit /b 0

:ciaskipped
echo.
echo   ------------------------------------------------------------
echo    Built:  QuizFabric3DSProd.3dsx
echo    Skipped CIA - makerom and/or bannertool are missing.
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
