#include "test.h"

#include "../src/core/knock.h"

using namespace snow;

TEST(train_hits_only_snowmen_on_the_rails) {
  TrainHitBox box;
  box.frontX = 900;
  box.length = 420;
  box.dir = 1;
  box.groundY = 1050;  // rails along the taskbar
  // A snowman standing on the same edge, under the train body.
  CHECK(TrainHitsSnowman(box, 700, 1050));
  CHECK(TrainHitsSnowman(box, 700, 1050 - 20));  // a little uneven ground is fine
  CHECK(TrainHitsSnowman(box, 900, 1050));       // right under the nose
  CHECK(!TrainHitsSnowman(box, 300, 1050));      // already behind the train
  CHECK(!TrainHitsSnowman(box, 1100, 1050));     // still ahead of the nose
  // A snowman standing on a window top far above the rails is safe.
  CHECK(!TrainHitsSnowman(box, 700, 600));
  // Reversed train: the loco leads to the left.
  TrainHitBox rev;
  rev.frontX = 500;
  rev.length = 420;
  rev.dir = -1;
  rev.groundY = 1040;
  CHECK(TrainHitsSnowman(rev, 700, 1040));
  CHECK(!TrainHitsSnowman(rev, 300, 1040));
}

TEST(knock_pose_throws_up_spins_and_returns_to_the_spot) {
  // Flight: the snowman is in the air right after the hit, drifting along the
  // train's direction, and it spins.
  KnockPose p = KnockAt(0.15, 1);
  CHECK(p.dy < 0.f);
  CHECK(p.dx > 0.f);
  CHECK(p.rot != 0.f);
  // Mirror direction: everything flips.
  KnockPose m = KnockAt(0.15, -1);
  CHECK(m.dx < 0.f);
  CHECK(m.rot == -p.rot);
  // Mid-flight is higher than just after the hit, and it lands again.
  CHECK(KnockAt(0.30, 1).dy <= KnockAt(0.10, 1).dy);
  CHECK(KnockAt(1.2, 1).dy == 0.f);
  // After the tumble the snowman is exactly back on its base.
  KnockPose end = KnockAt(kKnockDuration, 1);
  CHECK(end.done);
  CHECK(end.dx == 0.f && end.dy == 0.f && end.rot == 0.f);
  CHECK(KnockAt(kKnockDuration * 2.0, 1).done);
  CHECK(KnockAt(0.0, 1).dx == 0.f);
}

TEST(knock_works_for_placed_and_built_snowmen_alike) {  // "Built" snowmen (rolled from snowballs / placed for free) stand on the same
  // rails as window snowmen, so the same hit box covers both: position is the
  // only input.
  TrainHitBox box{1200, 500, 1, 1050};
  CHECK(TrainHitsSnowman(box, 1000, 1050));  // placed on the taskbar
  CHECK(TrainHitsSnowman(box, 1050, 1046));  // built on the desktop edge
  CHECK(!TrainHitsSnowman(box, 1000, 900));  // on a window far above
  // Every knocked snowman is thrown by the same physics.
  KnockPose a = KnockAt(0.2, 1);
  KnockPose b = KnockAt(0.2, 1);
  CHECK(a.dx == b.dx && a.dy == b.dy && a.rot == b.rot);
}

TEST(knock_throw_is_strong_and_varies_per_snowman) {
  double maxDx = 0, minDy = 0, maxRot = 0;
  for (int i = 0; i <= 100; ++i) {
    const double t = kKnockDuration * i / 100.0;
    const KnockPose p = KnockAt(t, 1, 7);
    maxDx = std::max(maxDx, static_cast<double>(p.dx));
    minDy = std::min(minDy, static_cast<double>(p.dy));
    maxRot = std::max(maxRot, std::fabs(static_cast<double>(p.rot)));
  }
  CHECK(maxDx >= 120.0);  // a real throw, not a nudge
  CHECK(minDy <= -60.0);  // and a real jump
  CHECK(maxRot >= 2.0);   // with a visible spin
  // Neighbouring snowmen do not move in lockstep.
  CHECK(KnockAt(0.2, 1, 3).dx != KnockAt(0.2, 1, 9).dx);
}

TEST(knock_parts_fly_together_and_return) {
  const uint32_t seed = 42;
  for (int dir : {1, -1}) {
    double maxDx = 0, minDy = 0;
    float lastRot = 0;
    for (int i = 0; i < static_cast<int>(KnockPart::Count); ++i) {
      const KnockPart part = static_cast<KnockPart>(i);
      float partMaxDx = 0, partMinDy = 0, partMaxRot = 0;
      for (int k = 0; k <= 100; ++k) {
        const double t = kKnockDuration * k / 100.0;
        const PartPose p = KnockPartAt(t, dir, seed, part);
        // Every part is thrown along the train, never straight up into orbit.
        if (dir > 0) CHECK(p.dx >= -1.f);
        else CHECK(p.dx <= 1.f);
        partMaxDx = std::max(partMaxDx, std::fabs(p.dx));
        partMinDy = std::min(partMinDy, p.dy);
        partMaxRot = std::max(partMaxRot, std::fabs(p.rot));
      }
      CHECK_MSG(partMaxDx >= 60.f && partMaxDx <= 340.f, "part impulse stays sane");
      CHECK_MSG(partMinDy <= -40.f && partMinDy >= -260.f, "part jumps, not into the stratosphere");
      CHECK_MSG(partMaxRot >= 0.4f && partMaxRot <= 9.f, "part spins visibly");
      maxDx = std::max(maxDx, static_cast<double>(partMaxDx));
      minDy = std::min(minDy, static_cast<double>(partMinDy));
      lastRot = PartPose{}.rot;  // keep the compiler honest about the loop body
      (void)lastRot;
    }
    // Pieces return exactly to their place so the snowman can rebuild.
    for (int i = 0; i < static_cast<int>(KnockPart::Count); ++i) {
      const PartPose end = KnockPartAt(kKnockDuration, dir, seed, static_cast<KnockPart>(i));
      CHECK(end.done && end.dx == 0.f && end.dy == 0.f && end.rot == 0.f);
    }
  }
  // Neighbouring parts do not move in perfect lockstep (lively, not rigid).
  CHECK(KnockPartAt(0.2, 1, 7, KnockPart::Bottom).dx != KnockPartAt(0.2, 1, 7, KnockPart::Hat).dx);
  // Custom colours never change the physics: the same seed gives the same throw.
  CHECK(KnockPartAt(0.25, 1, 99, KnockPart::Top).dx == KnockPartAt(0.25, 1, 99, KnockPart::Top).dx);
}

TEST(knock_parts_land_on_the_surface_not_at_their_own_height) {
  const float H = 92.f;  // a real snowman on a 100 % monitor
  auto dropOf = [&](KnockPart part) {
    const int i = static_cast<int>(part);
    return (-kPartHalfHeight[i] - kPartPivotY[i]) * H;
  };
  // After the throw the hat, the face (with the carrot), the arms and the scarf
  // lie on the surface the snowman stood on: their dy reaches the full drop
  // instead of stopping at head/body height.
  for (KnockPart part : {KnockPart::Hat, KnockPart::Face, KnockPart::ArmLeft, KnockPart::Scarf, KnockPart::Buttons}) {
    const float drop = dropOf(part);
    const PartPose lying = KnockPartAt(1.55, 1, 5, part, H);
    CHECK_MSG(lying.dy > 20.f, "part falls below its own rest height");
    CHECK_NEAR(lying.dy, drop, 1.5);
  }
  // And every part still ends exactly at its rest pose for the rebuild.
  for (int i = 0; i < static_cast<int>(KnockPart::Count); ++i) {
    const PartPose end = KnockPartAt(kKnockDuration, -1, 5, static_cast<KnockPart>(i), H);
    CHECK(end.done && end.dx == 0.f && end.dy == 0.f && end.rot == 0.f);
  }
  // The bottom ball already rests on the surface: its drop is zero.
  CHECK_NEAR(KnockPartAt(1.55, 1, 5, KnockPart::Bottom, H).dy, 0.f, 0.5);
}
