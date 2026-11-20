#include <cmath>

#include "../src/core/motion.h"
#include "test.h"

using namespace snow;

// Simulates a window moved by an input device at `inputHz`, sampled by the
// overlay once per display frame at `frameHz`.
struct Drag {
  double inputHz, frameHz;
  double pos(double t, double v) const {
    double step = std::floor(t * inputHz) / inputHz;  // last input event before t
    return v * step;
  }
};

TEST(motion_still_window_never_predicts) {
  MotionTracker m;
  for (int i = 0; i < 120; ++i) m.addSample(i / 60.0, Vec2d(100, 200));
  double now = 2.0;
  Vec2d p = m.predict(now + 0.016, now);
  CHECK(p.x == 100 && p.y == 200);
  CHECK(!m.moving(now));
  CHECK(m.speed() == 0);
}

TEST(motion_velocity_estimate_constant_drag) {
  MotionTracker m;
  Drag d{1000, 60};
  double v = 1200;  // px/s
  for (int f = 0; f < 60; ++f) {
    double t = f / d.frameHz;
    m.addSample(t, Vec2d(d.pos(t, v), 50));
  }
  CHECK_NEAR(m.velocity().x, v, 0.05 * v);
  CHECK_NEAR(m.velocity().y, 0, 1);
}

TEST(motion_prediction_reduces_error_for_full_frame_lead) {
  // Sampling at frame start and displaying one frame later: without prediction
  // the error is v * frame; with it the error must be much smaller.
  MotionTracker m;
  Drag d{1000, 60};
  double v = 900, frame = 1.0 / 60;
  double errPred = 0, errNone = 0;
  int n = 0;
  for (int f = 0; f < 120; ++f) {
    double t = f * frame;
    m.addSample(t, Vec2d(d.pos(t, v), 0));
    if (f < 10) continue;
    double target = t + frame;
    double truth = d.pos(target, v);
    errPred += std::fabs(m.predict(target, t).x - truth);
    errNone += std::fabs(m.position().x - truth);
    ++n;
  }
  errPred /= n;
  errNone /= n;
  CHECK_NEAR(errNone, v * frame, 1.5);
  CHECK_MSG(errPred < 0.15 * errNone, "prediction must cancel most of the one-frame lag");
}

TEST(motion_sudden_stop_detected_quickly_with_events) {
  MotionTracker m;
  double frame = 1.0 / 144, v = 1500;
  double t = 0;
  for (int f = 0; f < 72; ++f) {
    t = f * frame;
    m.addSample(t, Vec2d(v * t, 0));
    m.noteInputEvent(t, 0.008);  // 125 Hz mouse
  }
  double stopT = t;
  double stopX = v * t;
  int framesPredicting = 0;
  for (int f = 1; f < 20; ++f) {
    double tt = stopT + f * frame;
    m.addSample(tt, Vec2d(stopX, 0));
    if (std::fabs(m.predict(tt + frame, tt).x - stopX) > 0.5) ++framesPredicting;
  }
  CHECK_MSG(framesPredicting <= 2, "prediction must stop within two frames of the window stopping");
  CHECK(m.speed() == 0);
}

TEST(motion_stepped_prediction_matches_periodic_input) {
  // 144 Hz input steps, 60 Hz display, sampling 2 ms before each composition:
  // the stepped predictor adds a step only when one is due before the target.
  const double inputHz = 144, v = 1800, frame = 1.0 / 60, lead = 0.002;
  MotionTracker lin, stp;
  auto truth = [&](double t) { return v * std::floor(t * inputHz) / inputHz; };
  double eLin = 0, eStp = 0;
  int exactStp = 0, exactLin = 0, n = 0;
  for (int f = 0; f < 240; ++f) {
    double target = 0.5 + f * frame;
    double t = target - lead;
    double lastStep = std::floor(t * inputHz) / inputHz;
    for (MotionTracker* m : {&lin, &stp}) {
      m->addSample(t, Vec2d(truth(t), 0));
      m->noteInputEvent(lastStep, 1.0 / inputHz);
    }
    if (f < 10) continue;
    double want = truth(target);
    double pl = lin.predict(target, t).x, ps = stp.predictStepped(target, t).x;
    eLin += std::fabs(pl - want);
    eStp += std::fabs(ps - want);
    exactLin += std::fabs(pl - want) < 1.0;
    exactStp += std::fabs(ps - want) < 1.0;
    ++n;
  }
  CHECK_MSG(eStp < eLin * 0.5, "stepped prediction should halve the error on periodic input");
  CHECK(exactStp > exactLin);
  CHECK(exactStp >= n * 9 / 10);
}

TEST(scatter_slow_drag_stays_attached) {
  ScatterDetector s;
  int bursts = 0;
  for (int f = 0; f < 600; ++f) {
    ScatterEvent e = s.update(f / 60.0, 1 / 60.0, 400, 1000);
    bursts += e.burst ? 1 : 0;
    CHECK(e.fraction == 0.f);
  }
  CHECK(bursts == 0);
  CHECK(!s.detached());
}

TEST(scatter_fast_motion_detaches_once_with_hysteresis) {
  ScatterDetector s;  // on 900, off 600
  int bursts = 0;
  // Speed hovering around the trigger threshold (850..950) must not flicker.
  for (int f = 0; f < 600; ++f) {
    double speed = 900 + 50 * std::sin(f * 0.7);
    ScatterEvent e = s.update(f / 60.0, 1 / 60.0, speed, 0);
    bursts += e.burst ? 1 : 0;
  }
  CHECK(bursts == 1);
  CHECK(s.detached());
  // Drop below the off-threshold: re-attached.
  s.update(10.1, 1 / 60.0, 500, 0);
  CHECK(!s.detached());
}

TEST(scatter_fraction_grows_with_speed) {
  ScatterDetector a, b;
  ScatterEvent ea = a.update(0, 1 / 60.0, 950, 0);
  ScatterEvent eb = b.update(0, 1 / 60.0, 2500, 0);
  CHECK(ea.burst && eb.burst);
  CHECK(eb.fraction > ea.fraction);
  CHECK(eb.fraction <= 1.f && ea.fraction >= 0.45f);
}

TEST(scatter_jolt_trigger_below_speed_threshold) {
  ScatterDetector s;
  // Moving at 700 px/s (below 900) then a violent stop: acceleration spike.
  for (int f = 0; f < 30; ++f) s.update(f / 60.0, 1 / 60.0, 700, 0);
  ScatterEvent e = s.update(0.6, 1 / 60.0, 0, 60000);
  CHECK(e.burst);
  CHECK(e.fromJolt);
  // Small accelerations at low speed are noise, not jolts.
  ScatterDetector q;
  for (int f = 0; f < 30; ++f) q.update(f / 60.0, 1 / 60.0, 100, 0);
  CHECK(!q.update(0.6, 1 / 60.0, 0, 60000).burst);
}

TEST(scatter_regrow_after_still_delay) {
  ScatterDetector s;
  s.update(0, 1 / 60.0, 2000, 0);  // burst
  CHECK(!s.canGrow(0.1));
  double t = 0.1;
  for (; t < 0.5; t += 1 / 60.0) s.update(t, 1 / 60.0, 0, 0);
  CHECK(!s.canGrow(t));        // still for only ~0.4 s
  for (; t < 2.2; t += 1 / 60.0) s.update(t, 1 / 60.0, 0, 0);
  CHECK(s.canGrow(t));         // still for > 1.5 s
}
