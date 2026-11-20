@echo off
rem Double-click to run the snow test (about 7 minutes). Calls harness.cmd, which
rem writes SnowHarness-results.txt in this folder. Extra arguments go to harness.cmd.
setlocal
cd /d "%~dp0"
if not exist "harness.cmd" goto :missing
if not exist "SnowHarness.exe" goto :missing
if not exist "DesktopSnow.exe" goto :missing
if /i "%CI%"=="true" goto :go

:check
tasklist /fi "imagename eq DesktopSnow.exe" 2>nul | find /i "DesktopSnow.exe" >nul
if errorlevel 1 goto :ready
echo Desktop Snow is running and must be closed for the test:
echo   right-click the snowflake icon next to the clock and choose Quit
echo   (click the small arrow next to the clock if you do not see the icon).
echo When it is closed, press any key here.
pause >nul
echo.
goto :check

:ready
echo Desktop Snow test - takes about 7 minutes
echo.
echo   - Laptop: plug in the charger now.
echo   - Close games and fullscreen videos.
echo   - After you press a key, do not touch the mouse or keyboard until it says Done.
echo   - A blue test window moves around, then the screen turns black for about
echo     10 seconds. That is part of the test.
echo.
echo Press any key to start, or close this window to cancel.
pause >nul
echo.

:go
call harness.cmd %*
exit /b %ERRORLEVEL%

:missing
echo Some files are missing next to TEST.cmd.
echo Extract the whole zip first: right-click the zip, choose "Extract All...",
echo then open the extracted folder and double-click TEST.cmd there.
if /i not "%CI%"=="true" pause
exit /b 1
