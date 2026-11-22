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
// carved columns). Starts at `radius0`, grows quickly at first, then saturates.
inline float SnowballRadius(float mass, float radius0) {
  return radius0 * std::sqrt(1.f + std::max(0.f, mass) / 900.f);
}

}  // namespace snow
