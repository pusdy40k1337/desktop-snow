Desktop Snow - ready-to-run test build
=====================================

For Windows 10 (version 2004 or later) and Windows 11, 64-bit. Nothing to
install: no installer, no Visual C++ Redistributable, no .NET.


Quick start - 5 steps
---------------------
  1. Unblock the zip: right-click the downloaded zip > Properties. If there is
     an "Unblock" checkbox at the bottom, tick it and click OK.
  2. Extract it: right-click the zip > Extract All... > Extract, then open the
     new folder. Do not run anything from inside the zip itself.
  3. Test: double-click TEST.cmd. It asks you to close Desktop Snow if it is
     running. On a laptop, plug in the charger. Press a key, then do not touch
     the mouse or keyboard for about 7 minutes, until the window says Done.
     The screen turns black for about 10 seconds in the middle - that is part
     of the test.
  4. Send back SnowHarness-results.txt from this folder.
  5. Use it: double-click START.cmd. Snow starts falling. To stop it,
     right-click the snowflake icon next to the clock and choose Quit.

  If Windows says "Windows protected your PC": click "More info", then
  "Run anyway". If it says "The publisher could not be verified": click Run.
  The files are not code-signed; step 1 usually prevents these questions.


Files
  START.cmd         double-click to start the app (same as run.cmd)
  TEST.cmd          double-click to run the 7-minute test (checks first, then
                    runs harness.cmd)
  DesktopSnow.exe   the app
  SnowHarness.exe   measurement tool: how closely the snow follows a moving window
  run.cmd           starts DesktopSnow.exe from this folder with a debug panel
  harness.cmd       runs SnowHarness.exe and writes SnowHarness-results.txt
  README.txt        this file


More detail: unpacking
----------------------
  - Extract into a folder you can write to, for example Downloads or Desktop
    (not C:\Program Files). The test writes its results into that folder.
  - On a laptop, plug in the charger: on battery the app caps itself at 30 fps.


More detail: running the app (START.cmd or run.cmd)
---------------------------------------------------
  - Snow starts falling and a small debug panel shows fps, the sync mode and
    the estimated pile error. Drag windows around and let snow pile up on them.
  - To stop: right-click the Desktop Snow icon in the taskbar tray (click the
    small arrow next to the clock if it is hidden) and choose Quit.
  - The log, including one line per minute like
      engine: last 60 s: 3000 frames (50.0 fps, cap 60, display 100.0 Hz),
      900 while windows moved (100.0 fps), ... latency-wait timeouts, ...
    is appended to DesktopSnow-log.txt in this folder. Falling snow alone runs
    at whole fractions of the refresh rate up to the 60 fps cap (50 fps on
    100 Hz); while a window moves, every refresh gets a new frame.
  - Double-clicking DesktopSnow.exe itself starts it normally: no debug panel,
    log in %LOCALAPPDATA%\DesktopSnow\logs.


More detail: the test (TEST.cmd or harness.cmd)
-----------------------------------------------
  - Desktop Snow must not be running, and fullscreen apps and games should be
    closed. TEST.cmd checks for a running Desktop Snow and waits until you
    quit it.
  - First it moves a blue test window across your main monitor at three input
    rates and measures the gap between the window's top edge and the snow
    line. Then a black window covers the whole screen for about 10 seconds,
    like a game or a fullscreen video: the snow must disappear, the app must
    stop drawing, and the snow must come back when it closes. Finally it lets
    snow fall for 3 minutes with nothing moving and records the frame rate
    once a minute.
  - SnowHarness-results.txt contains the sweep measurements, the fullscreen
    test, the per-minute fps and frame-wait timeout lines and the overlay
    logs. The raw data (CSV, screenshots) is in harness-results\<date-time>\
    if it is ever needed.

  Advanced (typed in a Command Prompt opened in this folder):
    Shorter run, about 3 minutes:   harness.cmd --quick
    Only the fullscreen test:       harness.cmd --fullscreen-only   (about 30 s)
    All options:                    SnowHarness.exe --help


Tray menu
---------
  Snow on                      on/off (also Ctrl+Alt+S)
  Pause during fullscreen      on by default: no snow over games, fullscreen
                               video, F11 browsers or slideshows on that monitor
  Snow on the taskbar          the snow line along the top of the taskbar; it
                               always stays behind windows, so it never covers
                               a maximised app's status bar or input field


Where things are stored
-----------------------
  Settings and decorations: %APPDATA%\DesktopSnow
  Logs (when not started through START.cmd or run.cmd):
    %LOCALAPPDATA%\DesktopSnow\logs
  To remove everything: Quit, turn off "Launch at startup" in the tray menu
  first if you turned it on, then delete this folder, %APPDATA%\DesktopSnow
  and %LOCALAPPDATA%\DesktopSnow.
