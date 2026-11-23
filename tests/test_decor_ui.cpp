#include "test.h"

#include "../src/core/decor.h"

using namespace snow;

namespace {
const RectI kWork(0, 0, 1920, 1050);
const RectI kBar(593, 16, 1327, 96);  // default toolbar at 100 %
}  // namespace

TEST(palette_default_position_is_centred_at_the_top) {
  PaletteLayout pl = LayoutPalette(kWork, 1.f);
  CHECK(pl.bar.top == 16);
  CHECK(pl.bar.left == kBar.left);
  CHECK(pl.bar.right == kBar.right);
  CHECK(pl.buttons.size() == static_cast<size_t>(Tool::Count));
}

TEST(palette_drag_clamps_inside_the_monitor) {
  // Dragged far left/up: the bar sticks to the work-area corner.
  PaletteLayout pl = LayoutPalette(kWork, 1.f, -500, -240);
  CHECK(pl.bar.left == kWork.left);
  CHECK(pl.bar.top == kWork.top);
  // Dragged far right/down: never leaves the work area.
  pl = LayoutPalette(kWork, 1.f, 5000, 4000);
  CHECK(pl.bar.right == kWork.right);
  CHECK(pl.bar.bottom == kWork.bottom);
  // A legible position is kept exactly (the drag follows the mouse).
  pl = LayoutPalette(kWork, 1.f, 300, 700);
  CHECK(pl.bar.left == 300);
  CHECK(pl.bar.top == 700);
  // And a bar wider than a tiny monitor still starts at the left edge.
  RectI tiny(0, 0, 400, 300);
  PaletteLayout small = LayoutPalette(tiny, 1.f, 900, 900);
  CHECK(small.bar.left >= tiny.left);
  CHECK(small.bar.top >= tiny.top);
  CHECK(small.bar.right <= tiny.right);
  CHECK(small.bar.bottom <= tiny.bottom);
}

TEST(palette_is_a_solid_ui_block_with_a_6px_margin) {
  PaletteLayout pl = LayoutPalette(kWork, 1.f);
  CHECK(PaletteUiHit(pl, pl.bar.left - 5, pl.bar.top - 5, 6));  // just outside: still UI
  CHECK(!PaletteUiHit(pl, pl.bar.left - 8, pl.bar.top, 6));
  CHECK(!PaletteUiHit(pl, pl.bar.right + 7, pl.bar.top + 20, 6));
  // Frame between the buttons: a hit on the bar but not on a tool.
  Tool t = Tool::Tree;
  CHECK(!PaletteHit(pl, pl.bar.left + 2, pl.bar.top + 2, t));
  CHECK(PaletteHit(pl, pl.buttons[1].rect.left + 2, pl.buttons[1].rect.top + 2, t));
  CHECK(t == Tool::Bauble);
}

TEST(hint_plate_follows_the_toolbar) {
  PaletteLayout top = LayoutPalette(kWork, 1.f);  // default: at the very top
  RectI hint = HintRect(kWork, top.bar, 1.f);
  CHECK(hint.top >= top.bar.bottom);  // the plate sits under the bar
  CHECK(hint.bottom <= kWork.bottom);
  // Dragged to the bottom half: the plate moves above the bar.
  PaletteLayout low = LayoutPalette(kWork, 1.f, 300, 900);
  RectI hintLow = HintRect(kWork, low.bar, 1.f);
  CHECK(hintLow.bottom <= low.bar.top);
  CHECK(hintLow.top >= kWork.top);
  // The plate never leaves the monitor on a very small work area.
  RectI tiny(0, 0, 600, 260);
  PaletteLayout tinyBar = LayoutPalette(tiny, 1.f);
  RectI tinyHint = HintRect(tiny, tinyBar.bar, 1.f);
  CHECK(tinyHint.top >= tiny.top);
  CHECK(tinyHint.bottom <= tiny.bottom);
}

TEST(colour_picker_swallows_near_misses_and_hits_swatches) {
  ColorPickerLayout cp = LayoutColorPicker(1000, 600, kWork, 1.f);
  CHECK(!cp.swatches.empty());
  const RectI& s = cp.swatches[3];
  const int cx = (s.left + s.right) / 2, cy = (s.top + s.bottom) / 2;
  CHECK(ColorPickerHit(cp, cx, cy) == 3);
  CHECK(ColorPickerHit(cp, cx, cy, 3) == 3);
  // A point inside the popup frame but between two swatches: UI, not a colour.
  const int gapX = (cp.swatches[0].right + cp.swatches[1].left) / 2;
  const int gapY = (cp.swatches[0].top + cp.swatches[0].bottom) / 2;
  CHECK(ColorPickerUiHit(cp, gapX, gapY, 6));
  CHECK(ColorPickerHit(cp, cp.bar.left + 1, cp.bar.top + 1, 2) == -1);
  // Well outside: nothing.
  CHECK(!ColorPickerUiHit(cp, cp.bar.left - 30, cp.bar.top - 30, 6));
}

TEST(decor_clear_all_versus_snowmen_only) {
  DecorState d;
  d.trees.push_back(TreeDecor{});
  d.snowmen.push_back(SnowmanDecor{});
  d.free.push_back(FreeDecor{});
  ClearSnowmenOnly(d);
  CHECK(d.snowmen.empty());
  CHECK(d.trees.size() == 1);
  CHECK(d.free.size() == 1);
  d.snowmen.push_back(SnowmanDecor{});
  ClearAllDecorations(d);
  CHECK(d.trees.empty());
  CHECK(d.snowmen.empty());
  CHECK(d.free.empty());
}

TEST(snowman_from_balls_keeps_its_radii) {
  SnowmanDecor sm;
  // Huge bottom, medium middle, small top, as rolled by hand.
  sm.rBottom = 0.26f;
  sm.rMiddle = 0.15f;
  sm.rTop = 0.09f;
  sm.layers = 3;
  const SnowmanParts p = SnowmanProportions(sm);
  CHECK(p.layers == 3);
  CHECK_NEAR(p.rb, 0.26, 1e-4);
  CHECK_NEAR(p.rm, 0.15, 1e-4);
  CHECK_NEAR(p.rh, 0.09, 1e-4);
  // Stacked without gaps: each centre is one ball below the previous one.
  CHECK_NEAR(p.cmY, p.cbY - p.rb - p.rm + 0.22f * std::min(p.rb, p.rm), 1e-4);
  CHECK_NEAR(p.chY, p.cmY - p.rm - p.rh + 0.22f * std::min(p.rm, p.rh), 1e-4);
  CHECK(p.cmY < p.cbY);
  CHECK(p.chY < p.cmY);
  // Two tiers: body + head only, no middle ball.
  SnowmanDecor two = sm;
  two.layers = 2;
  const SnowmanParts q = SnowmanProportions(two);
  CHECK(q.layers == 2);
  CHECK(q.rm == 0.f);
  CHECK(q.chY < q.cbY);
  // Classic snowmen keep the original constants.
  SnowmanDecor classic;
  const SnowmanParts c = SnowmanProportions(classic);
  CHECK_NEAR(c.rb, 0.200, 1e-6);
  CHECK_NEAR(c.cmY, -0.515, 1e-6);
  CHECK_NEAR(c.chY, -0.747, 1e-6);
}
