@echo off
setlocal
title QuizFabric DS - build PRODUCTION

call "%~dp0..\_devkit_env.bat"
if errorlevel 1 exit /b 1

"%DKP_BASH%" -l -c "cd '%MSYS_PROJ%/ds' && rm -fr build_prod"
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%/ds' && make PROD=1"
if errorlevel 1 goto :failed

echo.
echo   Built: ds\QuizFabricDSProd.nds (PRODUCTION - fixed server)
echo.
pause
exit /b 0

:failed
echo BUILD FAILED - see the output above.
pause
exit /b 1
