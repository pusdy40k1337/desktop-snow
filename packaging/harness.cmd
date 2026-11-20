@echo off
rem Measures how closely the snow follows a moving window, then lets snow fall for
rem 3 minutes and records the frame rate. Writes SnowHarness-results.txt next to
rem this file. Extra arguments go to SnowHarness.exe, e.g.  harness.cmd --quick
setlocal
cd /d "%~dp0"
set "RC=1"
if not exist "SnowHarness.exe" goto :missing
if not exist "DesktopSnow.exe" goto :missing
tasklist /fi "imagename eq DesktopSnow.exe" 2>nul | find /i "DesktopSnow.exe" >nul
if not errorlevel 1 goto :running

echo SnowHarness takes about 7 minutes, or about 3 with --quick.
echo Do not touch the mouse or keyboard until it says Done.
echo It moves a blue test window around the main monitor, then covers the screen with a
echo black "fullscreen app" for a few seconds, then lets snow fall for 3 minutes.
echo.
SnowHarness.exe %*
set "RC=%ERRORLEVEL%"
echo.
if exist "SnowHarness-results.txt" (
  echo Done. Send this file back: "%~dp0SnowHarness-results.txt"
) else (
  echo SnowHarness did not write SnowHarness-results.txt - send a screenshot of this window instead.
)
goto :end

:missing
echo SnowHarness.exe and DesktopSnow.exe must be in the same folder as harness.cmd.
echo Extract the whole zip first, then run harness.cmd from the extracted folder.
goto :end

:running
echo Desktop Snow is already running. Quit it first: right-click its tray icon and choose Quit.
echo Then run harness.cmd again.

:end
if /i not "%CI%"=="true" pause
exit /b %RC%
