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

TEST(knock_works_for_placed_and_built_snowmen_alike) {
  // "Built" snowmen (rolled from snowballs / placed for free) stand on the same
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
