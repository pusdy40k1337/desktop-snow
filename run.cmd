@echo off
rem Debug build + run with the debug overlay and a log console.
rem Uses a separate "dev" instance (own state file, can run next to a normal copy).
rem Extra arguments are passed through, e.g.:  run.cmd --sync vsync --predict off
setlocal
call "%~dp0build.cmd" -Config Debug -NoTests || exit /b 1
"%~dp0dist-debug\DesktopSnow.exe" --debug --console --instance dev --telemetry "%~dp0dist-debug\telemetry.csv" %*
