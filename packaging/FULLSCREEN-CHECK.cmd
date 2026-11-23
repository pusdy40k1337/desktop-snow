@echo off
rem Double-click to record 90 seconds of pause decisions (fullscreen apps,
rem maximized windows). Writes FULLSCREEN-CHECK-log.txt next to this file.
rem Extra arguments go to DesktopSnow.exe (CI passes a shorter --quit-after).
setlocal
cd /d "%~dp0"
set "LOG=%~dp0FULLSCREEN-CHECK-log.txt"
if not exist "DesktopSnow.exe" goto :missing
if /i "%CI%"=="true" goto :go

:check
tasklist /fi "imagename eq DesktopSnow.exe" 2>nul | find /i "DesktopSnow.exe" >nul
if errorlevel 1 goto :ready
echo Desktop Snow is running and must be closed for this check:
echo   right-click the snowflake icon next to the clock and choose Quit
echo   (click the small arrow next to the clock if you do not see the icon).
echo When it is closed, press any key here.
pause >nul
echo.
goto :check

:ready
echo Fullscreen check - takes 90 seconds
echo.
echo After you press a key, Desktop Snow starts with a small info panel.
echo Then, within 90 seconds, do as many of these as you can:
echo   1. Open YouTube, play a video, click its fullscreen button. Wait 5 seconds, press Esc.
echo   2. In the browser press F11. Wait 5 seconds, press F11 again.
echo   3. Maximize the browser with its square button. Wait 5 seconds, then restore it.
echo   4. If you have a game: start it, wait 5 seconds, then Alt+Tab out of it.
echo Desktop Snow closes by itself after 90 seconds.
echo.
echo Press any key to start.
pause >nul

:go
if exist "%LOG%" del "%LOG%"
echo Recording... Desktop Snow closes by itself after 90 seconds.
start "" /wait "%~dp0DesktopSnow.exe" --diagnostics --no-persist --quit-after 90 --log "%LOG%" %*
echo.
if exist "%LOG%" (
  echo Done. Send this file back: "%LOG%"
) else (
  echo The log was not written - send a screenshot of this window instead.
)
if /i not "%CI%"=="true" pause
exit /b 0

:missing
echo DesktopSnow.exe must be in the same folder as FULLSCREEN-CHECK.cmd.
echo Extract the whole zip first: right-click the zip, choose "Extract All...",
echo then open the extracted folder and double-click FULLSCREEN-CHECK.cmd there.
if /i not "%CI%"=="true" pause
exit /b 1
