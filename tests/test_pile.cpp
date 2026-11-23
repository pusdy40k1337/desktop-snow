#include <cmath>
#include <vector>

#include "../src/core/pile.h"
#include "test.h"

using namespace snow;

static PileParams Params() {
  PileParams p;
  p.columnStep = 3;
  p.maxHeight = 30;
  p.maxSlope = 0.9f;
  p.relaxRate = 10;
  p.diffusion = 6;
  return p;
}

TEST(pile_reset_column_count) {
  PileModel m(300, Params());
  CHECK(m.columns() == 100);
  CHECK(m.mass() == 0.f);
  PileModel odd(301, Params());
  CHECK(odd.columns() == 101);
}

TEST(pile_deposit_conserves_mass_and_caps) {
  PileModel m(300, Params());
  float overflow = m.deposit(150, 10);
  CHECK_NEAR(overflow, 0, 1e-5);
  CHECK_NEAR(m.mass(), 10, 1e-4);
  // Fill far beyond the cap: heights never exceed it and the excess is reported.
  float lost = 0;
  for (int i = 0; i < 200; ++i) lost += m.deposit(150, 10);
  CHECK(m.peak() <= 30.f + 1e-4f);
  CHECK_NEAR(m.mass() + lost, 2010, 1e-2);
}

TEST(pile_deposit_respects_exposure_mask) {
  PileModel m(300, Params());
  std::vector<uint8_t> exposed(static_cast<size_t>(m.columns()), 1);
  for (int i = 40; i < 60; ++i) exposed[static_cast<size_t>(i)] = 0;
  m.deposit(150, 10, &exposed);  // column 50: hidden under a higher window
  CHECK_NEAR(m.mass(), 0, 1e-6);
  m.settle(1.0f, 1.0f, &exposed);
  CHECK(m.height(50) == 0.f);
  CHECK(m.height(10) > 0.9f);
}

TEST(pile_relax_limits_slope_and_conserves_mass_inside) {
  PileModel m(300, Params());
  for (int i = 0; i < 20; ++i) m.deposit(150, 20);
  float before = m.mass();
  float spilled = 0;
  for (int i = 0; i < 400; ++i) spilled += m.relax(1.f / 60.f);
  CHECK_NEAR(m.mass() + spilled, before, 0.05);
  float maxDiff = 0;
  for (int i = 0; i + 1 < m.columns(); ++i) maxDiff = std::max(maxDiff, std::fabs(m.height(i) - m.height(i + 1)));
  CHECK_MSG(maxDiff <= 0.9f * 3.f + 0.3f, "slope must settle near the angle of repose");
}

TEST(pile_open_ends_spill) {
  PileModel m(30, Params());  // 10 columns
  m.settle(30, 1, nullptr);   // brick of snow at max height
  float spilled = 0;
  for (int i = 0; i < 600; ++i) spilled += m.relax(1.f / 60.f);
  CHECK(spilled > 1.f);
  CHECK(m.height(0) <= 0.9f * 3.f + 0.5f);  // ends taper
}

TEST(pile_resize_anchors_left_edge) {
  PileModel m(300, Params());
  for (int i = 0; i < m.columns(); ++i) m.deposit(m.columnCenter(i), static_cast<float>(i % 7));
  std::vector<float> before = m.heights();
  // Right edge dragged inward by 60 px: left part stays put, cut part spills.
  float spilled = m.resize(100, 400, 100, 340);
  CHECK(m.columns() == 80);
  for (int i = 0; i < 80; ++i) CHECK_NEAR(m.height(i), before[static_cast<size_t>(i)], 1e-5);
  float cut = 0;
  for (int i = 80; i < 100; ++i) cut += before[static_cast<size_t>(i)];
  CHECK_NEAR(spilled, cut, 1e-3);
}

TEST(pile_resize_anchors_right_edge) {
  PileModel m(300, Params());
  m.deposit(m.columnCenter(99), 10);  // snow near the right end
  std::vector<float> before = m.heights();
  float spilled = m.resize(100, 400, 40, 400);  // left edge dragged outward
  CHECK(m.columns() == 120);
  CHECK_NEAR(spilled, 0, 1e-6);
  for (int k = 0; k < 100; ++k) CHECK_NEAR(m.height(119 - k), before[static_cast<size_t>(99 - k)], 1e-5);
}

TEST(pile_resize_both_edges_resamples_not_resets) {
  PileModel m(300, Params());
  m.settle(12, 1, nullptr);
  float spilled = m.resize(100, 400, 0, 600);  // e.g. restore from snapped
  CHECK(m.columns() == 200);
  CHECK_NEAR(spilled, 0, 1e-6);
  CHECK_NEAR(m.height(100), 12, 1e-4);
  CHECK(m.mass() > 1000.f);  // not reset
}

TEST(pile_scatter_debris_proportional_to_mass) {
  PileModel small(300, Params()), big(300, Params());
  small.settle(4, 1, nullptr);
  big.settle(16, 1, nullptr);
  std::vector<PileDebris> a, b;
  float ra = small.scatter(0.5f, 2.0f, 10000, a);
  float rb = big.scatter(0.5f, 2.0f, 10000, b);
  CHECK_NEAR(ra, 0.5f * 4 * 100, 1e-2);
  CHECK_NEAR(rb, 0.5f * 16 * 100, 1e-2);
  CHECK(!a.empty() && !b.empty());
  float ratio = static_cast<float>(b.size()) / static_cast<float>(a.size());
  CHECK_NEAR(ratio, 4.0, 0.1);
  float sum = 0;
  for (auto& d : b) sum += d.mass;
  CHECK_NEAR(sum, rb, 0.05);
  CHECK_NEAR(big.mass(), 0.5f * 16 * 100, 1e-2);
}

TEST(pile_scatter_cap_keeps_total_mass) {
  PileModel m(300, Params());
  m.settle(20, 1, nullptr);
  std::vector<PileDebris> out;
  float removed = m.scatter(1.0f, 1.0f, 50, out);
  CHECK(out.size() == 50);
  float sum = 0;
  for (auto& d : out) sum += d.mass;
  CHECK_NEAR(sum, removed, 0.05);
  CHECK(m.empty());
}

TEST(pile_remove_range_is_local) {
  PileModel m(300, Params());
  m.settle(10, 1, nullptr);
  std::vector<PileDebris> out;
  m.removeRange(140, 160, 1.0f, 2.f, 100, out);
  CHECK(m.height(50) < 5.f);
  CHECK_NEAR(m.height(10), 10, 1e-4);
  CHECK_NEAR(m.height(90), 10, 1e-4);
}

TEST(pile_corner_radius_caps_ends) {
  PileParams p = Params();
  p.cornerRadius = 12;
  PileModel m(300, p);
  m.settle(30, 1, nullptr);
  CHECK(m.height(0) < 15.f);
  CHECK_NEAR(m.height(50), 30, 1e-4);
}

TEST(pile_dpi_change_resamples_columns) {
  PileModel m(300, Params());
  m.settle(10, 1, nullptr);
  PileParams p = Params();
  p.columnStep = 4.5f;
  p.maxHeight = 45;
  m.setParams(p);
  m.scaleHeights(1.5f);
  CHECK(m.columns() == 67);
  CHECK_NEAR(m.height(30), 15, 1e-3);
}

TEST(pile_version_changes_only_on_shape_change) {
  PileModel m(300, Params());
  uint32_t v0 = m.version();
  m.relax(1.f / 60.f);  // empty: nothing moves
  CHECK(m.version() == v0);
  m.deposit(10, 1);
  CHECK(m.version() != v0);
}

TEST(pile_visual_heights_ease_towards_the_target) {
  std::vector<float> target = {0.f, 10.f, 4.f};
  std::vector<float> visual = {8.f, 0.f, 4.f};
  CHECK(SmoothHeights(visual, target, 0.5f));  // something moved
  CHECK_NEAR(visual[0], 4.0, 1e-4);
  CHECK_NEAR(visual[1], 5.0, 1e-4);
  // Converges without overshooting.
  for (int i = 0; i < 40; ++i) SmoothHeights(visual, target, 0.5f);
  CHECK_NEAR(visual[0], 0.0, 0.05);
  CHECK_NEAR(visual[1], 10.0, 0.05);
  CHECK_NEAR(visual[2], 4.0, 0.05);
  // A resize copies the target at once instead of easing from stale sizes.
  std::vector<float> small = {1.f};
  CHECK(SmoothHeights(small, target, 0.2f));
  CHECK(small.size() == 3 && small[1] == 10.f);
  CHECK(!SmoothHeights(small, target, 0.2f));  // already settled
}
