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

// ---- broken snowman: every element flies on its own ------------------------
// The parts are the actual pieces of the artwork (three balls, hat, scarf,
// stick arms, coal buttons, the face with its carrot), so the renderer can draw
// each one with its normal shape and colour while it tumbles.
enum class KnockPart : int { Bottom = 0, Middle, Top, Hat, Scarf, ArmLeft, ArmRight, Buttons, Face, Count };

// How hard each part is thrown relative to the base impulse.
constexpr float kPartImpulse[] = {0.80f, 0.90f, 1.00f, 1.30f, 1.05f, 1.12f, 1.12f, 1.02f, 1.18f};

// Local pivots of the parts in the snowman's unit space (1 = full height,
// y = 0 at the base). Shared with the renderer.
constexpr float kPartPivotY[] = {-0.200f, -0.515f, -0.747f, -0.860f, -0.650f, -0.520f, -0.520f, -0.515f, -0.747f};
constexpr float kPartPivotX[] = {0.f, 0.f, 0.f, 0.090f, 0.f, -0.135f, 0.135f, 0.f, 0.f};

struct PartPose {
  float dx = 0;   // px along the train direction
  float dy = 0;   // px, negative = air
  float rot = 0;  // radians
  bool done = false;
};

// Deterministic, lively but coordinated throw: the same base impulse as the
// body (so the pieces stay together in the air), a per-part multiplier, a
// little seed jitter, gravity, one soft bounce off the edge and an exact return
// to the original spot at kKnockDuration.
// Half height of each part in unit space: the pieces come to rest with their
// own bottom on the surface, not at their original height inside the snowman.
constexpr float kPartHalfHeight[] = {0.200f, 0.150f, 0.112f, 0.070f, 0.028f, 0.030f, 0.030f, 0.018f, 0.070f};

inline float KnockSmooth(float u) {
  u = std::clamp(u, 0.f, 1.f);
  return u * u * (3.f - 2.f * u);
}

// Ballistic throw with a real floor: `dy` is px below the part's rest position
// (negative = in the air, positive = fallen). Every part falls until its own
// bottom touches the surface the snowman stood on (`height` = snowman height in
// px, `embed` = px of bite into the snow), bounces once, lies there, and only
// then eases back to the exact rest pose for the rebuild.
inline PartPose KnockPartAt(double t, int dir, uint32_t seed, KnockPart part, float height = 1.f,
                            float embed = 0.f) {
  PartPose p;
  const int i = static_cast<int>(part);
  if (t <= 0) return p;
  if (t >= kKnockDuration) {
    p.done = true;
    return p;
  }
  const double pi = 3.14159265358979;
  const double u = t / kKnockDuration;
  const double g = 1700.0;
  const double mult = static_cast<double>(kPartImpulse[i]) * KnockRand(seed + 17u * static_cast<uint32_t>(i + 1), 0.90, 1.10);
  const double throwX = 150.0 * mult;
  const double vy0 = -KnockRand(seed + 29u * static_cast<uint32_t>(i + 1), 430.0, 560.0) * mult;

  // Distance from the part's rest height down to its own bottom on the surface.
  const double drop = std::max(0.f, -kPartHalfHeight[i] - kPartPivotY[i]) * static_cast<double>(height) +
                      static_cast<double>(embed);
  const double disc = vy0 * vy0 + 2.0 * g * drop;
  const double tFloor = (-vy0 + std::sqrt(std::max(0.0, disc))) / g;  // first touch-down
  double y;  // px below the rest position
  if (t < tFloor) {
    y = vy0 * t + 0.5 * g * t * t;
    if (y > drop) y = drop;
  } else {
    // One soft bounce off the surface, then it stays on it.
    const double vb = -0.32 * std::fabs(vy0 + g * tFloor);
    const double tb = t - tFloor;
    y = drop + vb * tb + 0.5 * g * tb * tb;
    if (y > drop) y = drop;
  }

  const double holdEnd = 0.78 * kKnockDuration;
  double dxOut;
  if (t < tFloor) {
    dxOut = static_cast<double>(dir) * throwX * std::sin((t / std::max(1e-6, tFloor)) * pi * 0.5);
  } else if (t < holdEnd) {
    dxOut = static_cast<double>(dir) * throwX;
  } else {
    const double k = static_cast<double>(KnockSmooth(static_cast<float>((t - holdEnd) / (kKnockDuration - holdEnd))));
    dxOut = static_cast<double>(dir) * throwX * (1.0 - k);
  }
  double dy = y;
  if (t >= holdEnd) {
    const double k = static_cast<double>(KnockSmooth(static_cast<float>((t - holdEnd) / (kKnockDuration - holdEnd))));
    dy = drop * (1.0 - k);
  }

  p.dx = static_cast<float>(dxOut);
  p.dy = static_cast<float>(dy);
  const double spinDir = (i % 2 == 0) ? 1.0 : -1.0;
  const double spin = KnockRand(seed + 37u * static_cast<uint32_t>(i + 1), 2.2, 4.8) * spinDir * static_cast<double>(dir);
  p.rot = static_cast<float>(spin * std::sin(u * pi) * std::pow(1.0 - u, 0.6));
  return p;
}

}  // namespace snow
