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
  // One pixel short is not fullscreen without the shell's confirmation...
  FsWindow almost = Fw(RectI(0, 0, 1920, 1079));
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
  // An ordinary window on top: the user is working, not watching.
  FsWindow notepad = Fw(RectI(100, 100, 900, 700));
  CHECK(!DetectFullscreen({Mon(kM1, kW1)}, {notepad, video}, ShellNotifyState::Normal)[0]);
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
