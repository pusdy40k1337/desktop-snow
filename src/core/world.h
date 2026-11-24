// The portable simulation: falling snow, per-window piles, scatter debris,
// wind gusts, cursor sweeping and sparkles. No Windows dependencies, so it is
// unit-tested on any platform. Coordinates: physical px, virtual screen.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "geom.h"
#include "motion.h"
#include "pile.h"
#include "rng.h"

namespace snow {

struct MonitorInfo {
  std::string id;   // device name
  RectI rect;       // full monitor rect
  RectI work;       // work area (excludes the taskbar)
  float scale = 1;  // DPI / 96
  bool primary = false;
};

struct WindowInput {
  uint64_t id = 0;
  RectI rect;              // visible frame (DWM extended frame bounds), predicted to display time
  int z = 0;               // 0 = topmost
  bool holdsSnow = false;  // eligible for a pile (also requires room above the top edge)
  bool isTaskbar = false;
  float cornerRadius = 0;  // px
  float scale = 1;         // DPI scale of the monitor holding the top edge
  Vec2 velocity;           // px/s, smoothed
  float accel = 0;         // px/s^2
};

enum class ParticleKind : uint8_t { Flake = 0, Debris = 1, Confetti = 2 };

struct Particle {
  float x = 0, y = 0, prevY = 0;
  float vx = 0, vy = 0;
  float size = 3;            // diameter px
  float alpha = 1;
  float life = 0, maxLife = 0;  // maxLife 0 = recycled flake (immortal)
  float phase = 0, sway = 0;
  float r1 = 0, r2 = 0;       // per-particle random factors (size, speed)
  float spin = 0, angle = 0;  // confetti
  float breezeX = 0, breezeY = 0;  // cursor-draught offset, decays over time
  uint16_t monitor = 0;
  uint8_t layer = 1;          // 0 far, 1 mid, 2 near
  uint8_t colorIndex = 0;
  ParticleKind kind = ParticleKind::Flake;
};

struct Sparkle {
  float x = 0, y = 0, size = 4;
  double t0 = 0, dur = 1;
  uint64_t surface = 0;
  float localX = 0;  // re-anchored to the surface every frame (moves with the window)
};

struct Surface {
  uint64_t id = 0;
  RectI rect;
  int z = 0;
  bool isTaskbar = false;
  bool active = false;       // drawn this frame (dormant surfaces keep snow while minimised)
  float scale = 1;
  float cornerRadius = 0;
  PileModel pile;
  ScatterDetector scatter;
  std::vector<uint8_t> exposed;  // per column: pile base not covered by a higher window
  double lastSeen = 0;
  Vec2 velocity;
  float speed = 0;
  float wobble = 0, wobbleVel = 0;  // decoration wobble spring (snowmen)
  float regrowTarget = 0;    // mean height the pile had before it was scattered
  float spillAcc = 0;        // mass slid off the ends, waiting to become particles
  Vec2 lastFastVel;          // velocity while it was last moving fast (sudden-stop jolts)
};

struct SimSettings {
  bool snowing = true;
  bool confetti = false;
  bool piles = true;
  bool taskbarPile = true;
  float density = 45;          // legacy scale 0..100
  float intensity = 1.0f;      // multiplier incl. battery saver
  float windSpeed = 5;         // DIP/s (legacy units)
  float flakeMin = 3, flakeMax = 7;  // DIP
  float fallSpeed = 90;        // DIP/s
  float sway = 35;
  float pileMaxHeight = 34;    // DIP
  float taskbarMaxHeight = 40; // DIP
  float growth = 1.0f;
  float cornerRadiusOverride = -1;  // DIP, -1 = use the window's own radius
  ScatterParams scatter;
  float inherit = 0.55f;
  bool windGusts = true, cursorSweep = true, melt = false, sparkle = true;
  float cursorBreeze = 0.30f;  // 0..1, how strongly the mouse drifts the flakes
  bool reducedMotion = false;
  float initialDusting = 0.22f;  // fraction of cap given to windows present at start
  int maxParticles = 6000;
};

struct WorldStats {
  int surfaces = 0, activeSurfaces = 0, flakes = 0, debris = 0;
  float totalMass = 0;
  int bursts = 0;
};

class World {
 public:
  World();
  void configure(const SimSettings& s);
  const SimSettings& settings() const { return s_; }
  void setMonitors(const std::vector<MonitorInfo>& monitors);
  const std::vector<MonitorInfo>& monitors() const { return monitors_; }
  // Per monitor (same order as setMonitors): 1 = paused (a fullscreen app is
  // showing). Paused monitors spawn no snow and their flakes freeze in place
  // until the monitor resumes.
  void setPausedMonitors(const std::vector<uint8_t>& paused) { paused_ = paused; }
  bool monitorPaused(size_t i) const { return i < paused_.size() && paused_[i] != 0; }
  void setCursor(Vec2 p, bool valid);
  void seed(uint64_t s) { rng_.reseed(s); }

  // Advance to `now` (seconds). `windows` must be in z-order, topmost first.
  void update(double now, float dt, const std::vector<WindowInput>& windows);
  // The window is gone for good: its snow falls off.
  void windowDestroyed(uint64_t id);
  void clearSnow();
  void kick(uint64_t surfaceId, float impulse);
  void puff(Vec2 p, int count, float scale);
  // A rolling snowball scooped the snow in [x0, x1) local px of this edge.
  // Returns the collected mass (height-px); the pile keeps its own relaxation.
  float carvePile(uint64_t surfaceId, float x0, float x1);

  const std::unordered_map<uint64_t, Surface>& surfaces() const { return surfaces_; }
  const Surface* surface(uint64_t id) const;
  // Active surfaces, bottom of the z-order first (draw order).
  const std::vector<const Surface*>& drawOrder() const { return drawOrder_; }
  const std::vector<Particle>& particles() const { return particles_; }
  const std::vector<Sparkle>& sparkles() const { return sparkles_; }
  float wind() const { return windNow_; }
  float gust() const { return gustEnv_; }
  double now() const { return now_; }
  WorldStats stats() const;

  // Absolute y of the pile base at absolute x (window top + rounded-corner drop).
  static float BaseY(const Surface& s, float absX);
  // Absolute y of the pile surface (base minus snow height).
  static float SurfaceY(const Surface& s, float absX);
  static int MonitorIndexAt(const std::vector<MonitorInfo>& mons, float x, float y);

 private:
  PileParams pileParamsFor(const Surface& s) const;
  void syncSurfaces(const std::vector<WindowInput>& windows, float dt);
  void computeExposure(Surface& s, const std::vector<WindowInput>& windows);
  void ensureFlakes();
  void spawnFlake(Particle& p, uint16_t monitor, bool anywhere);
  void spawnDebris(const Surface& s, const std::vector<PileDebris>& debris, Vec2 baseVel, float spread);
  void updateGusts(float dt);
  void updateCursor(float dt);
  void updateParticles(float dt);
  void updateSparkles();
  void updatePiles(float dt);

  SimSettings s_;
  std::vector<MonitorInfo> monitors_;
  std::unordered_map<uint64_t, Surface> surfaces_;
  std::vector<const Surface*> drawOrder_;
  std::vector<Surface*> active_;
  std::vector<Particle> particles_;
  std::vector<Sparkle> sparkles_;
  std::vector<PileDebris> debrisTmp_;
  std::vector<int> flakeTarget_;  // per monitor
  std::vector<uint8_t> paused_;   // per monitor
  Rng rng_;
  double now_ = 0;
  bool firstFrame_ = true;
  // wind gusts
  float windNow_ = 0, gustEnv_ = 0, gustStrength_ = 0, gustDir_ = 1;
  double gustStart_ = 0, gustDur_ = 0, nextGust_ = 30;
  // cursor
  Vec2 cursor_, cursorPrev_;
  bool cursorValid_ = false, cursorPrevValid_ = false;
  Vec2 cursorVel_;
  // sparkles
  double nextSparkle_ = 0;
  int bursts_ = 0;
};

}  // namespace snow
