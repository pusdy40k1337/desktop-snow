// The gift train hitting snowmen: geometry + a small ballistic pose. Portable
// and unit-tested; the Windows renderer only applies the result to sprites.
#pragma once
#include <algorithm>
#include <cmath>

namespace snow {

// The train rides along `groundY`; its body spans [frontX - dir*length, frontX]
// (dir = +1 when it drives left to right, frontX is the locomotive's nose).
struct TrainHitBox {
  float frontX = 0;
  float length = 0;
  int dir = 1;
  float groundY = 0;
};

// Vertical tolerance: only snowmen standing on the same edge (taskbar / bottom
// of the monitor) are on the rails. Snowmen on window tops higher up are safe.
constexpr float kTrainHitToleranceY = 34.f;
// A little slack in front of the nose so a fast train cannot tunnel through.
constexpr float kTrainHitPadX = 14.f;

inline bool TrainHitsSnowman(const TrainHitBox& box, float baseX, float baseY) {
  if (std::fabs(baseY - box.groundY) > kTrainHitToleranceY) return false;
  const float a = box.frontX - static_cast<float>(box.dir) * box.length - kTrainHitPadX;
  const float b = box.frontX + kTrainHitPadX;
  const float lo = std::min(a, b), hi = std::max(a, b);
  return baseX >= lo && baseX <= hi;
}

// How long a knocked snowman tumbles before it is back on its feet.
constexpr double kKnockDuration = 2.4;

struct KnockPose {
  float dx = 0;   // px, along the train's direction
  float dy = 0;   // px, negative = in the air
  float rot = 0;  // radians, lean / spin
  bool done = false;
};

// Deterministic ballistic tumble: up, spin, land, settle back exactly on the
// spot. `dir` is the train direction (+1/-1); t is seconds since the hit.
inline KnockPose KnockAt(double t, int dir) {
  KnockPose p;
  if (t <= 0) return p;
  if (t >= kKnockDuration) {
    p.done = true;
    return p;
  }
  const double u = t / kKnockDuration;  // 0..1
  const double pi = 3.14159265358979;
  const double g = 1500.0;
  const double vy0 = -430.0;
  // Pushed along the train, then settles back on its spot.
  p.dx = static_cast<float>(std::sin(u * pi) * 95.0 * static_cast<double>(dir));
  double dy = vy0 * t + 0.5 * g * t * t;
  const double floorT = -2.0 * vy0 / g;  // first landing
  if (t > floorT) dy = 0;                // rests on the ground after landing
  p.dy = static_cast<float>(std::min(0.0, dy));
  const double spin = 6.0 * static_cast<double>(dir);
  // Spin grows fast, then unwinds to zero as the snowman lands.
  p.rot = static_cast<float>(spin * std::sin(u * pi) * (1.0 - u));
  return p;
}

}  // namespace snow
