@echo off
setlocal
title QuizFabric 3DS - clean

call "%~dp0_devkit_env.bat"
if errorlevel 1 exit /b 1

echo.
echo   Removing build output ...
"%DKP_BASH%" -l -c "cd '%MSYS_PROJ%' && make clean"

echo   Done.
echo.
pause
exit /b 0
