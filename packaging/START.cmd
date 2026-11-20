@echo off
rem Double-click to start Desktop Snow. Calls run.cmd: small debug panel on screen,
rem log appended to DesktopSnow-log.txt in this folder.
setlocal
cd /d "%~dp0"
set "RC=0"
if not exist "run.cmd" goto :missing
if not exist "DesktopSnow.exe" goto :missing
tasklist /fi "imagename eq DesktopSnow.exe" 2>nul | find /i "DesktopSnow.exe" >nul
if not errorlevel 1 goto :running
call run.cmd %*
exit /b %ERRORLEVEL%

:running
echo Desktop Snow is already running. Look for the snowflake icon next to the clock
echo (click the small arrow next to the clock if you do not see it).
goto :end

:missing
echo Some files are missing next to START.cmd.
echo Extract the whole zip first: right-click the zip, choose "Extract All...",
echo then open the extracted folder and double-click START.cmd there.
set "RC=1"

:end
if /i not "%CI%"=="true" pause
exit /b %RC%
