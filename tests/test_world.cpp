#include <cmath>
#include <vector>

#include "../src/core/actors.h"
#include "../src/core/decor.h"
#include "../src/core/strings.h"
#include "../src/core/theme.h"
#include "../src/core/world.h"
#include "test.h"

using namespace snow;

static std::vector<MonitorInfo> OneMonitor() {
  MonitorInfo m;
  m.id = "M1";
  m.rect = RectI(0, 0, 1920, 1080);
  m.work = RectI(0, 0, 1920, 1040);
  m.scale = 1;
  m.primary = true;
  return {m};
}

static WindowInput Win(uint64_t id, RectI r, int z) {
  WindowInput w;
  w.id = id;
  w.rect = r;
  w.z = z;
  w.holdsSnow = true;
  return w;
}

static SimSettings Quiet() {
  SimSettings s;
  s.windGusts = false;
  s.sparkle = false;
  s.cursorSweep = false;
  s.initialDusting = 0;
  return s;
}

TEST(world_snow_accumulates_on_window_top_edge) {
  World w;
  w.configure(Quiet());
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  double t = 0;
  for (int f = 0; f < 60 * 120; ++f) {
    t += 1 / 60.0;
    w.update(t, 1 / 60.f, wins);
  }
  const Surface* s = w.surface(1);
  CHECK(s != nullptr);
  CHECK(s->pile.mass() > 100.f);
  CHECK(s->pile.peak() <= 34.f + 1e-3f);
  // The pile's base is exactly the window's top edge.
  CHECK(World::BaseY(*s, 900) == 500.f);
}

TEST(world_pile_follows_window_exactly) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.3f;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  float massBefore = w.surface(1)->pile.mass();
  // Slow drag: 300 px/s for 2 s. Pile must stay attached and in place.
  double t = 0.016;
  for (int f = 0; f < 120; ++f) {
    t += 1 / 60.0;
    int dx = static_cast<int>(std::lround(300 * (t - 0.016)));
    wins[0].rect = RectI(400 + dx, 500 + dx / 3, 1400 + dx, 900 + dx / 3);
    wins[0].velocity = Vec2(300, 100);
    w.update(t, 1 / 60.f, wins);
    const Surface* s = w.surface(1);
    CHECK(s->rect == wins[0].rect);
    CHECK(World::BaseY(*s, static_cast<float>(wins[0].rect.left + 500)) == static_cast<float>(wins[0].rect.top));
  }
  CHECK(w.surface(1)->pile.mass() >= massBefore * 0.95f);
  CHECK(w.stats().bursts == 0);
}

TEST(world_fast_fling_scatters_and_regrows) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.4f;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  float before = w.surface(1)->pile.mass();
  CHECK(before > 100.f);
  wins[0].velocity = Vec2(2400, 0);
  w.update(0.033, 1 / 60.f, wins);
  float after = w.surface(1)->pile.mass();
  CHECK(after < before * 0.2f);
  CHECK(w.stats().debris > 50);
  CHECK(w.stats().bursts == 1);
  // Window rests: after the regrow delay the pile grows back.
  wins[0].velocity = Vec2();
  double t = 0.033;
  for (int f = 0; f < 60 * 20; ++f) {
    t += 1 / 60.0;
    w.update(t, 1 / 60.f, wins);
  }
  CHECK(w.surface(1)->pile.mass() > after * 3.f);
}

TEST(world_occluded_columns_do_not_accumulate) {
  World w;
  w.configure(Quiet());
  w.setMonitors(OneMonitor());
  // Window 2 (on top) covers the middle of window 1's top edge.
  std::vector<WindowInput> wins = {Win(2, RectI(700, 300, 1000, 800), 0), Win(1, RectI(400, 500, 1400, 900), 1)};
  double t = 0;
  for (int f = 0; f < 60 * 60; ++f) {
    t += 1 / 60.0;
    w.update(t, 1 / 60.f, wins);
  }
  const Surface* s = w.surface(1);
  CHECK(s != nullptr);
  int covered = s->pile.columnAt(850 - 400);
  CHECK(!s->exposed[static_cast<size_t>(covered)]);
  CHECK(s->pile.height(covered) < 1.f);
  CHECK(s->pile.height(s->pile.columnAt(100)) > 1.f);
}

TEST(world_maximized_like_window_at_screen_top_has_no_snow) {
  World w;
  w.configure(Quiet());
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(0, 0, 1920, 1040), 0)};
  double t = 0;
  for (int f = 0; f < 600; ++f) {
    t += 1 / 60.0;
    w.update(t, 1 / 60.f, wins);
  }
  const Surface* s = w.surface(1);
  CHECK(s == nullptr || s->pile.mass() < 1e-3f);
}

TEST(world_minimised_window_keeps_snow_destroyed_drops_it) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.4f;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  float m = w.surface(1)->pile.mass();
  std::vector<WindowInput> none;
  w.update(0.033, 1 / 60.f, none);  // minimised: dormant
  CHECK(w.surface(1) != nullptr);
  CHECK(!w.surface(1)->active);
  w.update(0.05, 1 / 60.f, wins);   // restored
  CHECK(w.surface(1)->pile.mass() >= m * 0.95f);
  w.windowDestroyed(1);
  CHECK(w.surface(1) == nullptr);
  CHECK(w.stats().debris > 0);
}

TEST(world_resize_resamples_pile) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.4f;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  float m = w.surface(1)->pile.mass();
  wins[0].rect.right = 1600;  // drag the right edge outward
  w.update(0.033, 1 / 60.f, wins);
  CHECK(w.surface(1)->pile.width() == 1200.f);
  CHECK(w.surface(1)->pile.mass() >= m * 0.98f);
}

TEST(world_flake_count_scales_with_intensity) {
  World a, b;
  SimSettings s = Quiet();
  a.configure(s);
  s.intensity = 0.5f;
  b.configure(s);
  a.setMonitors(OneMonitor());
  b.setMonitors(OneMonitor());
  std::vector<WindowInput> none;
  a.update(0.016, 1 / 60.f, none);
  b.update(0.016, 1 / 60.f, none);
  CHECK(a.stats().flakes > 0);
  CHECK_NEAR(static_cast<double>(b.stats().flakes) / a.stats().flakes, 0.5, 0.02);
}

TEST(world_cursor_sweeps_snow) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.5f;
  set.cursorSweep = true;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  float before = w.surface(1)->pile.mass();
  for (int f = 0; f < 30; ++f) {
    w.setCursor(Vec2(450.f + f * 30.f, 495.f), true);  // 1800 px/s along the pile
    w.update(0.033 + f / 60.0, 1 / 60.f, wins);
  }
  CHECK(w.surface(1)->pile.mass() < before * 0.85f);
}

TEST(theme_twinkle_respects_flash_limit) {
  for (float rate : {0.2f, 0.8f, 5.f, 50.f}) {
    std::vector<float> samples;
    for (int i = 0; i < 240 * 10; ++i) samples.push_back(Twinkle(i / 240.0, 0.3f, rate, false));
    int flashes = CountFlashes(samples, 0.1f);
    CHECK_MSG(flashes <= 3 * 10, "at most 3 flashes per second");
    CHECK(flashes <= 9);  // clamped to 0.8 Hz -> <= 8 in 10 s
  }
  CHECK(Twinkle(1.23, 0.1f, 0.5f, true) == 1.f);
}

TEST(theme_night_factor) {
  CHECK(NightFactor(12, 0) == 0.f);
  CHECK(NightFactor(23, 0) == 1.f);
  CHECK(NightFactor(3, 0) == 1.f);
  float dusk = NightFactor(17, 30);
  CHECK(dusk > 0.f && dusk < 1.f);
}

TEST(decor_anchor_matching) {
  WindowAnchor a;
  a.process = "notepad.exe";
  a.className = "Notepad";
  a.title = "*shopping.txt - Notepad";
  a.hwnd = 0x10;
  WindowIdentity same{0x10, "notepad.exe", "Notepad", "shopping.txt - Notepad", false};
  WindowIdentity reopened{0x99, "notepad.exe", "Notepad", "shopping.txt - Notepad", false};
  WindowIdentity otherDoc{0x98, "notepad.exe", "Notepad", "todo.txt - Notepad", false};
  WindowIdentity otherApp{0x10, "chrome.exe", "Chrome_WidgetWin_1", "shopping.txt - Notepad", false};
  int s1 = AnchorMatchScore(a, same), s2 = AnchorMatchScore(a, reopened), s3 = AnchorMatchScore(a, otherDoc),
      s4 = AnchorMatchScore(a, otherApp);
  CHECK(s1 > s2 && s2 > s3);
  CHECK(s3 >= kAnchorMinScore);  // safe fallback: same app, different document
  CHECK(s4 == 0);                // never another process, even with the same handle
  WindowAnchor tb;
  tb.taskbar = true;
  CHECK(AnchorMatchScore(tb, WindowIdentity{1, "explorer.exe", "Shell_TrayWnd", "", true}) == 100);
}

TEST(decor_tree_geometry) {
  Vec2 base(500, 900);
  float h = 230;
  TreeBox b = TreeBounds(base, h);
  CHECK_NEAR(b.right - b.left, 0.72f * h, 1e-3);
  float u, v;
  Vec2 p = TreeLocalToScreen(base, h, 0.3f, 0.6f);
  CHECK(TreeScreenToLocal(base, h, p, u, v));
  CHECK_NEAR(u, 0.3, 1e-5);
  CHECK_NEAR(v, 0.6, 1e-5);
  CHECK(TreeFoliageContains(0.5f, 0.5f));
  CHECK(!TreeFoliageContains(0.05f, 0.2f));
}

TEST(decor_palette_fits_and_hits) {
  RectI work(0, 0, 1024, 728);
  PaletteLayout l = LayoutPalette(work, 1.5f);
  CHECK(l.buttons.size() == static_cast<size_t>(Tool::Count));
  CHECK(l.bar.left >= 0 && l.bar.right <= 1024);
  Tool t;
  const RectI& r = l.buttons[3].rect;
  CHECK(PaletteHit(l, r.left + 2, r.top + 2, t) && t == Tool::CandyCane);
  CHECK(!PaletteHit(l, 1, 700, t));
}

TEST(strings_complete_and_resolved) {
  for (int i = 0; i < static_cast<int>(Str::Count); ++i) {
    for (Lang l : {Lang::En, Lang::Ru, Lang::Uk}) CHECK(Tr(static_cast<Str>(i), l)[0] != '\0');
  }
  CHECK(ResolveLanguage("auto", 0x19) == Lang::Ru);
  CHECK(ResolveLanguage("auto", 0x22) == Lang::Uk);
  CHECK(ResolveLanguage("auto", 0x09) == Lang::En);
  CHECK(ResolveLanguage("uk", 0x09) == Lang::Uk);
}

TEST(actors_santa_and_yeti_lifecycle) {
  ActorSystem a;
  std::vector<MonitorInfo> mons = OneMonitor();
  std::vector<SnowmanSpot> spots = {{7, Vec2(900, 500), 0}};
  CHECK(a.trigger(ActorKind::Santa, 0, mons, spots));
  double t = 0;
  for (; t < 9; t += 1 / 60.0) a.update(t, 1 / 60.f, mons, spots);
  CHECK(!a.santa().active);
  CHECK(a.trigger(ActorKind::Yeti, t, mons, spots));
  bool scared = false, landed = false;
  for (double e = t + 30; t < e; t += 1 / 60.0) {
    a.update(t, 1 / 60.f, mons, spots);
    uint32_t id;
    float dir;
    Vec2 at;
    if (a.popSnowmanScare(id, dir)) scared = id == 7;
    if (a.popLanding(at)) landed = true;
    if (!a.yeti().active) break;
  }
  CHECK(scared);
  CHECK(landed);
  CHECK(!a.yeti().active);
  CHECK(!a.trigger(ActorKind::Yeti, t, mons, {}));  // nobody to hunt
}

TEST(world_cursor_breeze_pushes_falling_flakes) {
  World w;
  SimSettings set = Quiet();
  set.snowing = true;
  set.intensity = 1.f;
  set.cursorBreeze = 1.f;  // slider at 100 %: the strongest allowed draught
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> none;
  for (int i = 0; i < 120; ++i) w.update(i / 60.0, 1 / 60.f, none);
  w.setCursor(Vec2(300.f, 300.f), true);
  w.setCursor(Vec2(1500.f, 300.f), true);  // a fast stroke across the screen
  w.update(2.1, 1 / 60.f, none);
  int pushed = 0;
  for (const auto& p : w.particles())
    if (p.kind == ParticleKind::Flake && std::fabs(p.vx) > 25.f) ++pushed;
  CHECK(pushed > 0);
}

TEST(world_sweep_keeps_debris_per_frame_bounded) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.9f;
  set.cursorSweep = true;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  int prev = w.stats().debris;
  for (int f = 0; f < 24; ++f) {
    w.setCursor(Vec2(450.f + f * 35.f, 495.f), true);
    w.update(0.033 + f / 60.0, 1 / 60.f, wins);
    const int nowDebris = w.stats().debris;
    CHECK_MSG(nowDebris - prev <= 60, "sweeping spawns a bounded burst per frame");
    prev = nowDebris;
  }
}

TEST(world_slow_cursor_still_erases_the_drift) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.5f;
  set.cursorSweep = true;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  w.update(0.016, 1 / 60.f, wins);
  const float before = w.surface(1)->pile.mass();
  float x = 450.f;
  for (int f = 0; f < 60; ++f) {
    x += 2.f;  // ~120 px/s: slow, below the old 280 px/s threshold
    w.setCursor(Vec2(x, 495.f), true);
    w.update(0.033 + f / 60.0, 1 / 60.f, wins);
  }
  CHECK(w.surface(1)->pile.mass() < before * 0.9f);
}

TEST(world_breeze_moves_flakes_not_just_their_velocity_field) {
  SimSettings set = Quiet();
  set.snowing = true;
  set.intensity = 1.f;
  set.cursorBreeze = 1.f;
  World a, b;
  a.configure(set);
  b.configure(set);
  a.setMonitors(OneMonitor());
  b.setMonitors(OneMonitor());
  std::vector<WindowInput> none;
  for (int i = 0; i < 120; ++i) {
    a.update(i / 60.0, 1 / 60.f, none);
    b.update(i / 60.0, 1 / 60.f, none);
  }
  a.setCursor(Vec2(300.f, 300.f), true);
  a.setCursor(Vec2(1500.f, 300.f), true);  // fast stroke
  b.setCursor(Vec2(300.f, 300.f), true);
  b.setCursor(Vec2(300.f, 300.f), true);  // no movement
  a.update(2.1, 1 / 60.f, none);
  b.update(2.1, 1 / 60.f, none);
  const auto& pa = a.particles();
  const auto& pb = b.particles();
  CHECK(!pa.empty() && pa.size() == pb.size());
  bool moved = false;
  for (size_t i = 0; i < pa.size(); ++i)
    if (pa[i].kind == ParticleKind::Flake && (std::fabs(pa[i].x - pb[i].x) > 0.5f || std::fabs(pa[i].y - pb[i].y) > 0.5f))
      moved = true;
  CHECK(moved);
}

TEST(world_cursor_breeze_scales_with_the_setting) {
  // Measures the draught itself (Particle::breezeX) after a realistic mouse
  // stroke: 15 px per frame (~900 px/s), not a teleport across the screen.
  auto breeze = [](float amount) {
    SimSettings set = Quiet();
    set.snowing = true;
    set.intensity = 1.f;
    set.cursorBreeze = amount;
    World w;
    w.configure(set);
    w.setMonitors(OneMonitor());
    std::vector<WindowInput> none;
    for (int i = 0; i < 120; ++i) w.update(i / 60.0, 1 / 60.f, none);
    float x = 300.f;
    for (int f = 0; f < 20; ++f) {
      x += 15.f;
      w.setCursor(Vec2(x, 300.f), true);
      w.update(2.0 + f / 60.0, 1 / 60.f, none);
    }
    double sum = 0.0;
    int n = 0;
    float peak = 0.f;
    for (const auto& p : w.particles()) {
      if (p.kind != ParticleKind::Flake) continue;
      sum += std::fabs(p.breezeX);
      peak = std::max(peak, std::fabs(p.breezeX));
      ++n;
    }
    return std::pair<double, float>{n > 0 ? sum / n : 0.0, peak};
  };
  const auto off = breeze(0.f);
  const auto soft = breeze(0.30f);
  const auto full = breeze(1.f);
  CHECK(off.first == 0.0 && off.second == 0.f);  // 0 %: the mouse never touches snow
  CHECK(soft.first > 0.0);
  CHECK_MSG(full.first / soft.first >= 1.8 && full.first / soft.first <= 5.0,
            "the slider scales the draught proportionally");
  CHECK_MSG(full.second <= 260.5f, "the draught is clamped, flakes never turn into bullets");
}

TEST(world_pile_height_slider_shrinks_live_snow) {
  World w;
  SimSettings set = Quiet();
  set.initialDusting = 0.9f;
  set.pileMaxHeight = 40.f;
  w.configure(set);
  w.setMonitors(OneMonitor());
  std::vector<WindowInput> wins = {Win(1, RectI(400, 500, 1400, 900), 0)};
  for (int i = 0; i < 30; ++i) w.update(i / 60.0, 1 / 60.f, wins);
  const float tall = w.surface(1)->pile.peak();
  CHECK(tall > 12.f);
  // The user drags "Pile height" down to 10: what is already lying shrinks now.
  set.pileMaxHeight = 10.f;
  w.configure(set);
  CHECK(w.surface(1)->pile.peak() <= 10.f + 1e-3f);
  CHECK(w.surface(1)->pile.peak() < tall);
  // And raising it again allows the drift to grow past the old low cap.
  set.pileMaxHeight = 40.f;
  w.configure(set);
  for (int i = 0; i < 60; ++i) w.update(1.0 + i / 60.0, 1 / 60.f, wins);
  CHECK(w.surface(1)->pile.peak() > 10.5f);
}
