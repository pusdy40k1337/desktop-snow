# Phase 0 – Audit of the original (DeepSeek) code

Audited revision: the repository's single commit (Electron app, now frozen in
`reference/macos-electron/`). Nothing here was assumed to work; every point
below cites the code.

## What it is

| | |
|---|---|
| Stack | Electron 33 + TypeScript + PixiJS 8, esbuild; a Swift helper (`scripts/windowtracker.swift`) for macOS window tracking |
| Architecture | Main process (tray, settings, overlays, tracker) → IPC → one transparent `BrowserWindow` per display running a PixiJS "effect plugin" (`src/effects/snow`) |
| Target platform in practice | **macOS only.** README, paths (`~/Library/...`), Dock, Screen Recording/Accessibility permissions, `swiftc`, `sips`, `iconutil` |
| Tests | none |
| Assets | none in the repository (no `Assets` folder in the tree or in git history) |

## What is actually implemented vs. fake (on Windows)

| Feature | Status on Windows | Evidence |
|---|---|---|
| Falling snow (3 parallax layers, wind, sway, colour/size/density) | **Works** (GPU via PixiJS) | `src/effects/snow/index.ts` `rebuild()` / `update()` |
| Snow settling on windows | **Fake**: the Windows "tracker" is a stub that always reports zero windows | `main.ts` `startFallbackTracker()` sends `windows: []` every 250 ms |
| Snow on the taskbar | Partial: estimated from the primary display's work area only; wrong for top/side taskbars and other monitors | `main.ts` 260–282 |
| Fullscreen pause | **Never triggers** on Windows (stub reports `fullscreen: false`) | `main.ts` 268 |
| Battery saving ("half the particles, fewer frames") | **Fake**: `quality` is only read in `rebuild()`, which never runs on a battery change; frame rate never changes | `index.ts` 245–250, 650–653; renderer `onWindows` |
| Tray icon | **Invisible on Windows**: the icon file is produced only by the macOS script; on Windows `nativeImage.createEmpty()` + `setTitle` (macOS-only) | `main.ts` 378–387, `scripts/make-app.sh` |
| Windows build (`npm run win`) | **Broken**: `build/icon_1024.png` is generated only by the macOS script, which deletes `build/` at the end | `package.json` `build.win.icon`, `make-app.sh` 70 |
| Settings persistence | Works but fragile (see bugs) | `src/main/settings.ts` |
| Pile state persistence | Not implemented (`restore()` is a no-op) | `index.ts` 817–819 |

## Can the stack meet the requirements?

**No.** The requirement that matters most is a pile that stays within ~1 px of
a window's top edge while it is dragged at 60/144 Hz. In the Electron design:

1. Window positions are polled (20 Hz on macOS, nothing on Windows), sent over
   IPC to a renderer process, and the pile is *lerped* toward the target with a
   40 ms-capped extrapolation (`index.ts` 663–671). That is lag and overshoot by
   design.
2. Chromium's compositor adds 1–3 frames of queueing between `requestAnimationFrame`
   and DWM, and the app cannot control when the frame is latched.
3. Transparent, click-through Electron windows on Windows are expensive and the
   frame rate is capped at 20–60 fps by a ticker.

A Windows-native implementation can read the dragged window's rectangle once per
composition, sample it *just before* the DWM deadline and present in the same
composition, which is the only way to get close to frame-accurate sync.

**Decision:** rewrite natively in **C++20 + Win32 + Direct3D 11 + DirectComposition
+ Direct2D** (no runtime dependencies, single static exe, small memory footprint).
The simulation core (pile model, scatter detector, prediction, settings) is
portable C++ so it is unit-tested on every platform. The Electron app is kept
untouched in `reference/macos-electron/` for macOS comparison; no Windows code
depends on it.

Alternatives considered: C#/WPF or WinUI (composition is not under our control,
same latency problem); Rust + `windows` crate (equivalent capability, heavier
toolchain for you to install); Electron + native addon (still has the Chromium
compositor queue).

## Bug list (original code)

Severity: **C** critical, **M** major, **m** minor.

| # | Sev | Bug | Where |
|---|---|---|---|
| 1 | C | No window tracking on Windows at all: snow never settles on windows | `main.ts` `startFallbackTracker` |
| 2 | C | Tray icon invisible on Windows → no way to pause/quit except Task Manager | `main.ts` `createTray`, icon only built by macOS script |
| 3 | C | `npm run win` cannot produce a correct build (icon path generated and then deleted by the macOS-only script) | `package.json`, `make-app.sh` |
| 4 | C | Pile follows windows by lerp + clamped extrapolation from 20 Hz samples: visible lag, overshoot after stops, no compositor sync | `index.ts` 350–363, 660–671 |
| 5 | M | Fullscreen detection never fires on Windows; the overlay sits over games at the `screen-saver` z-level | `main.ts` 68, 243, 268 |
| 6 | M | Battery mode is fake (quality never re-applied; fps unchanged) | `index.ts` 245–250, renderer 64–67 |
| 7 | M | Resizing a window by more than 8 px wipes its pile (arrays reallocated, not resampled) | `index.ts` 376–382 |
| 8 | M | Columns hidden behind a window in front are permanently zeroed, so snow is deleted, not hidden | `index.ts` 404–408 |
| 9 | M | Velocity from two samples, timed with the *renderer* clock, no smoothing and no hysteresis: the scatter trigger flickers and fires on jitter | `index.ts` 354–374 |
| 10 | M | Settings written synchronously and non-atomically on every slider `input` event; a crash mid-write corrupts the file, and a corrupt file silently resets everything (`catch {}`) | `settings.ts` 27–48 |
| 11 | M | `fs.appendFileSync('/tmp/snow-render.log')` on every renderer console message and in the tracker path (`/tmp` does not exist on Windows; the tracker call is not wrapped in try/catch) | `main.ts` 79, 249 |
| 12 | M | Continuous rendering at 20–60 fps even when nothing changes (`backgroundThrottling: false`), with fixed 1.5× resolution regardless of DPI | `main.ts` 62, `renderer.ts` 16 |
| 13 | m | `rect.y < 40` ("under the macOS menu bar") skips any window near the top of any display | `index.ts` 333 |
| 14 | m | Magic `DOCK_MOUND_SHIFT = -10` "hand-tuned by the user" | `index.ts` 432 |
| 15 | m | Rebuilding overlays on display add/remove throws away all snow | `main.ts` 101–112 |
| 16 | m | "Pause" is persisted but deliberately never restored | `settings.ts` 33–34 |
| 17 | m | `restore()` is a no-op; nothing besides settings persists | `index.ts` 817 |
| 18 | m | Generated `.effects.generated.ts` is committed | `src/renderer/` |
| 19 | m | 9 npm audit findings (2 high) in the dependency tree | `npm audit` during setup |

All of these are addressed by the rewrite (the new code has none of these
paths). The frozen reference is intentionally left unchanged so it can still be
compared on a Mac.
