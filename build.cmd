@echo off
rem One-command build: dist\DesktopSnow.exe (+ SnowHarness.exe), runs unit tests.
rem Options: -Config Debug   -Clean   -NoTests
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" %*
exit /b %ERRORLEVEL%
