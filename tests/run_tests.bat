WG@echo off
setlocal
title QuizFabric 3DS - protocol tests

REM json.c and sio.c have no 3DS dependencies, so the protocol layer can be
REM tested with a plain host gcc. The runner uses devkitPro's MSYS2 bash
REM because MSYS2 gcc needs an MSYS context (POSIX /usr layout, ld next to
REM gcc) and cannot link when invoked from plain cmd.exe.

call "%~dp0..\_devkit_env.bat"
if errorlevel 1 exit /b 1

echo.
echo   Compiling protocol tests ...
"%DKP_BASH%" -l -c "cd '%MSYS_PROJ%' && gcc -std=gnu99 -Wall -Wextra -O1 -o /tmp/qf_protocol_tests.exe tests/test_protocol.c source/json.c source/sio.c"
if errorlevel 1 goto :buildfail

echo.
"%DKP_BASH%" -l -c "/tmp/qf_protocol_tests.exe"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
  echo   All protocol tests passed.
) else (
  echo   SOME TESTS FAILED.
)
pause
exit /b %RC%

:buildfail
echo.
echo   Test build failed.
pause
exit /b 1
