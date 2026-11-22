# Desktop Snow — *New Year is already here.*

A New Year / Christmas overlay for **Windows 10 (2004+) and Windows 11**. Snow
falls over the whole desktop and **settles on the top edge of every window and
the taskbar**, follows windows while you drag them, and flies off when you fling
a window. Grow Christmas trees, build snowmen that ride on your windows, and
get visits from Santa, a gift train and a snowman-hunting yeti.

Native C++20 / Win32 / Direct3D 11 / DirectComposition / Direct2D. One
self-contained exe, no installer, no runtime dependencies.

## Download

[**Latest release**](../../releases/latest):

- **DesktopSnow.exe** — a single self-contained exe. Download and run.
- **DesktopSnow-portable.zip** — exe + `START.cmd` / `TEST.cmd` launchers + README.

Works on Windows 10 (2004+) and Windows 11, x64.

## Install & run

**No toolchain needed:** every CI run on `windows-latest` publishes a
ready-to-run zip. Open the PR's or branch's latest *windows* run (Actions tab)
and download the artifact `DesktopSnow-win64-<commit>`. It contains
`DesktopSnow.exe`, `SnowHarness.exe`, the double-click launchers `START.cmd`
and `TEST.cmd` (wrappers around `run.cmd` and `harness.cmd`, also included) and a
`README.txt` with the exact steps. The exes link the C++ runtime statically
(`/MT`), so they run on a clean Windows without a Visual C++ redistributable.
CI checks the exes' imported DLLs on every run.

1. Build it (below) or use the zip above.
2. Run `DesktopSnow.exe`. A snowflake appears in the tray (it may be in the
   `^` overflow; drag it onto the taskbar to keep it visible).
3. Everything is in the tray menu: on/off, intensity, effect, theme, quick
   settings, *Decorate…*, *Surprise me*, language, launch at startup, quit.

| Hotkey | Action |
|---|---|
| `Ctrl+Alt+S` | snow on/off |
| `Ctrl+Alt+D` | decorate mode (trees, ornaments, snowmen); `Esc` / right-click to leave |

State lives in `%APPDATA%\DesktopSnow\state.json` (versioned, saved atomically,
with `state.json.bak` as the last good copy). Logs: `%LOCALAPPDATA%\DesktopSnow\logs`.
Settings from the old Electron version are imported on first run.

## Build (Windows)

Requirements: Visual Studio 2019/2022/2026 or the free *Build Tools* with the
**Desktop development with C++** workload (CMake comes with it):

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"
```

| Command | Result |
|---|---|
| `build.cmd` | Release build + unit tests → `dist\DesktopSnow.exe`, `dist\SnowHarness.exe` |
| `build.cmd -Config Debug` | Debug build → `dist-debug\` |
| `run.cmd` | Debug build and run with the debug overlay, a log console and telemetry (`dist-debug\telemetry.csv`); separate "dev" instance |
| `harness.cmd` | drag-sync measurement (see below) |

From Linux/macOS you can only compile-check:
`cmake -S . -B build/mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake && cmake --build build/mingw`
and run the portable unit tests: `cmake -S . -B build/host && cmake --build build/host && build/host/bin/snow_tests`.

CI (`.github/workflows/windows.yml`) builds the PR head commit with MSVC on
`windows-latest` and runs the unit tests. It checks that the exes import no
Visual C++ runtime DLLs and uploads the ready-to-run zip. Then it unzips that
zip into a fresh folder and runs the packaged `START.cmd` and `TEST.cmd` (which
call `run.cmd` and `harness.cmd`) from there. The packaged scripts live in
`packaging/`.

## How the window tracking works

- **Geometry**: `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` (visible frame,
  no invisible resize borders), per-monitor-v2 DPI aware, physical pixels everywhere.
  Windows 11 rounded corners (8 px × scale; `DWMWA_WINDOW_CORNER_PREFERENCE`
  respected) — the pile drapes over the corner curve.
- **Events, not polling**: `SetWinEventHook` (location change, move/size start/end,
  create/destroy/show/hide, cloak/uncloak, foreground, minimise) on a dedicated
  thread; a 1 s safety re-scan catches z-order changes that have no event.
- **Per frame**: windows that moved in the last 350 ms (or are in a move/size loop)
  are re-read once per frame.
- **DWM sync** (“late latch”): the renderer reads DirectComposition's frame
  statistics, sleeps until *next composition − measured render cost − margin*,
  samples the window, renders and presents, so the window move and the pile
  update land in the same DWM frame. A smoothed-velocity predictor extrapolates
  over the remaining 2–4 ms; it switches off instantly when the window stops,
  so a still window never jitters. `--sync vsync` and `--predict off` exist for
  comparison.
- **Visibility**: piles are clipped by windows above (z-order), hidden for
  minimised/cloaked/hidden windows; tool windows, the desktop shell, UWP ghost
  frames, other virtual desktops and the overlay itself are skipped.
- **Overlay windows**: one per monitor, `WS_EX_LAYERED | WS_EX_TRANSPARENT |
  WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP`
  with a premultiplied-alpha composition swap chain. Clicks pass through; never
  activated; not in Alt+Tab/taskbar.
- **Fullscreen pause** (tray: *Pause during fullscreen*, on by default): per
  monitor, the first relevant window in z-order is fullscreen when its DWM frame
  bounds cover the monitor (a captioned maximised window is not; always-on-top
  and floating windows such as picture-in-picture or player controls are looked
  through; Alt+Tab, Start, taskbars and click-through overlays are ignored).
  `SHQueryUserNotificationState` adds exclusive D3D and presentation mode.
  Checked on WinEvents (foreground, foreground moved/resized, windows shown,
  hidden, closed) plus a 500 ms fallback timer, debounced 300 ms. That monitor's
  overlay is hidden, spawns no snow and its flakes freeze; with every monitor
  paused the renderer sleeps. The snow fades back in over 0.6 s.
- **Taskbar snow** (tray: *Snow on the taskbar*) is drawn at desktop level: any
  window in front, e.g. a maximised app's status bar, covers it.
- **Frame rate**: while any window moves, every DWM composition gets a new
  frame; idle snowfall runs at whole fractions of the refresh rate up to the
  60 fps cap (50 fps at 100 Hz).

## Measuring sync (`harness.cmd`)

`SnowHarness.exe` starts the overlay in measurement mode, puts a black backdrop
and a blue test window on the primary monitor, moves the window with
`SetWindowPos` along sweeps (still, slow, fast, sine, sudden stop, vertical) at
60/144/1000 Hz input rates, captures every composed frame with Desktop
Duplication (GDI fallback) and measures, in pixels, where the overlay's outline
is versus the real window edge. It also checks the visible frame against
`GetWindowRect` vs `EXTENDED_FRAME_BOUNDS` and that clicks pass through.
By default it measures the app's default mode (`latch-step`) against the
naive baseline (`vsync-nopredict`). It then runs plain snowfall with nothing
moving for 3 minutes (`--snowfall <sec>`, 0 skips it), where the overlay logs
fps and frame-wait timeouts once a minute.
Everything printed, plus the overlay logs, goes to `SnowHarness-results.txt`
next to `SnowHarness.exe`: that one file is what to send back. Raw data:
`harness-results\<time>\summary.md`, `frames.csv`. See
[docs/SYNC.md](docs/SYNC.md) for the measured numbers and the physical limits.

## Measured results (GitHub `windows-latest`, real Windows in a VM)

From `SnowHarness` and the CI resource check, 1024×768 at 64 Hz, no GPU
(software rendering). Details and raw tables: [docs/SYNC.md](docs/SYNC.md).

| | naive overlay (vsync, no prediction) | this app (late latch + stepped prediction) |
|---|---|---|
| still window | 0 px, no jitter | 0 px, no jitter |
| slow drag (150 px/s) | 1.9–2.3 px mean, 6–22% of frames ≤ 1 px | 0.1–0.4 px mean, 92–97% ≤ 1 px |
| fast drag (1800 px/s) | 22–24 px mean (about one frame behind) | 1.7–3.3 px mean, 68–91% ≤ 1 px; p95 7–30 px |
| vertical drift | none | none: start-to-end change of 0.1–1.4 px in either direction, no trend |
| frame bounds | `EXTENDED_FRAME_BOUNDS` == visible edge (PASS) | |
| click-through | PASS (click reached the window below in 29–48 ms), never foreground | |
| CPU / memory | 3-minute snowfall check: 3.6% of one core at 45 fps (display 64 Hz), handles ~260, private memory ~9 MB, clean shutdown. 25-minute soak on 8c9b49c (this pacing code) with windows opening and closing: 3.6% of one core, handles 257–264, GDI 7, USER 18–20, private memory 10.8–13.0 MB (levels off), 44.6–46.7 fps, clean exit; 18–29 frame-latency waits per minute timed out, a VM effect (0 on the real PC below). All on a 2-core VM with software rendering (no GPU). | |

The acceptance target "≤ 1 px during drags" is met for most moving frames but
**not at p95 for fast drags**, and single CI runs vary by several px at p95;
docs/SYNC.md has all three runs and explains why (input steps landing inside the
~1.7 ms sample-to-composition gap, and missed compositions on the VM) and why a
separate overlay window cannot fully remove it.

**First real-PC run** (build 598fe4c, Windows 10, RX 580, 1920×1080 at 100 Hz):
still windows 0 px, slow drags 0.8–1.0 px mean, fast drags 10–11 px mean and
p95 21–31 px. The cause: the overlay drew at 50 fps the whole time, drags
included, because the 60 fps cap applied while windows moved, i.e. a new frame
only every second refresh. Fixed in 1baa4c8 (full rate while anything moves);
not re-measured on that PC yet. Details in docs/SYNC.md.

`SnowHarness` also runs a **fake fullscreen app** test: a borderless window
covers the test monitor; the overlay must hide, present nothing, drop its CPU
use and come back with snow after the window closes. CI fails if it doesn't.
CI also renders every decoration with Direct2D into PNGs (artifact
`decor-render-<commit>`, `DesktopSnow.exe --render-decor <dir>`).

## Project layout

```
src/core/      portable: pile model, motion/scatter, world, visitors, settings, JSON, themes, strings
src/app/       Windows: tracker, render engine, tray/app, Direct2D artwork, platform helpers
src/harness/   SnowHarness.exe (drag-sync measurements)
tests/         unit tests (pile, scatter, prediction, settings persistence, world, harness detection)
docs/          AUDIT.md, SYNC.md, TEST-CHECKLIST.md
assets/        optional PNG overrides (see assets/README.md)
reference/macos-electron/   the original Electron app, frozen for macOS comparison
```

Effects are pluggable through the simulation settings (`effect`: `snow`,
`confetti`, `none`); snow is the flagship.

## Known limitations

- The overlay is a separate window, so perfect sync is bounded by how the dragged
  window moves (mouse event rate) and by the DWM deadline; see docs/SYNC.md.
- Rounded-corner radius is inferred (Windows does not report the actual radius);
  override with `piles.cornerRadius` in `state.json` if a theme/app differs.
- Exclusive-fullscreen games, the UAC secure desktop and the lock screen are never
  drawn over (by design). Windows on top of the overlay (other always-on-top apps,
  Start menu) hide the snow under them.
- The `Assets` folder mentioned in the brief is not in the repository yet; all
  artwork is procedural until it is added (assets/README.md).
- macOS is not a target; the original Electron app remains in `reference/`.
