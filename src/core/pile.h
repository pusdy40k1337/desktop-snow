// 1D snow heightmap that sits on a horizontal edge (window top, taskbar top).
// Units: local x in physical px from the left end of the edge, heights in px.
// "Mass" is height-px summed over columns (height * 1 column).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace snow {

struct PileParams {
  float columnStep = 3.0f;   // px between columns
  float maxHeight = 34.0f;   // hard cap, px
  float maxSlope = 0.9f;     // max |dh| per px of horizontal distance (angle of repose)
  float relaxRate = 10.0f;   // 1/s, how quickly over-steep slopes flow
  float diffusion = 8.0f;    // px^2/s, gentle softening of the silhouette
  float cornerRadius = 0.f;  // window corner radius: snow thins over rounded corners
};

struct PileDebris {
  float localX = 0;   // px from left end
  float height = 0;   // pile height at that column before removal
  float mass = 0;     // mass represented by this debris particle
};

class PileModel {
 public:
  PileModel() = default;
  PileModel(float width, const PileParams& p) { reset(width, p); }

  void reset(float width, const PileParams& p);
  void setParams(const PileParams& p);  // keeps heights (re-caps them)
  const PileParams& params() const { return p_; }

  float width() const { return width_; }
  int columns() const { return static_cast<int>(h_.size()); }
  float step() const { return p_.columnStep; }
  float height(int i) const { return (i >= 0 && i < columns()) ? h_[i] : 0.f; }
  const std::vector<float>& heights() const { return h_; }
  float columnCenter(int i) const { return (i + 0.5f) * p_.columnStep; }
  int columnAt(float localX) const;          // clamped index
  float heightAt(float localX) const;        // linear interpolation, 0 outside
  float capAt(int i) const;                  // max height allowed at column i
  float mass() const;
  float peak() const;
  bool empty() const { return peak() < 0.25f; }
  uint32_t version() const { return version_; }  // bumps whenever the shape changes

  // Adds `mass` around localX (small kernel). Returns mass that did not fit.
  float deposit(float localX, float mass, const std::vector<uint8_t>* exposed = nullptr);
  // Adds `heightPerSecond * dt` to every exposed column (ambient settling).
  float settle(float heightPerSecond, float dt, const std::vector<uint8_t>* exposed = nullptr);
  // Mass-conserving slope relaxation + diffusion. Open ends: mass flowing past
  // either end leaves the pile and is returned (caller spawns falling particles).
  float relax(float dt);
  // Re-maps the profile for a new edge span [newLeft,newRight] (absolute px).
  // Left edge unchanged -> anchored left; right unchanged -> anchored right;
  // otherwise proportional resample. Returns the mass that no longer fits.
  float resize(float oldLeft, float oldRight, float newLeft, float newRight);
  // Uniform height scale (DPI change).
  void scaleHeights(float s);
  // Removes `fraction` of the snow everywhere. Debris count is proportional to
  // removed mass (one particle per `massPerParticle`, at most maxDebris; when
  // capped, each particle carries proportionally more mass). Returns removed mass.
  float scatter(float fraction, float massPerParticle, int maxDebris, std::vector<PileDebris>& out);
  // Same, restricted to [x0, x1) local px, fraction can taper towards the range ends.
  float removeRange(float x0, float x1, float fraction, float massPerParticle, int maxDebris,
                    std::vector<PileDebris>& out);
  // Slow uniform melting, px per second.
  void melt(float pxPerSecond, float dt);
  // Snowball rolling: takes ALL snow in [x0, x1) local px (a ball scoops the
  // snow it rolls over). Returns the collected mass (height-px summed).
  float carveRange(float x0, float x1);
  void clear();

 private:
  void recap();
  PileParams p_;
  float width_ = 0;
  std::vector<float> h_;
  std::vector<float> tmp_;
  uint32_t version_ = 0;
};

// The renderer shows a smoothed copy of the heightmap: the simulation keeps its
// exact target heights (mass, snowballs, melt), while `visual` approaches them
// with an exponential ease so a mouse stroke never makes the drift jump.
// Returns true when anything moved (the caller then rebuilds the geometry).
inline bool SmoothHeights(std::vector<float>& visual, const std::vector<float>& target, float k) {
  if (visual.size() != target.size()) {
    visual = target;
    return true;
  }
  bool changed = false;
  for (size_t i = 0; i < target.size(); ++i) {
    const float d = target[i] - visual[i];
    if (std::fabs(d) < 0.02f) {
      if (visual[i] != target[i]) {
        visual[i] = target[i];
        changed = true;
      }
      continue;
    }
    visual[i] += d * k;
    changed = true;
  }
  return changed;
}

}  // namespace snow
