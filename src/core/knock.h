// The gift train hitting snowmen: geometry + a small ballistic pose. Portable
// and unit-tested; the Windows renderer only applies the result to sprites.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

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
constexpr double kKnockDuration = 2.6;

struct KnockPose {
  float dx = 0;   // px, along the train's direction
  float dy = 0;   // px, negative = in the air
  float rot = 0;  // radians, lean / spin
  bool done = false;
};

// Deterministic per-object variation so a row of snowmen does not move in
// lockstep: the same seed always gives the same throw.
inline double KnockRand(uint32_t seed, double lo, double hi) {
  uint32_t h = seed * 2654435761u + 0x9E3779B9u;
  h ^= h >> 13;
  h *= 0x85EBCA6Bu;
  h ^= h >> 16;
  const double u = static_cast<double>(h % 1000u) / 999.0;
  return lo + (hi - lo) * u;
}

// Ballistic tumble: a real impulse along the train (a long, flat arc), height
// from a real vertical kick, a fast spin that unwinds, and a smooth return to
// the exact spot on its edge. `dir` is the train direction (+1/-1); t is
// seconds since the hit. `seed` (usually the object id) varies the throw.
inline KnockPose KnockAt(double t, int dir, uint32_t seed = 0) {
  KnockPose p;
  if (t <= 0) return p;
  if (t >= kKnockDuration) {
    p.done = true;
    return p;
  }
  const double u = t / kKnockDuration;  // 0..1
  const double pi = 3.14159265358979;
  const double g = 1700.0;
  const double vy0 = -KnockRand(seed, 470.0, 560.0);
  const double throwX = KnockRand(seed, 135.0, 185.0);
  const double spin = KnockRand(seed, 5.5, 9.0) * static_cast<double>(dir);
  // Pushed along the train, then settles back on its spot.
  p.dx = static_cast<float>(static_cast<double>(dir) * throwX * std::sin(std::pow(u, 0.85) * pi));
  const double dy = vy0 * t + 0.5 * g * t * t;
  const double floorT = -2.0 * vy0 / g;  // first landing
  p.dy = static_cast<float>(std::min(0.0, t > floorT ? 0.0 : dy));
  // Spin grows fast, then unwinds to zero as the snowman lands.
  p.rot = static_cast<float>(spin * std::sin(u * pi) * std::pow(1.0 - u, 0.7));
  return p;
}

}  // namespace snow
