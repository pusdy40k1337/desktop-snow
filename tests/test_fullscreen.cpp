#include <string>
#include <vector>

#include "../src/core/fullscreen.h"
#include "../src/core/pacing.h"
#include "../src/core/settings.h"
#include "../src/core/world.h"
#include "test.h"

using namespace snow;

// ---- pacing -----------------------------------------------------------------

// Simulates `seconds` of frames and returns how many compositions got a new frame.
static int FramesIn(double hz, int cap, bool uncapped, double seconds) {
  double period = 1.0 / hz, last = 0;
  int n = 0;
  for (double next = period; next < seconds;) {
    double t = ChooseFrameTarget(next, last, period, cap, uncapped);
    if (t >= seconds) break;
    ++n;
    last = t;
    next = t + period;  // the following composition is the earliest we can make
  }
  return n;
}

TEST(pacing_moving_window_gets_every_composition) {
  // The user's PC: 100 Hz display, default cap 60. Moving: every refresh.
  CHECK(FramesIn(100, 60, true, 10.0) >= 995);
  CHECK(FramesIn(144, 60, true, 10.0) >= 1435);
  CHECK(FramesIn(60, 60, true, 10.0) >= 595);
}

TEST(pacing_idle_snowfall_keeps_its_cap) {
  // Idle: whole refreshes only, never above the cap (100 Hz -> 50 fps, 144 -> 72).
  int f100 = FramesIn(100, 60, false, 10.0);
  CHECK(f100 >= 495 && f100 <= 505);
  int f144 = FramesIn(144, 60, false, 10.0);
  CHECK(f144 >= 715 && f144 <= 725);
  int f60 = FramesIn(60, 60, false, 10.0);
  CHECK(f60 >= 595);
  int f64 = FramesIn(64, 60, false, 10.0);  // CI VM
  CHECK(f64 >= 635);
}

TEST(pacing_never_targets_a_composition_twice) {
  double period = 0.01;
  CHECK_NEAR(ChooseFrameTarget(1.0, 1.0, period, 60, true), 1.01, 1e-9);
  CHECK_NEAR(ChooseFrameTarget(1.0, 0.99, period, 60, true), 1.0, 1e-9);
}

// ---- fullscreen detection ---------------------------------------------------

static FsMonitor Mon(RectI r, RectI work) { return FsMonitor{r, work}; }

static FsWindow Fw(RectI frame) {
  FsWindow w;
  w.frame = frame;
  return w;
}

static const RectI kM1(0, 0, 1920, 1080), kW1(0, 0, 1920, 1040);
static const RectI kM2(1920, 0, 3840, 1080);

TEST(fullscreen_borderless_window_covering_monitor) {
  auto r = DetectFullscreen({Mon(kM1, kW1)}, {Fw(kM1)}, ShellNotifyState::Normal);
  CHECK(r.size() == 1 && r[0]);
  // Larger than the monitor (exclusive-style window with borders) still counts.
  r = DetectFullscreen({Mon(kM1, kW1)}, {Fw(RectI(-8, -8, 1928, 1088))}, ShellNotifyState::Normal);
  CHECK(r[0]);
}

TEST(fullscreen_ordinary_windows_are_not) {
  FsWindow max = Fw(kW1);  // maximised: frame = work area
  max.zoomed = max.captioned = true;
  CHECK(!DetectFullscreen({Mon(kM1, kW1)}, {max}, ShellNotifyState::Normal)[0]);
  // Monitor without a taskbar: a maximised captioned window reaches every edge.
  FsWindow max2 = Fw(kM2);
  max2.zoomed = max2.captioned = true;
  CHECK(!DetectFullscreen({Mon(kM2, kM2)}, {max2}, ShellNotifyState::Normal)[0]);
  // A few pixels short is not fullscreen without the shell's confirmation...
  FsWindow almost = Fw(RectI(0, 0, 1920, 1077));
  almost.foreground = true;
  CHECK(!DetectFullscreen({Mon(kM1, kW1)}, {almost}, ShellNotifyState::Normal)[0]);
  // ...but is when the shell reports a busy (fullscreen) foreground app.
  CHECK(DetectFullscreen({Mon(kM1, kW1)}, {almost}, ShellNotifyState::Busy)[0]);
  CHECK(!DetectFullscreen({Mon(kM1, kW1)}, {}, ShellNotifyState::Normal)[0]);
}

TEST(fullscreen_maximised_but_covering_the_taskbar_counts) {
  // Some apps go fullscreen by staying maximised with a caption but covering
  // the taskbar too: more than an ordinary maximised window can reach.
  FsWindow w = Fw(kM1);
  w.zoomed = w.captioned = true;
  CHECK(DetectFullscreen({Mon(kM1, kW1)}, {w}, ShellNotifyState::Normal)[0]);
}

TEST(fullscreen_looks_through_floating_and_topmost_windows) {
  FsWindow controls = Fw(RectI(600, 950, 1300, 1030));  // player controls
  controls.floating = true;
  FsWindow pip = Fw(RectI(1500, 700, 1900, 1000));  // always-on-top picture-in-picture
  pip.topmost = true;
  FsWindow video = Fw(kM1);
  CHECK(DetectFullscreen({Mon(kM1, kW1)}, {controls, pip, video}, ShellNotifyState::Normal)[0]);
  // An ordinary window on top no longer resumes the snow: opening WinRAR or a
  // file dialog over a fullscreen Chrome/video must keep the monitor paused.
  FsWindow notepad = Fw(RectI(100, 100, 900, 700));
  CHECK(DetectFullscreen({Mon(kM1, kW1)}, {notepad, video}, ShellNotifyState::Normal)[0]);
  // But a merely maximised window underneath is not a fullscreen app.
  FsWindow maxBelow = Fw(kW1);
  maxBelow.zoomed = maxBelow.captioned = true;
  CHECK(!DetectFullscreen({Mon(kM1, kW1)}, {notepad, maxBelow}, ShellNotifyState::Normal)[0]);
}

TEST(fullscreen_is_per_monitor) {
  FsWindow video = Fw(kM2);
  FsWindow editor = Fw(RectI(100, 100, 1500, 900));
  editor.foreground = true;  // the user works on monitor 1, the video plays on monitor 2
  auto r = DetectFullscreen({Mon(kM1, kW1), Mon(kM2, kM2)}, {editor, video}, ShellNotifyState::Normal);
  CHECK(!r[0]);
  CHECK(r[1]);
}

TEST(fullscreen_shell_signals) {
  FsWindow game = Fw(RectI(1920, 0, 2560, 480));  // exclusive mode, resolution not yet updated
  game.foreground = true;
  auto r = DetectFullscreen({Mon(kM1, kW1), Mon(kM2, kM2)}, {game}, ShellNotifyState::D3DFullscreen);
  CHECK(!r[0]);
  CHECK(r[1]);
  r = DetectFullscreen({Mon(kM1, kW1), Mon(kM2, kM2)}, {}, ShellNotifyState::D3DFullscreen);
  CHECK(r[0] && r[1]);
  r = DetectFullscreen({Mon(kM1, kW1), Mon(kM2, kM2)}, {}, ShellNotifyState::Presentation);
  CHECK(r[0] && r[1]);
}

// ---- pause decision: fullscreen + maximised ----------------------------------

static PauseDecision Decide1(const std::vector<FsWindow>& ws, PauseOptions o = PauseOptions{}) {
  return DecidePause({Mon(kM1, kW1)}, ws, ShellNotifyState::Normal, o)[0];
}

TEST(pause_maximized_window_on_top) {
  FsWindow browser = Fw(kW1);  // maximised with the normal button: frame = work area, taskbar visible
  browser.zoomed = browser.captioned = browser.foreground = true;
  PauseDecision d = Decide1({browser});
  CHECK(d.pause);
  CHECK(d.reason == PauseReason::Maximized);
  CHECK(d.window == 0);
  CHECK(std::string(d.rule) == "maximized (zoomed)");
  CHECK(std::string(PauseReasonName(d.reason)) == "maximized");
  // The option switched off: still reported, but no pause.
  PauseOptions off;
  off.maximized = false;
  d = Decide1({browser}, off);
  CHECK(!d.pause);
  CHECK(d.reason == PauseReason::Maximized);
  // Windows 10 style: the DWM frame of a maximised window can stop a pixel short.
  FsWindow dpi = Fw(RectI(0, 1, 1919, 1039));
  dpi.zoomed = dpi.captioned = true;
  CHECK(Decide1({dpi}).reason == PauseReason::Maximized);
}

TEST(pause_snapped_or_borderless_window_filling_the_work_area) {
  FsWindow snapped = Fw(kW1);  // not zoomed, resized / snapped to the full work area
  PauseDecision d = Decide1({snapped});
  CHECK(d.pause && d.reason == PauseReason::Maximized);
  CHECK(std::string(d.rule) == "covers the work area");
  // Borderless maximised app: frame overhangs the work area by its resize border.
  FsWindow borderless = Fw(RectI(-8, -8, 1928, 1048));
  borderless.zoomed = true;
  d = Decide1({borderless});
  CHECK(d.pause && d.reason == PauseReason::Maximized);
}

TEST(pause_almost_the_work_area_alone_does_not_pause) {
  FsWindow almost = Fw(RectI(0, 0, 1920, 1030));  // 10 px short of the work area
  PauseDecision d = Decide1({almost});
  CHECK(!d.pause);
  CHECK(d.reason == PauseReason::None);
  CHECK(std::string(d.rule) == "top window does not cover");
  FsWindow narrow = Fw(RectI(6, 0, 1920, 1040));  // 6 px short at the left edge
  CHECK(!Decide1({narrow}).pause);
  // A maximised window below a small one stays paused: the user is still
  // working inside the maximised app (Spotify over a maximised Chrome).
  FsWindow below = Fw(kW1);
  below.zoomed = below.captioned = true;
  PauseDecision under = Decide1({almost, below});
  CHECK(under.pause && under.reason == PauseReason::Maximized);
  CHECK(under.window == 1);
  CHECK(std::string(under.rule) == "maximized (zoomed)");
  CHECK(std::string(Decide1({}).rule) == "no window");
  CHECK(std::string(PauseReasonName(PauseReason::None)) == "none");
}

TEST(pause_small_window_over_maximised_keeps_pause) {
  // The reported bug: Chrome maximised, Spotify opened on top -> snow started
  // falling over everything. The maximised window keeps the monitor paused.
  FsWindow browser = Fw(kW1);
  browser.zoomed = browser.captioned = true;
  FsWindow spotify = Fw(RectI(1200, 120, 1850, 700));
  spotify.foreground = true;
  PauseDecision d = Decide1({spotify, browser});
  CHECK(d.pause);
  CHECK(d.reason == PauseReason::Maximized);
  CHECK(std::string(d.rule) == "maximized (zoomed)");
  // Same with a borderless fullscreen window under the small one.
  FsWindow video = Fw(kM1);
  PauseDecision f = Decide1({spotify, video});
  CHECK(f.pause && f.reason == PauseReason::Fullscreen);
  CHECK(std::string(f.rule) == "covers the monitor (any z-order)");
}

TEST(pause_fullscreen_within_tolerance) {
  // Borderless fullscreen a pixel or two short of the monitor (125 %/150 % rounding).
  FsWindow fs = Fw(RectI(1, 0, 1920, 1078));
  PauseDecision d = Decide1({fs});
  CHECK(d.pause && d.reason == PauseReason::Fullscreen);
  CHECK(std::string(d.rule) == "covers the monitor (any z-order)");
  CHECK(std::string(PauseReasonName(d.reason)) == "fullscreen");
  // Fullscreen pause off: no pause even though it is reported.
  PauseOptions off;
  off.fullscreen = false;
  d = Decide1({fs}, off);
  CHECK(!d.pause && d.reason == PauseReason::Fullscreen);
  // F11 browser that was maximised: no caption any more, covers the monitor.
  FsWindow f11 = Fw(kM1);
  f11.zoomed = true;
  CHECK(Decide1({f11}).reason == PauseReason::Fullscreen);
}

TEST(pause_topmost_fullscreen_window) {
  FsWindow player = Fw(kM1);  // "always on top" video player in fullscreen
  player.topmost = true;
  FsWindow editor = Fw(RectI(100, 100, 900, 700));
  PauseDecision d = Decide1({player, editor});
  CHECK(d.pause && d.reason == PauseReason::Fullscreen && d.window == 0);
  // Topmost maximised window (Task Manager "always on top", maximised).
  FsWindow tm = Fw(kW1);
  tm.topmost = tm.zoomed = tm.captioned = true;
  CHECK(Decide1({tm, editor}).reason == PauseReason::Maximized);
}

TEST(pause_fullscreen_holds_under_a_small_window) {
  // Chrome in F11 fullscreen, then the user opens WinRAR on top of it: the snow
  // must stay paused (the fullscreen app is still what fills the monitor).
  FsWindow chrome = Fw(kM1);
  FsWindow winrar = Fw(RectI(300, 200, 1200, 800));
  winrar.foreground = true;
  PauseDecision d = Decide1({winrar, chrome});
  CHECK(d.pause && d.reason == PauseReason::Fullscreen);
  CHECK(d.window == 1);
  CHECK(std::string(d.rule) == "covers the monitor (any z-order)");
  // The fullscreen pause switched off: reported, but no pause.
  PauseOptions off;
  off.fullscreen = false;
  d = Decide1({winrar, chrome}, off);
  CHECK(!d.pause && d.reason == PauseReason::Fullscreen);
  // A maximised window under the small one keeps the monitor paused: the user
  // is still working inside the maximised app (Spotify over a maximised Chrome).
  FsWindow maximised = Fw(kW1);
  maximised.zoomed = maximised.captioned = true;
  PauseDecision under = Decide1({winrar, maximised});
  CHECK(under.pause && under.reason == PauseReason::Maximized);
  CHECK(std::string(under.rule) == "maximized (zoomed)");
  // A borderless window that fills the work area and stops short of the monitor
  // edge is a maximised-style window too: it keeps the pause.
  FsWindow shortBy10 = Fw(RectI(0, 0, 1920, 1070));
  PauseDecision shortD = Decide1({winrar, shortBy10});
  CHECK(shortD.pause && shortD.reason == PauseReason::Maximized);
  CHECK(std::string(shortD.rule) == "covers the work area");
}

TEST(pause_second_monitor_with_other_dpi) {
  // Monitor 2: 2560x1440 at 150 %, right of monitor 1, its own taskbar.
  const RectI m2(1920, 0, 4480, 1440), w2(1920, 0, 4480, 1392);
  std::vector<FsMonitor> mons = {Mon(kM1, kW1), Mon(m2, w2)};
  FsWindow video = Fw(RectI(1921, 0, 4479, 1439));  // fullscreen video, 1 px short on each side
  FsWindow editor = Fw(RectI(100, 100, 1500, 900));
  editor.foreground = true;
  auto d = DecidePause(mons, {editor, video}, ShellNotifyState::Normal, PauseOptions{});
  CHECK(!d[0].pause && d[0].reason == PauseReason::None);
  CHECK(d[1].pause && d[1].reason == PauseReason::Fullscreen && d[1].window == 1);
  // Maximised browser on monitor 2 only.
  FsWindow browser = Fw(w2);
  browser.zoomed = browser.captioned = true;
  d = DecidePause(mons, {browser, editor}, ShellNotifyState::Normal, PauseOptions{});
  CHECK(!d[0].pause);
  CHECK(d[1].pause && d[1].reason == PauseReason::Maximized);
}

TEST(pause_spill_onto_neighbour_monitor_is_ignored) {
  // A borderless maximised window on monitor 1 overhangs 8 px onto monitor 2,
  // where a fullscreen video plays below it in z-order.
  FsWindow spill = Fw(RectI(-8, -8, 1928, 1048));
  spill.zoomed = true;
  FsWindow video = Fw(kM2);
  auto d = DecidePause({Mon(kM1, kW1), Mon(kM2, kM2)}, {spill, video}, ShellNotifyState::Normal, PauseOptions{});
  CHECK(d[0].reason == PauseReason::Maximized);
  CHECK(d[1].reason == PauseReason::Fullscreen && d[1].window == 1);
}

TEST(pause_shell_states_name_their_rule) {
  auto d = DecidePause({Mon(kM1, kW1)}, {}, ShellNotifyState::Presentation, PauseOptions{});
  CHECK(d[0].pause && std::string(d[0].rule) == "shell: presentation mode");
  FsWindow game = Fw(kM1);
  game.foreground = true;
  d = DecidePause({Mon(kM1, kW1)}, {game}, ShellNotifyState::D3DFullscreen, PauseOptions{});
  CHECK(d[0].pause && d[0].window == 0 && std::string(d[0].rule) == "shell: exclusive fullscreen (D3D)");
}

TEST(fullscreen_debounce) {
  FullscreenDebouncer d(0.3);
  std::vector<std::string> fs = {"M1"}, none;
  CHECK(!d.update(0.0, fs));
  CHECK_NEAR(d.pendingIn(0.1), 0.2, 1e-9);
  CHECK(!d.update(0.2, fs));
  CHECK(d.update(0.31, fs));
  CHECK(d.applied() == fs);
  CHECK(d.pendingIn(0.31) < 0);
  // Alt+Tab: away for 0.2 s and back -> no change, no flicker.
  CHECK(!d.update(1.0, none));
  CHECK(!d.update(1.2, fs));
  CHECK(!d.update(1.6, fs));
  CHECK(d.applied() == fs);
  // A different state restarts the clock.
  CHECK(!d.update(2.0, none));
  CHECK(!d.update(2.2, {"M2"}));
  CHECK(!d.update(2.4, {"M2"}));
  CHECK(d.update(2.55, {"M2"}));
  d.reset({});
  CHECK(d.applied().empty());
}

// ---- world pause -------------------------------------------------------------

TEST(world_paused_monitor_spawns_nothing_and_freezes) {
  MonitorInfo a, b;
  a.id = "A";
  a.rect = RectI(0, 0, 1920, 1080);
  a.work = RectI(0, 0, 1920, 1040);
  b.id = "B";
  b.rect = RectI(1920, 0, 3840, 1080);
  b.work = b.rect;
  World w;
  SimSettings s;
  s.windGusts = s.sparkle = s.cursorSweep = false;
  w.configure(s);
  w.setMonitors({a, b});
  w.setPausedMonitors({0, 1});  // B paused from the first frame: nothing spawns there
  std::vector<WindowInput> none;
  double t = 0;
  for (int i = 0; i < 120; ++i) w.update(t += 1.0 / 60, 1.f / 60, none);
  auto countOn = [&](int mon) {
    int n = 0;
    for (const auto& p : w.particles())
      if (p.kind != ParticleKind::Debris && p.monitor == mon) ++n;
    return n;
  };
  CHECK(countOn(0) > 100);
  CHECK(countOn(1) == 0);

  // Resume B, let it fill, then pause A: A's flakes stay exactly where they were.
  w.setPausedMonitors({0, 0});
  for (int i = 0; i < 120; ++i) w.update(t += 1.0 / 60, 1.f / 60, none);
  CHECK(countOn(1) > 100);
  w.setPausedMonitors({1, 0});
  std::vector<Particle> before;
  for (const auto& p : w.particles())
    if (p.monitor == 0 && p.kind != ParticleKind::Debris) before.push_back(p);
  for (int i = 0; i < 60; ++i) w.update(t += 1.0 / 60, 1.f / 60, none);
  std::vector<Particle> after;
  for (const auto& p : w.particles())
    if (p.monitor == 0 && p.kind != ParticleKind::Debris) after.push_back(p);
  CHECK(before.size() == after.size());
  bool same = before.size() == after.size();
  for (size_t i = 0; same && i < before.size(); ++i) same = before[i].x == after[i].x && before[i].y == after[i].y;
  CHECK(same);
  // ...and move again once resumed.
  w.setPausedMonitors({0, 0});
  w.update(t += 1.0 / 60, 1.f / 60, none);
  bool moved = false;
  size_t k = 0;
  for (const auto& p : w.particles())
    if (p.monitor == 0 && p.kind != ParticleKind::Debris && k < after.size()) moved = moved || p.y != after[k++].y;
  CHECK(moved);
}

TEST(world_monitor_unplug_and_replug) {
  MonitorInfo a, b;
  a.id = "A";
  a.rect = RectI(0, 0, 1920, 1080);
  a.work = RectI(0, 0, 1920, 1040);
  b.id = "B";
  b.rect = RectI(1920, 0, 4480, 1440);  // larger, other DPI
  b.work = RectI(1920, 0, 4480, 1392);
  b.scale = 1.5f;
  World w;
  SimSettings s;
  s.windGusts = s.sparkle = s.cursorSweep = false;
  w.configure(s);
  w.setMonitors({a, b});
  w.setPausedMonitors({0, 0});
  std::vector<WindowInput> none;
  double t = 0;
  for (int i = 0; i < 120; ++i) w.update(t += 1.0 / 60, 1.f / 60, none);
  auto countOn = [&](int mon) {
    int n = 0;
    for (const auto& p : w.particles())
      if (p.kind != ParticleKind::Debris && p.monitor == mon) ++n;
    return n;
  };
  CHECK(countOn(1) > 100);
  // Unplug B (the pause vector still has two entries until the app catches up).
  w.setMonitors({a});
  for (int i = 0; i < 60; ++i) w.update(t += 1.0 / 60, 1.f / 60, none);
  bool valid = true;
  for (const auto& p : w.particles()) valid = valid && p.monitor < 1;
  CHECK(valid);
  CHECK(countOn(0) > 100);
  // Plug it back in: it snows there again.
  w.setMonitors({a, b});
  w.setPausedMonitors({0, 0});
  for (int i = 0; i < 120; ++i) w.update(t += 1.0 / 60, 1.f / 60, none);
  CHECK(countOn(1) > 100);
}

TEST(settings_pause_fullscreen_roundtrip) {
  AppState st;
  CHECK(st.settings.pauseFullscreen);  // default on
  st.settings.pauseFullscreen = false;
  AppState back;
  FromJson(ToJson(st), back);
  CHECK(!back.settings.pauseFullscreen);
  AppState defaults;
  FromJson(json::Value::object(), defaults);
  CHECK(defaults.settings.pauseFullscreen);
}
