@echo off
rem Starts Desktop Snow from this folder with the on-screen debug panel (fps, sync
rem mode, estimated pile error). The log, with one fps line per minute, is appended
rem to DesktopSnow-log.txt next to this file. Extra arguments go to DesktopSnow.exe.
setlocal
if not exist "%~dp0DesktopSnow.exe" goto :missing
start "" "%~dp0DesktopSnow.exe" --debug --log "%~dp0DesktopSnow-log.txt" %*
echo Desktop Snow is running. To stop it, right-click its tray icon and choose Quit.
echo Log: "%~dp0DesktopSnow-log.txt"
if /i not "%CI%"=="true" timeout /t 10 >nul
exit /b 0

:missing
echo DesktopSnow.exe must be in the same folder as run.cmd. Extract the whole zip first.
if /i not "%CI%"=="true" pause
exit /b 1
