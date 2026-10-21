@echo off
setlocal
title QuizFabric 3DS - build 3DSX

call "%~dp0_devkit_env.bat"
if errorlevel 1 exit /b 1

echo.
echo   Building QuizFabric3DS.3dsx ...
echo.

"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%\"; cd '%MSYS_PROJ%' && make 3dsx"
if errorlevel 1 goto :failed

echo.
echo   ------------------------------------------------------------
echo    Built:  QuizFabric3DS.3dsx   (plus QuizFabric3DS.smdh)
echo   ------------------------------------------------------------
echo.
echo    Copy QuizFabric3DS.3dsx to your SD card under /3ds/
echo    and launch it from the Homebrew Launcher.
echo.
pause
exit /b 0

:failed
echo.
echo   ************************************************************
echo    BUILD FAILED - see the compiler output above.
echo   ************************************************************
echo.
pause
exit /b 1
