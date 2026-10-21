@echo off
setlocal
title QuizFabric 3DS - toolchain check

call "%~dp0_devkit_env.bat"
if errorlevel 1 exit /b 1

echo.
echo   devkitPro root : %DKP_ROOT%
echo   project (msys) : %MSYS_PROJ%
echo.
echo   Checking tools ...
echo.

REM The compiler is NOT on PATH with current devkitPro installs: devkit-env.sh
REM only exports tools/bin. The Makefile reaches it via $(DEVKITARM)/bin, so
REM check the actual file instead of `command -v`.
REM NOTE: keep `2>` redirections OUT of $(...) below - cmd.exe's parser mangles
REM the line if a redirect sits inside a command substitution in a quoted string.
"%DKP_BASH%" -l -c "export PATH=\"$PATH:%DKP_PATH_EXTRA%:%DKP_TOOLS_MSYS%\"; for t in arm-none-eabi-gcc make 3dsxtool smdhtool makerom bannertool; do p=\"$(command -v $t)\"; if [ -z \"$p\" ] && [ \"$t\" = arm-none-eabi-gcc ] && [ -x \"$DEVKITARM/bin/$t\" ]; then p=\"$DEVKITARM/bin/$t (via DEVKITARM)\"; fi; if [ -n \"$p\" ]; then printf '    [ok]      %%-18s %%s\n' \"$t\" \"$p\"; else printf '    [MISSING] %%s\n' \"$t\"; fi; done"

echo.
echo   Checking libraries ...
echo.
"%DKP_BASH%" -l -c "for f in $DEVKITPRO/libctru/lib/libctru.a $DEVKITPRO/libctru/lib/libcitro3d.a $DEVKITPRO/libctru/lib/libcitro2d.a; do if [ -f \"$f\" ]; then printf '    [ok]      %%s\n' \"$f\"; else printf '    [MISSING] %%s\n' \"$f\"; fi; done"

echo.
echo   Notes:
echo     arm-none-eabi-gcc / make / 3dsxtool / smdhtool  -^> needed for .3dsx
echo     makerom + bannertool                            -^> also needed for .cia
echo     bannertool is not shipped by devkitPro; put bannertool.exe in .\tools\
echo.
pause
exit /b 0
