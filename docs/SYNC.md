# Window/pile synchronisation: design, measurements, limits

## Pipeline (per frame)

1. **Pace.** Read DirectComposition frame statistics (`nextEstimatedFrameTime`,
   composition rate; fallback `DwmGetCompositionTimingInfo`). Wait until the swap
   chains can accept a frame (DXGI frame-latency waitable, max latency 1). Every
   present's release is counted: a release that turns up after a timed-out wait
   is drained instead of letting the next wait return a frame early, and an
   overlay DWM stops taking frames from (e.g. a display that is off) is skipped
   and retried after 0.5 s, so it cannot stall the other monitors.
2. **Late latch** (default while any window is moving): sleep until
   `next composition − lead`, where `lead = p90(render cost of the last 32 frames)
   + 1.5 ms margin`. A high-resolution waitable timer plus a ≤0.6 ms spin gives
   sub-millisecond wake-up; the spin only happens while a window is moving.
3. **Sample.** For every window that moved in the last 350 ms or is in a move/size
   loop, read `DWMWA_EXTENDED_FRAME_BOUNDS` again (a few µs per window).
4. **Predict.** Extrapolate to the composition time:
   - *stepped* (default for input below ~400 Hz): windows are moved by mouse
     events in periodic steps, so only whole steps that are due before the
     composition are added (step phase from WinEvent timestamps, period from
     the event interval);
   - *linear* for fine-grained input (≥ 400 Hz), where event-time jitter is
     comparable to the step;
   - **no prediction at all when the window is still** (stop detected within
     ~1.6 input intervals), so a still window never jitters.
5. **Render + Present(0)** before DWM latches, so the window move and the pile
   update appear in the same composed frame.

`--sync vsync` (classic: sample right after the previous composition) and
`--predict off` / `--predictor linear|stepped` / `--latch-margin <ms>` exist for
comparison; the debug overlay shows the live values.

Every frame is presented with sync interval 0. DWM composes the swap chain, so
this cannot tear, and the pacing above decides when frames happen. Builds up
to f43a211 used sync interval 1 outside late latch. There, the frame-latency
release arrived one refresh after the frame's target, so the waits ran ordinary
snowfall and the vsync baseline at **half the refresh rate**: 31.25 ms between
frames at 64 Hz in harness telemetry, against 15.6 ms for late latch.

## How it is measured

`SnowHarness.exe` (see README) captures the composed desktop with Desktop
Duplication and finds, in every frame, the real top/left edge of a blue test
window and the overlay's 1-px outline drawn with exactly the transform used for
piles. Offset 0 = pixel-perfect. The window is moved with `SetWindowPos` from a
thread that steps at a fixed *input rate* (60/144/1000 Hz ≈ cheap mouse / gaming
mouse / 1000 Hz mouse); sweeps: still 2 s, slow 150 px/s, sine, fast 1800 px/s,
sudden stop after 1500 px/s, vertical 900 px/s.

## Measured on GitHub `windows-latest` (real Windows, VM)

Environment reported by the harness: Windows 10.0.26100 (Server 2025), 1024×768,
**64 Hz**, 100% scale, per-monitor-v2, no GPU (WARP + basic display adapter),
Desktop Duplication capture, `--quick` durations.

Cells: **mean / p95 |offset| in px (share of frames within 1 px)**, moving
sweeps use the axis of motion. Three runs of the current late-latch code:
A = run 44614657702 (commit c96b328), B = run 44616097652 (f43a211, which only
adds logging) and C = run 44618866123 (62f2c12, sync interval 0 everywhere).
Late latch already presented with interval 0, so A–C measure the same path.
The baseline is from run C, the first at full frame rate. Earlier baselines ran
at half rate and lagged 37–40 px on the fast sweep, which overstated the gain.

| mode | input Hz | run | still | slow | sine | fast | stop | vertical |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| baseline: vsync, no prediction | 60 | C | 0/0 (100%) | 2.3/5 (13%) | 5.9/12 (28%) | 23.8/31 (20%) | 7.1/25 (67%) | 13.0/15 (13%) |
| baseline: vsync, no prediction | 144 | C | 0/0 (100%) | 1.9/3 (22%) | 6.6/10 (10%) | 22.4/38 (16%) | 2.2/21 (90%) | 13.2/19 (6%) |
| baseline: vsync, no prediction | 1000 | C | 0/0 (100%) | 2.1/3 (6%) | 6.6/11 (11%) | 24.0/29 (5%) | 6.4/23 (65%) | 12.2/15 (8%) |
| **late latch + prediction (default)** | 60 | A | 0/0 (100%) | 0.2/2 (92%) | 0.8/9 (90%) | 3.1/30 (84%) | 0.3/3 (94%) | 0.9/4 (87%) |
| **late latch + prediction (default)** | 60 | B | 0/0 (100%) | 0.2/3 (93%) | 0.6/3 (93%) | 3.0/29 (87%) | 1.4/22 (93%) | 1.2/7 (86%) |
| **late latch + prediction (default)** | 60 | C | 0/0 (100%) | 0.1/2 (95%) | 0.5/3 (93%) | 2.2/30 (91%) | 1.0/8 (90%) | 1.7/15 (84%) |
| **late latch + prediction (default)** | 144 | A | 0/0 (100%) | 0.2/1 (96%) | 1.1/5 (79%) | 1.7/13 (89%) | 0.6/2 (94%) | 1.5/10 (80%) |
| **late latch + prediction (default)** | 144 | B | 0/0 (100%) | 0.2/1 (96%) | 0.6/5 (92%) | 3.1/25 (84%) | 0.6/4 (93%) | 1.6/13 (85%) |
| **late latch + prediction (default)** | 144 | C | 0/0 (100%) | 0.4/2 (95%) | 1.4/9 (79%) | 2.9/24 (81%) | 0.4/2 (94%) | 1.2/12 (90%) |
| **late latch + prediction (default)** | 1000 | A | 0/0 (100%) | 0.3/2 (94%) | 0.4/1 (98%) | 2.4/7 (75%) | 0.8/2 (93%) | 1.2/4 (87%) |
| **late latch + prediction (default)** | 1000 | B | 0/0 (100%) | 0.2/1 (97%) | 0.6/2 (94%) | 2.5/7 (73%) | 0.2/1 (97%) | 1.4/3 (80%) |
| **late latch + prediction (default)** | 1000 | C | 0/0 (100%) | 0.3/1 (96%) | 0.6/1 (97%) | 3.3/27 (68%) | 0.6/2 (94%) | 1.4/5 (81%) |

The default uses the stepped predictor below ~400 Hz input and linear above it
(stepped measured worse at 1000 Hz). A 0.5 ms or 1.0 ms latch margin produced
more misses than 1.5 ms in earlier runs, so 1.5 ms stays the default.

**Run-to-run variance on the VM is large.** An earlier run (44601617366, commit
916adc2) measured fast-sweep p95 of 10 px at 60 Hz and 7 px at 144 Hz with
90–91% of frames within 1 px. Four later runs with the same 60/144 Hz code path
gave p95 13–38 px. Treat any single row as ± a few px at p95. The pacing fix
in c96b328 had a clear effect at 1000 Hz: the run just before it (44613394215)
had 53% of fast-sweep frames, 64% of sine and 48% of vertical within 1 px,
against 68–98% after. However, that earlier run's input thread also ran
slower (742–968 Hz instead of ~995 Hz).

Other harness checks, every run: **extended frame bounds == visible frame (PASS)**;
`GetWindowRect` is larger by the invisible resize borders; **click-through PASS**
(the click reached the window under the overlay in 29–48 ms); the overlay never
became the foreground window; **still windows: 0 px offset, one distinct position,
no jitter, in every mode**.

### What the numbers mean

- The naive approach (sample right after the previous composition, draw, no
  prediction) lags **about one frame**: 22–24 px mean at 1800 px/s and 64 Hz
  (one frame ≈ 28 px). That is the "snow floats behind the window" effect.
- Late latch + prediction removes most of that lag: mean error 0.1–3.3 px,
  with 68–98% of moving frames within 1 px (so the median is ≤ 1 px in every
  sweep).
- **No cumulative vertical drift**: the harness compares the mean signed vertical
  offset at the start and end of the sweep. It changed by 0.1–1.4 px in either
  direction, with no consistent trend, because positions are re-read every frame
  and never integrated. That still exceeds the harness's strict 0.5 px drift
  check in some rows, and longer sweeps (non-`--quick`) have not been run in CI.
- The **p95 is not ≤ 1 px** for fast motion (7–30 px across the three runs). The
  remaining 2–32% of moving frames are off by whole input steps or a whole
  frame. Two causes:
  1. **Input quantisation (physical).** The window only moves when its owner
     processes an input event. If an event lands between our sample and the
     moment DWM latches the frame (the ~1.7 ms lead), the window jumps and the
     overlay cannot know. At 1800 px/s a 144 Hz step is 12.5 px; with a 125 Hz
     mouse at 1000 px/s it is 8 px. Step-aware prediction removes most of this
     for periodic input, but jittery event timing remains.
  2. **Missed compositions on the VM.** With software rendering and a 64 Hz
     virtual display, some frames still miss the DWM deadline and show the previous
     position (one frame ≈ 28 px at 1800 px/s). A shorter lead makes this worse
     (measured), a longer one makes cause 1 worse.

### Where perfect sync is impossible

The overlay is a different window from the one being dragged. Exact sync would
require drawing *inside* the dragged window's composition (cross-process
`SetParent`, injection into the target), which is unsafe, breaks for elevated and
UWP windows, and can still not draw above the window's own top edge. The best a
separate window can do is what this does: sample as late as the GPU allows and
predict the rest. So the residual error is bounded by
`window speed × (sample-to-latch time)` whenever an input event lands in that
gap, plus any frame the system misses. At slow or moderate drag speeds that is
0–1 px; for flings it is a few pixels for a frame or two, which is also when
the pile is scattered off anyway (> 900 px/s).

## Measured on a real PC (build 598fe4c, 2026-11-20)

Windows 10 19045, AMD Radeon RX 580, one 1920×1080 monitor at **100 Hz**, 100%
scale, Desktop Duplication capture, full durations.

| late latch + prediction | input Hz | still | slow | sine | fast | stop | vertical |
|---|---:|---:|---:|---:|---:|---:|---:|
| 598fe4c | 60 | 0/0 (100%) | 1.0/3 (60%) | 8.5/31 (55%) | 10.0/31 (67%) | 3.5/25 (83%) | 5.9/16 (60%) |
| 598fe4c | 144 | 0/0 (100%) | 0.9/2 (72%) | 7.6/26 (37%) | 11.3/26 (40%) | 3.0/21 (77%) | 5.5/13 (42%) |
| 598fe4c | 1000 | 0/0 (100%) | 0.8/2 (67%) | 6.6/19 (38%) | 10.1/21 (26%) | 2.4/15 (75%) | 5.1/11 (45%) |

Plain snowfall: 49.7–50.0 fps, 0 latency-wait timeouts, 0 stalled-overlay skips.
Click-through PASS, the overlay was never the foreground window, still windows 0 px.

**Why it was worse than on the 64 Hz VM.** Every overlay log line of the run
reads `3000 frames (50.0 fps, cap 60)`, *including the minutes with drags*. The
60 fps cap was applied while windows moved, and the pacer only skips whole
compositions: at 100 Hz, `1/60 s` means every second refresh. So on half of the
displayed frames the overlay showed the previous frame's pile while the window
had moved on by a full 10 ms. The numbers match that exactly: at 150 px/s the
mean signed lag is −0.83 px ≈ 5.5 ms ≈ half of the frames × 10 ms; at
1800 px/s a stale frame is 18 px plus the input step, which is the measured p95
of 21–31 px. On the VM (64 Hz) the cap allowed every refresh, so this never
showed there. Since 1baa4c8 the cap applies to idle snowfall only; while any
window moves, every composition gets a new frame (`src/core/pacing.h`, with
unit tests for 60/64/100/144 Hz). Idle snowfall still runs at whole fractions
of the refresh rate up to the cap (50 fps at 100 Hz, 72 fps at 144 Hz).

**What the run cannot tell yet.** It is not measured whether the remaining
error after this fix reaches ≤ 1 px at p95 for fast drags on that PC. The
physical floor described in "Where perfect sync is impossible" still applies.

**Other observations from that run**
- The `vsync-nopredict` baseline lost every frame from `144 Hz slow` on, still
  sweeps included, while the same overlay kept presenting 50 fps without
  timeouts. The likeliest cause is another window covering the blue test window
  (the plain-snowfall log also shows a 370 ms fullscreen window at 11:20:43, so
  something else was on screen); a stuck position tracker would look the same
  in the summary. The harness now checks before every sweep that nothing covers
  the test window, names what does, and raises the test window again.
- `Invisible border: FAIL` was a measurement artefact: the visible edges matched
  the extended frame bounds within the 1-px Windows 10 border on the left, right
  and bottom, but the top was found 31 px low because the title bar was too dark
  for the brightness threshold. The check now uses an inactive test window and a
  threshold of 12 over the pure-black backdrop.

### Not measured yet (needs your PC)

- Drag sync after the pacing fix at 100/144 Hz (run `harness.cmd`).
- **Real mouse drags** (the OS move/size loop instead of `SetWindowPos`): use
  `run.cmd` and the debug overlay's "est. pile error", or record the screen.
