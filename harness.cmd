@echo off
rem Runs the overlay alignment harness; all arguments are passed through (see --help).
setlocal
set "HARNESS=%~dp0dist\SnowHarness.exe"
if not exist "%HARNESS%" (
  echo dist\SnowHarness.exe not found - run build.cmd first.
  exit /b 1
)
"%HARNESS%" %*
exit /b %ERRORLEVEL%
