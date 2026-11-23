// Snowball rolling: where a press may start a ball (a grab zone that scales with
// the pile height) and how the ball grows from the snow it has collected.
// Portable and unit-tested; the Windows side feeds it piles and the cursor.
#pragma once
#include <algorithm>
#include <cmath>

#include "pile.h"

namespace snow {

// A snow layer thinner than this is not worth scooping.
constexpr float kSnowballMinHeight = 4.f;
// Base slack around the pile, scaled with the pile height: a tall drift is easy
// to catch, a thin dusting only grabs within a few pixels of the edge.
constexpr float kSnowballReachBase = 8.f;
constexpr float kSnowballReachPerHeight = 0.9f;

// Distance from the pile surface (positive = above it) at which a press still
// catches the snow. Grows with the local height, so a tall pile reaches further.
inline float SnowballReach(float pileHeight, float dpiScale) {
  if (pileHeight < kSnowballMinHeight * dpiScale) return 0.f;
  return (kSnowballReachBase + kSnowballReachPerHeight * pileHeight) * dpiScale;
}

// `localX` is px from the left end of the edge, `above`: how far above the pile
// surface the cursor is (negative = inside the snow). A press outside the edge
// span is ignored even when the pile is tall.
inline bool SnowballGrabAt(const PileModel& pile, float localX, float above, float dpiScale) {
  if (localX < -SnowballReach(pile.heightAt(std::clamp(localX, 0.f, pile.width())), dpiScale) ||
      localX > pile.width() + SnowballReach(pile.heightAt(std::clamp(localX, 0.f, pile.width())), dpiScale))
    return false;
  const float h = pile.heightAt(std::clamp(localX, 0.f, pile.width()));
  if (h < kSnowballMinHeight * dpiScale) return false;
  const float reach = SnowballReach(h, dpiScale);
  // From `reach` px above the snow surface down to a few px into the edge.
  return above <= reach && above >= -(4.f * dpiScale);
}

// Ball radius from the snow it has collected (mass = height-px summed over the
// carved columns). Starts at `radius0`, grows quickly - ~1.5-2 s of rolling
// makes a proper big ball - then saturates so it cannot eat the whole drift.
constexpr float kSnowballGrowth = 240.f;
constexpr float kSnowballMaxRadiusFactor = 7.5f;

inline float SnowballRadius(float mass, float radius0) {
  const float r = radius0 * std::sqrt(1.f + std::max(0.f, mass) / kSnowballGrowth);
  return std::min(r, radius0 * kSnowballMaxRadiusFactor);
}

// ---- lifting and dropping a ball ------------------------------------------
// The surface is a floor, not a rail: the ball may be lifted freely and only
// cannot sink below the edge.
constexpr float kBallEmbed = 1.5f;

inline float BallFloorY(float surfaceTopY, float radius, float embed = kBallEmbed) {
  return surfaceTopY - radius + embed;
}

// Smaller y = higher on screen. A ball above the edge keeps the wanted y; one
// pushed into the surface is clamped onto it.
inline float BallClampToFloor(float wantedY, float surfaceTopY, float radius, float embed = kBallEmbed) {
  return std::min(wantedY, BallFloorY(surfaceTopY, radius, embed));
}

// Gravity fall for a released ball until it lands on the surface.
inline void BallFallStep(float& y, float& vy, float surfaceTopY, float radius, float dt, float gravity = 2400.f,
                         float embed = kBallEmbed) {
  const float floorY = BallFloorY(surfaceTopY, radius, embed);
  if (y >= floorY) {
    y = floorY;
    vy = 0.f;
    return;
  }
  vy += gravity * dt;
  y += vy * dt;
  if (y >= floorY) {
    y = floorY;
    vy = 0.f;
  }
}

// A released ball glues onto a host ball when it is above it and close enough
// horizontally; the host must not be smaller (a big ball is the base).
inline bool BallCanStack(float dropX, float dropY, float dropR, float hostX, float hostY, float hostR) {
  return std::fabs(dropX - hostX) <= (dropR + hostR) * 0.60f && dropY <= hostY - hostR * 0.40f &&
         dropR <= hostR * 1.30f;
}

}  // namespace snow
