#include "test.h"

#include "../src/core/pile.h"
#include "../src/core/snowball.h"

using namespace snow;

namespace {
PileModel MakePile(float width, float height) {
  PileParams p;
  p.columnStep = 3.f;
  p.maxHeight = 34.f;
  PileModel pile(width, p);
  for (int i = 0; i < pile.columns(); ++i) {
    // PileModel has no setter; deposit the exact mass column by column.
    pile.deposit(pile.columnCenter(i), height, nullptr);  // kernel sums to ~1 per column
  }
  return pile;
}
}  // namespace

TEST(snowball_grab_needs_real_snow_and_scales_with_height) {
  PileModel empty(300.f, PileParams{});
  CHECK(!SnowballGrabAt(empty, 100.f, 0.f, 1.f));

  PileModel thin = MakePile(300.f, 2.5f);  // below the 4 px threshold
  CHECK(thin.heightAt(100.f) < 4.f);
  CHECK(!SnowballGrabAt(thin, 100.f, -1.f, 1.f));
  CHECK(!SnowballGrabAt(thin, 100.f, 10.f, 1.f));

  PileModel tall = MakePile(300.f, 30.f);
  CHECK(tall.heightAt(100.f) >= 4.f);
  CHECK(SnowballGrabAt(tall, 100.f, 0.f, 1.f));
  CHECK(SnowballGrabAt(tall, 100.f, 10.f, 1.f));
  // A tall pile has a longer reach than a thin one.
  CHECK(SnowballReach(30.f, 1.f) > SnowballReach(8.f, 1.f));
  CHECK(SnowballReach(2.f, 1.f) == 0.f);  // no reach at all below the threshold
  // Very far above the snow: no grab, so window drags still work there.
  CHECK(!SnowballGrabAt(tall, 100.f, 200.f, 1.f));
  // Far outside the edge span: no grab.
  CHECK(!SnowballGrabAt(tall, -200.f, 0.f, 1.f));
  CHECK(!SnowballGrabAt(tall, 900.f, 0.f, 1.f));
}

TEST(snowball_carve_eats_the_pile_along_its_path) {
  PileModel pile = MakePile(300.f, 20.f);
  const float before = pile.mass();
  const float peakBefore = pile.peak();
  CHECK(before > 1000.f);
  const float collected = pile.carveRange(90.f, 120.f);
  CHECK(collected > 0.f);
  CHECK(pile.mass() < before - collected + 1.f);
  // The carved columns are gone, the neighbours keep their snow.
  CHECK(pile.heightAt(105.f) < 0.5f);
  CHECK(pile.heightAt(20.f) > peakBefore * 0.5f);
  // Nothing left in the same span: a second pass collects nothing.
  CHECK(pile.carveRange(90.f, 120.f) == 0.f);
  // A complete sweep empties the whole edge.
  pile.carveRange(0.f, 300.f);
  CHECK(pile.empty());
}

TEST(snowball_radius_grows_with_collected_snow) {
  const float r0 = 6.f;
  CHECK(SnowballRadius(0.f, r0) == r0);
  const float a = SnowballRadius(900.f, r0);
  const float b = SnowballRadius(3600.f, r0);
  CHECK(a > r0);
  CHECK(b > a);
  // Saturating growth: doubling mass never doubles the radius.
  CHECK(b < 2.f * a);
}

TEST(snowball_grows_into_a_big_ball_quickly_and_stops) {
  const float r0 = 5.f;
  CHECK_NEAR(SnowballRadius(0.f, r0), r0, 1e-4);
  // ~1.5 s of rolling (a few hundred height-px scooped) makes a proper ball.
  CHECK(SnowballRadius(800.f, r0) >= r0 * 2.0f);
  CHECK(SnowballRadius(2000.f, r0) <= r0 * kSnowballMaxRadiusFactor + 1e-4f);
  CHECK(SnowballRadius(1e6f, r0) == r0 * kSnowballMaxRadiusFactor);
}

TEST(snowball_can_be_lifted_and_only_stops_at_the_surface) {
  const float top = 1000.f, r = 12.f;
  const float floorY = BallFloorY(top, r);
  CHECK_NEAR(floorY, top - r + kBallEmbed, 1e-4);
  // Lifted above the edge: the ball follows the cursor.
  CHECK_NEAR(BallClampToFloor(600.f, top, r), 600.f, 1e-4);
  // Pushed into the surface: it cannot sink below the edge.
  CHECK_NEAR(BallClampToFloor(1200.f, top, r), floorY, 1e-4);
  // Released in the air it falls under gravity and lands exactly on the edge.
  float y = 600.f, vy = 0.f;
  for (int i = 0; i < 240; ++i) BallFallStep(y, vy, top, r, 1.f / 60.f);
  CHECK_NEAR(y, floorY, 0.01);
  CHECK(vy == 0.f);
}

TEST(snowball_stacks_only_on_a_bigger_ball_below_it) {
  // A ball released right above a bigger one glues on top.
  CHECK(BallCanStack(500.f, 900.f, 10.f, 505.f, 985.f, 22.f));
  // Released beside it (not above) - no stack, it just falls.
  CHECK(!BallCanStack(560.f, 900.f, 10.f, 505.f, 985.f, 22.f));
  // A bigger ball dropped on a smaller one does not use it as a base.
  CHECK(!BallCanStack(500.f, 900.f, 30.f, 505.f, 985.f, 12.f));
}
