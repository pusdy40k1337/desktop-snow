# Manual test checklist (Windows 10 2004+ / Windows 11)

Build once with `build.cmd`, then run `dist\DesktopSnow.exe` (or `run.cmd` for
the debug build with the on-screen debug panel and a log console). Tick each
item; anything that fails: note Windows version, display setup (resolution,
scale %, refresh rate) and attach `%LOCALAPPDATA%\DesktopSnow\logs\main.log`.

## 1. Snow on windows (top priority)
- [ ] Snow falls on every monitor; a light dusting is already on existing windows at start.
- [ ] Open Notepad (not maximised). Within a few minutes a pile grows **exactly on the
      visible top edge** of its frame: no gap above, no overlap into the title bar,
      no horizontal offset. Zoom in with the Magnifier (Win + Plus) to check.
- [ ] Windows 11: the pile drapes over the **rounded corners** instead of floating past them.
- [ ] Drag Notepad slowly around: the pile moves with it, no visible lag, wobble or drift.
- [ ] Turn on *Debug overlay* (tray): the yellow 1-px outline of the dragged window
      sits exactly on its frame while dragging; the panel shows speed, lead time
      and "est. pile error".
- [ ] Fling the window fast (or shake it): the pile bursts off; particles fly in the
      direction of motion, fall and fade. A big pile throws more snow than a small one.
- [ ] Drag at a steady, moderate speed (well under one screen width per second): the pile stays on.
- [ ] Stop and wait ~2 s: the pile starts regrowing.
- [ ] Resize the window from the right edge: the left part of the pile stays where it was.
      Resize from the left edge: the right part stays. Maximise/restore: the pile
      comes back stretched, not reset.
- [ ] Put another window partly over Notepad's top edge: no snow is drawn over the
      front window; the covered part of the pile is hidden, and is still there when
      you move the front window away.
- [ ] Minimise Notepad and restore it: its snow is still there. Close it: its snow falls.
- [ ] Maximised and top-snapped windows have no pile (there is no room above them).
- [ ] The taskbar (bottom) collects snow along its top edge; auto-hidden taskbar: no pile.
- [ ] Per-monitor DPI: move a window between a 100% and a 150% monitor; the pile stays
      aligned on both and keeps its look.
- [ ] Not tracked: tool windows/palettes, the desktop, the Start menu, tooltips/menus,
      UWP windows on other virtual desktops (Task View), the overlay itself.

## 2. Drag-sync measurements
- [ ] Close other heavy apps, then run `harness.cmd` (≈ 7 minutes; the screen
      shows a black backdrop and a blue window moving, then a black "fullscreen
      app" for ~10 s). Do not touch mouse or keyboard. Send back
      `SnowHarness-results.txt` (written next to the exe). Repeat on a 144 Hz monitor if you have one.
- [ ] In the results, the `engine: last 60 s` lines of `overlay-latch-step.log`
      say `... while windows moved (X fps)`: X must be close to the display rate
      (100 on a 100 Hz monitor), not half of it.
- [ ] Real mouse: `run.cmd`, then drag Notepad by its title bar fast and slow,
      in circles and with sudden stops. Watch the pile edge with the Magnifier;
      the debug panel's "est. pile error" should stay near 0–2 px while dragging
      at a steady speed.

## 3. Decorations
- [ ] `Ctrl+Alt+D` (or tray → *Decorate…*): the screen dims slightly and a toolbar appears.
- [ ] *Tree*: click on the desktop → a tree grows (max 3; a 4th shows a notice).
- [ ] *Bauble* / *Candy cane*: click on the tree's branches → ornaments appear where you clicked.
- [ ] *Lights*: toggles twinkling garlands (smooth, no strobing). *Star*: toggles the star.
- [ ] Drag a tree to move it.
- [ ] *Snowman*: click on (or just above) a window's top edge → a snowman builds itself
      there. Drag the window: the snowman travels with it, no lag.
- [ ] *Hat* / *Scarf*: click a snowman to cycle. *Remove*: click an ornament/star/tree/snowman.
- [ ] `Esc`, right-click or *Done* leaves the mode. Clicks reach the apps again.
- [ ] Quit and start again: trees, ornaments and snowmen are where you left them;
      the snowman finds its window again (same app + title), or waits on the taskbar
      if that window is not open.
- [ ] Tray → *Surprise me*: Santa peeks in from a screen edge, the gift train crosses
      above the taskbar, the yeti jumps at a snowman (needs a snowman).

## 4. System behaviour
- [ ] Clicks, scrolling and drags reach the windows under the snow everywhere
      (try clicking on a pile and on falling flakes).
- [ ] The overlay never takes focus; it is not in Alt+Tab or on the taskbar.
- [ ] `Ctrl+Alt+S` toggles snow. If the hotkey is taken, a notice appears and the tray still works.
- [ ] Fullscreen (tray → *Pause during fullscreen* ticked, the default). Each of
      these hides the snow on its monitor within about half a second, and it
      fades back in about half a second after leaving fullscreen:
      a game in exclusive fullscreen; a game in borderless/windowed fullscreen;
      YouTube fullscreen (`F`) in Chrome/Edge/Firefox; `F11` in a browser;
      a video player (Movies & TV, VLC, mpv) fullscreen, also while its controls
      show; PowerPoint slideshow (`F5`).
- [ ] Alt+Tab out of a fullscreen game and straight back: no flash of snow.
      Win key / Start menu over a fullscreen video: the snow stays hidden.
- [ ] A maximised window (taskbar visible) is not fullscreen: snow keeps falling.
- [ ] Two monitors: fullscreen video on one, work on the other (click there):
      snow stays hidden on the video monitor only.
- [ ] Untick *Pause during fullscreen*: snow appears over the fullscreen app at once.
- [ ] Task Manager while everything is hidden by a fullscreen app: Desktop Snow
      uses ~0% CPU and ~0% GPU.
- [ ] Taskbar snow: maximise Notepad or a browser. The snow line on the taskbar's
      top edge never covers the bottom of the app (status bar, input fields, the
      browser's bottom bar); it is only visible where the desktop is behind it.
      Tray → *Snow on the taskbar* turns that snow line off and on.
- [ ] One monitor turned off at its power button (others stay on): the others keep
      snowing smoothly (the log shows no rising `latency-wait timeouts`), and the
      monitor gets its snow back when turned on.
- [ ] On battery or with battery saver: fewer flakes, 30 fps (debug panel shows `battery 1`).
- [ ] Settings → Accessibility → Visual effects → *Animation effects* off: reduced motion
      (no sway, no visitors, no twinkle). Tray → *Quick settings* → *Reduced motion* overrides it.
- [ ] Plug/unplug a monitor, change resolution or scale: overlays follow within a second.
- [ ] Sleep/resume, lock/unlock (`Win+L`): snow pauses and resumes.
- [ ] Restart Explorer (Task Manager → Windows Explorer → Restart): the tray icon comes back.
- [ ] Remote Desktop into the PC: it keeps working (capped at 30 fps).
- [ ] A UAC prompt appears and goes away: nothing breaks. An elevated window (e.g.
      Task Manager) gets snow too.
- [ ] Tray → *Launch at startup*, sign out/in: it starts. Untick: it does not.
- [ ] Start a second copy of the exe: no second overlay, the tray menu of the first opens.
- [ ] Tray → *Language*: English / Русский / Українська switch the menu and toolbar.
- [ ] Idle CPU with snow falling ≈ 1–3 % in Task Manager; memory < 150 MB.
      Leave it running for 25 minutes or more while you work normally: memory,
      handles and GDI objects stay flat (Task Manager → Details → add the
      *Handles* and *GDI objects* columns), and the log's per-minute lines show
      a steady fps and no growing number of timeouts.
- [ ] Decorations look right on a real GPU: trees (bare, with baubles, candy
      canes, lights, star), snowmen with each hat and scarf, Santa, train, yeti.
      Compare with the CI renders (`decor-render-<commit>` artifact); report
      anything blurry, misaligned or oddly coloured.
- [ ] Corrupt `%APPDATA%\DesktopSnow\state.json` by hand (delete half of it), start the
      app: it starts with the last good copy and shows a notice.
