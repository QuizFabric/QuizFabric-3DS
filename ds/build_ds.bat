@echo off
setlocal
title QuizFabric DS - build DEV

call "%~dp0..\_devkit_env.bat"
if errorlevel 1 exit /b 1

"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; cd '%MSYS_PROJ%/ds' && make"
if errorlevel 1 goto :failed

echo.
echo   Built: ds\QuizFabricDS.nds (dev)
echo.
pause
exit /b 0

:failed
echo BUILD FAILED - see the output above.
pause
exit /b 1
