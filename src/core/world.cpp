#include "world.h"

#include <algorithm>
#include <cmath>

namespace snow {

namespace {
struct LayerSpec {
  float size, speed, alpha, sway, wind;
};
constexpr LayerSpec kLayers[3] = {
    {0.55f, 0.55f, 0.45f, 0.6f, 0.6f},  // far
    {0.80f, 0.80f, 0.72f, 0.8f, 0.8f},  // mid
    {1.15f, 1.10f, 0.95f, 1.0f, 1.0f},  // near
};
constexpr float kDebrisGravity = 1500.f;  // DIP/s^2
}  // namespace

World::World() : rng_(0x5eed1234ULL) {}

void World::configure(const SimSettings& s) {
  bool pileChange = s.pileMaxHeight != s_.pileMaxHeight || s.taskbarMaxHeight != s_.taskbarMaxHeight ||
                    s.cornerRadiusOverride != s_.cornerRadiusOverride;
  bool kindChange = s.confetti != s_.confetti || s.snowing != s_.snowing;
  s_ = s;
  for (auto& [id, surf] : surfaces_) {
    surf.scatter.setParams(s_.scatter);
    if (pileChange) surf.pile.setParams(pileParamsFor(surf));
  }
  if (kindChange) {
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.kind != ParticleKind::Debris; }),
                     particles_.end());
  }
  if (!s_.piles) {
    for (auto& [id, surf] : surfaces_) surf.pile.clear();
  }
}

void World::setMonitors(const std::vector<MonitorInfo>& monitors) {
  bool same = monitors.size() == monitors_.size();
  for (size_t i = 0; same && i < monitors.size(); ++i)
    same = monitors[i].rect == monitors_[i].rect && monitors[i].scale == monitors_[i].scale;
  monitors_ = monitors;
  if (!same) {
    // Flakes belong to a monitor; regenerate them for the new layout.
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.kind != ParticleKind::Debris; }),
                     particles_.end());
  }
}

void World::setCursor(Vec2 p, bool valid) {
  cursorPrev_ = cursor_;
  cursorPrevValid_ = cursorValid_;
  cursor_ = p;
  cursorValid_ = valid;
}

const Surface* World::surface(uint64_t id) const {
  auto it = surfaces_.find(id);
  return it == surfaces_.end() ? nullptr : &it->second;
}

float World::BaseY(const Surface& s, float absX) {
  float dx = std::min(absX - static_cast<float>(s.rect.left), static_cast<float>(s.rect.right) - absX);
  return static_cast<float>(s.rect.top) + cornerDrop(dx, s.cornerRadius);
}

float World::SurfaceY(const Surface& s, float absX) {
  return BaseY(s, absX) - s.pile.heightAt(absX - static_cast<float>(s.rect.left));
}

int World::MonitorIndexAt(const std::vector<MonitorInfo>& mons, float x, float y) {
  for (size_t i = 0; i < mons.size(); ++i)
    if (mons[i].rect.containsF(x, y)) return static_cast<int>(i);
  return -1;
}

PileParams World::pileParamsFor(const Surface& s) const {
  PileParams p;
  p.columnStep = std::max(2.f, std::round(3.f * s.scale));
  p.maxHeight = (s.isTaskbar ? s_.taskbarMaxHeight : s_.pileMaxHeight) * s.scale;
  p.maxSlope = 0.9f;
  p.relaxRate = 10.f;
  p.diffusion = 6.f * s.scale * s.scale;
  p.cornerRadius = s.cornerRadius;
  return p;
}

void World::computeExposure(Surface& s, const std::vector<WindowInput>& windows) {
  int n = s.pile.columns();
  s.exposed.assign(static_cast<size_t>(n), 1);
  int y = s.rect.top - 1;  // the row the snow rests on
  float step = s.pile.step();
  for (const auto& w : windows) {
    if (w.z >= s.z || w.id == s.id) continue;
    if (y < w.rect.top || y >= w.rect.bottom) continue;
    if (w.rect.right <= s.rect.left || w.rect.left >= s.rect.right) continue;
    for (int i = 0; i < n; ++i) {
      float cx = static_cast<float>(s.rect.left) + (i + 0.5f) * step;
      if (cx >= w.rect.left && cx < w.rect.right) s.exposed[static_cast<size_t>(i)] = 0;
    }
  }
  // Columns with no monitor right above the edge (window top at a screen edge) cannot hold snow.
  if (!monitors_.empty()) {
    for (int i = 0; i < n; ++i) {
      float cx = static_cast<float>(s.rect.left) + (i + 0.5f) * step;
      if (MonitorIndexAt(monitors_, cx, static_cast<float>(y)) < 0) s.exposed[static_cast<size_t>(i)] = 0;
    }
  }
}

void World::spawnDebris(const Surface& s, const std::vector<PileDebris>& debris, Vec2 baseVel, float spread) {
  float scale = s.scale;
  float mpp = 2.2f * scale * scale;
  for (const auto& d : debris) {
    if (static_cast<int>(particles_.size()) >= s_.maxParticles) break;
    Particle p;
    p.kind = ParticleKind::Debris;
    p.x = static_cast<float>(s.rect.left) + d.localX + rng_.range(-0.5f, 0.5f) * s.pile.step();
    p.y = BaseY(s, p.x) - d.height * rng_.range(0.15f, 1.0f);
    p.prevY = p.y;
    float k = std::sqrt(std::max(0.2f, d.mass / mpp));
    p.size = std::min(10.f * scale, (1.8f + 1.8f * rng_.uniform()) * scale * k);
    if (s_.reducedMotion) {
      p.vx = rng_.range(-15.f, 15.f) * scale;
      p.vy = rng_.range(10.f, 40.f) * scale;
      p.maxLife = rng_.range(0.4f, 0.7f);
    } else {
      p.vx = baseVel.x + rng_.range(-1.f, 1.f) * spread;
      p.vy = baseVel.y * 0.6f + rng_.range(-1.f, 0.25f) * spread - 60.f * scale;
      p.maxLife = rng_.range(1.1f, 2.0f);
    }
    p.life = p.maxLife;
    p.alpha = 0.95f;
    p.layer = 2;
    p.monitor = 0;
    particles_.push_back(p);
  }
}

void World::syncSurfaces(const std::vector<WindowInput>& windows, float dt) {
  active_.clear();
  for (auto& [id, s] : surfaces_) s.active = false;

  for (const auto& w : windows) {
    bool eligible = s_.piles && (w.isTaskbar ? s_.taskbarPile : w.holdsSnow) && w.rect.width() >= 40;
    if (!eligible) continue;
    float radius = w.isTaskbar ? 0.f : (s_.cornerRadiusOverride >= 0 ? s_.cornerRadiusOverride * w.scale : w.cornerRadius);
    auto it = surfaces_.find(w.id);
    if (it == surfaces_.end()) {
      Surface s;
      s.id = w.id;
      s.rect = w.rect;
      s.z = w.z;
      s.isTaskbar = w.isTaskbar;
      s.scale = w.scale;
      s.cornerRadius = radius;
      s.pile.reset(static_cast<float>(w.rect.width()), pileParamsFor(s));
      s.scatter.setParams(s_.scatter);
      s.lastSeen = now_;
      if (firstFrame_ && s_.initialDusting > 0.f) {
        // Windows that exist when the app starts get a light dusting at once.
        for (int i = 0; i < s.pile.columns(); ++i) {
          float cap = s.pile.capAt(i);
          float target = cap * s_.initialDusting * (0.75f + 0.5f * rng_.uniform());
          s.pile.deposit(s.pile.columnCenter(i), target);
        }
        for (int k = 0; k < 4; ++k) s.pile.relax(0.05f);
      }
      it = surfaces_.emplace(w.id, std::move(s)).first;
    }
    Surface& s = it->second;
    if (std::fabs(w.scale - s.scale) > 0.01f) {
      float ratio = w.scale / s.scale;
      s.scale = w.scale;
      s.pile.setParams(pileParamsFor(s));
      s.pile.scaleHeights(ratio);
    }
    if (std::fabs(radius - s.cornerRadius) > 0.01f) {
      s.cornerRadius = radius;
      s.pile.setParams(pileParamsFor(s));
    }
    if (w.rect.width() != s.rect.width()) {
      float spilled = s.pile.resize(static_cast<float>(s.rect.left), static_cast<float>(s.rect.right),
                                    static_cast<float>(w.rect.left), static_cast<float>(w.rect.right));
      s.spillAcc += spilled;
    }
    s.rect = w.rect;
    s.z = w.z;
    s.active = true;
    s.lastSeen = now_;
    s.velocity = w.velocity;
    s.speed = w.velocity.length();
    if (s.speed > static_cast<float>(s_.scatter.joltMinSpeed)) s.lastFastVel = w.velocity;

    computeExposure(s, windows);

    float meanBefore = s.pile.mass() / static_cast<float>(std::max(1, s.pile.columns()));
    ScatterEvent ev = s.scatter.update(now_, dt, s.speed, w.accel);
    if (ev.fraction > 0.f && !s.pile.empty()) {
      debrisTmp_.clear();
      float mpp = 2.2f * s.scale * s.scale;
      int maxD = ev.burst ? 420 : 60;
      s.pile.scatter(ev.fraction, mpp, maxD, debrisTmp_);
      Vec2 vel = ev.fromJolt ? s.lastFastVel : s.velocity;
      Vec2 baseVel = vel * s_.inherit;
      float spread = (80.f + 0.12f * vel.length()) * s.scale;
      spawnDebris(s, debrisTmp_, baseVel, spread);
      if (ev.burst) {
        s.regrowTarget = std::max(s.regrowTarget, meanBefore);
        float dir = vel.x >= 0 ? -1.f : 1.f;
        s.wobbleVel += dir * std::min(6.f, 1.5f + vel.length() / 600.f);
        ++bursts_;
      }
    }
    active_.push_back(&s);
  }

  // Unseen surfaces stay dormant (minimised / other desktop) and keep their snow.
  for (auto it = surfaces_.begin(); it != surfaces_.end();) {
    if (!it->second.active && now_ - it->second.lastSeen > 600.0) it = surfaces_.erase(it);
    else ++it;
  }

  drawOrder_.clear();
  for (Surface* s : active_) drawOrder_.push_back(s);
  std::sort(drawOrder_.begin(), drawOrder_.end(), [](const Surface* a, const Surface* b) { return a->z > b->z; });
}

void World::windowDestroyed(uint64_t id) {
  auto it = surfaces_.find(id);
  if (it == surfaces_.end()) return;
  Surface& s = it->second;
  if (s.active && !s.pile.empty()) {
    debrisTmp_.clear();
    s.pile.scatter(1.f, 2.2f * s.scale * s.scale, 300, debrisTmp_);
    spawnDebris(s, debrisTmp_, Vec2(0, 0), 40.f * s.scale);
  }
  surfaces_.erase(it);
}

void World::clearSnow() {
  for (auto& [id, s] : surfaces_) {
    s.pile.clear();
    s.regrowTarget = 0;
    s.spillAcc = 0;
  }
  particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                  [](const Particle& p) { return p.kind == ParticleKind::Debris; }),
                   particles_.end());
  sparkles_.clear();
}

void World::kick(uint64_t surfaceId, float impulse) {
  auto it = surfaces_.find(surfaceId);
  if (it != surfaces_.end()) it->second.wobbleVel += impulse;
}

float World::carvePile(uint64_t surfaceId, float x0, float x1) {
  auto it = surfaces_.find(surfaceId);
  if (it == surfaces_.end()) return 0.f;
  return it->second.pile.carveRange(x0, x1);
}

void World::puff(Vec2 at, int count, float scale) {
  for (int i = 0; i < count && static_cast<int>(particles_.size()) < s_.maxParticles; ++i) {
    Particle p;
    p.kind = ParticleKind::Debris;
    p.x = at.x + rng_.range(-10.f, 10.f) * scale;
    p.y = at.y - rng_.range(0.f, 6.f) * scale;
    p.prevY = p.y;
    p.vx = rng_.range(-140.f, 140.f) * scale;
    p.vy = rng_.range(-260.f, -60.f) * scale;
    p.size = rng_.range(2.f, 4.5f) * scale;
    p.maxLife = p.life = rng_.range(0.7f, 1.3f);
    p.alpha = 0.9f;
    p.layer = 2;
    particles_.push_back(p);
  }
}

void World::spawnFlake(Particle& p, uint16_t monitor, bool anywhere) {
  const MonitorInfo& m = monitors_[monitor];
  p = Particle();
  p.kind = s_.confetti ? ParticleKind::Confetti : ParticleKind::Flake;
  p.monitor = monitor;
  float r = rng_.uniform();
  p.layer = r < 0.40f ? 0 : (r < 0.77f ? 1 : 2);
  p.r1 = rng_.uniform();
  p.r2 = rng_.uniform();
  p.phase = rng_.range(0.f, 2.f * kPi);
  p.sway = rng_.range(0.7f, 1.5f);  // sway angular frequency, rad/s
  p.x = static_cast<float>(m.rect.left) + rng_.uniform() * static_cast<float>(m.rect.width());
  p.y = anywhere ? static_cast<float>(m.rect.top) + rng_.uniform() * static_cast<float>(m.rect.height())
                 : static_cast<float>(m.rect.top) - rng_.range(4.f, 40.f) * m.scale;
  p.prevY = p.y;
  p.alpha = kLayers[p.layer].alpha;
  p.colorIndex = static_cast<uint8_t>(rng_.rangeInt(0, 5));
  p.spin = rng_.range(-4.f, 4.f);
  p.angle = rng_.range(0.f, 2.f * kPi);
}

void World::ensureFlakes() {
  size_t mcount = monitors_.size();
  flakeTarget_.assign(mcount, 0);
  if (s_.snowing || s_.confetti) {
    for (size_t i = 0; i < mcount; ++i) {
      const MonitorInfo& m = monitors_[i];
      float areaDip = (m.rect.width() / m.scale) * (m.rect.height() / m.scale);
      float count = areaDip / 9000.f * (s_.density / 45.f) * s_.intensity;
      if (s_.reducedMotion) count *= 0.5f;
      if (s_.confetti) count *= 0.6f;
      flakeTarget_[i] = std::max(0, static_cast<int>(count));
    }
    int total = 0;
    for (int c : flakeTarget_) total += c;
    int cap = s_.maxParticles * 7 / 10;
    if (total > cap && total > 0)
      for (int& c : flakeTarget_) c = c * cap / total;
  }
  std::vector<int> have(mcount, 0);
  for (auto& p : particles_) {
    if (p.kind == ParticleKind::Debris) continue;
    if (p.monitor >= mcount) {
      p.maxLife = -1;  // mark dead
      continue;
    }
    if (monitorPaused(p.monitor)) continue;  // frozen, kept for the resume
    if (have[p.monitor] >= flakeTarget_[p.monitor]) {
      p.maxLife = -1;
      continue;
    }
    ++have[p.monitor];
  }
  particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                  [](const Particle& p) { return p.maxLife < 0; }),
                   particles_.end());
  for (size_t i = 0; i < mcount; ++i) {
    if (monitorPaused(i)) continue;
    // Grow gradually (no sudden wall of snow), except on the very first frame.
    int missing = flakeTarget_[i] - have[i];
    int add = firstFrame_ ? missing : std::min(missing, std::max(4, flakeTarget_[i] / 40));
    for (int k = 0; k < add; ++k) {
      Particle p;
      spawnFlake(p, static_cast<uint16_t>(i), firstFrame_);
      particles_.push_back(p);
    }
  }
}

void World::updateGusts(float dt) {
  (void)dt;
  float base = s_.windSpeed;
  if (!s_.windGusts || s_.reducedMotion) {
    gustEnv_ = 0;
    gustDur_ = 0;
    windNow_ = base;
    return;
  }
  if (gustDur_ <= 0 && now_ >= nextGust_) {
    gustStart_ = now_;
    gustDur_ = rng_.range(3.f, 6.f);
    gustStrength_ = rng_.range(40.f, 110.f);
    gustDir_ = std::fabs(base) > 2.f ? (base > 0 ? 1.f : -1.f) : (rng_.chance(0.5f) ? 1.f : -1.f);
  }
  if (gustDur_ > 0) {
    double u = (now_ - gustStart_) / gustDur_;
    if (u >= 1.0) {
      gustDur_ = 0;
      gustEnv_ = 0;
      nextGust_ = now_ + rng_.range(25.f, 70.f);
    } else {
      float sv = std::sin(kPi * static_cast<float>(u));
      gustEnv_ = sv * sv;
    }
  }
  windNow_ = base + gustDir_ * gustStrength_ * gustEnv_;

  if (gustEnv_ > 0.35f) {
    for (Surface* s : active_) {
      if (s->isTaskbar || s->pile.empty()) continue;
      float w = s->pile.width();
      float x0 = gustDir_ > 0 ? w * 0.88f : 0.f;
      float x1 = gustDir_ > 0 ? w : w * 0.12f;
      debrisTmp_.clear();
      s->pile.removeRange(x0, x1, 0.5f * gustEnv_ * dt, 2.2f * s->scale * s->scale, 12, debrisTmp_);
      if (!debrisTmp_.empty())
        spawnDebris(*s, debrisTmp_, Vec2(gustDir_ * rng_.range(60.f, 160.f) * s->scale, -40.f * s->scale),
                    30.f * s->scale);
    }
  }
}

void World::updateCursor(float dt) {
  if (!cursorValid_ || !cursorPrevValid_ || dt <= 0.f) {
    cursorVel_ = Vec2();
    return;
  }
  Vec2 v = (cursor_ - cursorPrev_) * (1.f / dt);
  cursorVel_ = v;
  const float speed = v.length();

  // A fast cursor is a breeze for the falling flakes: they drift away from the
  // pointer instead of passing straight through it. Works even with reduced
  // motion (weaker), because the user explicitly expects the reaction.
  if (speed > 25.f && !particles_.empty()) {
    const float R = 125.f;
    const float R2 = R * R;
    const float strength = std::min(1.f, (speed - 60.f) / 900.f) * (s_.reducedMotion ? 0.45f : 1.f);
    for (auto& p : particles_) {
      if (p.kind != ParticleKind::Flake) continue;
      const float dx = p.x - cursor_.x, dy = p.y - cursor_.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 > R2 || d2 < 1e-3f) continue;
      const float falloff = 1.f - std::sqrt(d2) / R;
      const float f = falloff * falloff * strength;
      // The user-facing amount scales the whole draught; 0 % means the mouse
      // never touches the falling snow at all.
      const float amount = s_.cursorBreeze;
      if (amount <= 0.001f) continue;
      // Gentle velocity kick per frame (about a third of the old force at
      // 100 %), clamped so a flick can never shoot a flake off the screen.
      const float bx = (v.x * 0.11f + dx * 0.70f - dy * 0.20f) * f * amount;
      const float by = (v.y * 0.11f + dy * 0.70f + dx * 0.20f) * f * amount;
      p.breezeX = clampf(p.breezeX + bx, -260.f, 260.f);
      p.breezeY = clampf(p.breezeY + by, -220.f, 220.f);
    }
  }
  if (s_.reducedMotion) return;

  if (!s_.cursorSweep) return;
  // Any movement clears snow: a slow stroke brushes it away smoothly, a fast
  // flick also throws particles. Only the debris has a speed gate.
  const float move = (cursor_ - cursorPrev_).length();
  if (move < 0.5f) return;
  int budget = 12;
  for (Surface* s : active_) {
    if (budget <= 0) break;
    float scale = s->scale;
    float lx = cursor_.x - static_cast<float>(s->rect.left);
    if (lx < 0 || lx > s->pile.width()) continue;
    float h = s->pile.heightAt(lx);
    if (h < 1.f) continue;
    float base = BaseY(*s, cursor_.x);
    if (cursor_.y < base - h - 10.f * scale || cursor_.y > base + 4.f * scale) continue;
    int col = s->pile.columnAt(lx);
    if (col < static_cast<int>(s->exposed.size()) && !s->exposed[static_cast<size_t>(col)]) continue;
    float r = 18.f * scale;
    debrisTmp_.clear();
    const bool fast = speed > 320.f * scale;
    const float frac = fast ? clampf(speed / 1500.f, 0.20f, 0.8f) : 0.18f;
    // Slow strokes: the snow is just pressed away (no particles, no bursts).
    const int maxDebris = fast ? std::min(8, budget) : 0;
    s->pile.removeRange(lx - r, lx + r, frac, 2.2f * scale * scale, maxDebris, debrisTmp_);
    if (fast && !debrisTmp_.empty()) {
      budget -= static_cast<int>(debrisTmp_.size());
      spawnDebris(*s, debrisTmp_, v * 0.35f + Vec2(0, -80.f * scale), 60.f * scale);
    }
  }
}

void World::updatePiles(float dt) {
  for (Surface* s : active_) {
    float spilled = s->pile.relax(dt);
    s->spillAcc += spilled;
    float mpp = 2.2f * s->scale * s->scale;
    if (s->spillAcc >= mpp) {
      // Snow sliding off the ends of the edge falls down as small clumps.
      int count = std::min(6, static_cast<int>(s->spillAcc / mpp));
      s->spillAcc -= count * mpp;
      debrisTmp_.clear();
      for (int k = 0; k < count; ++k) {
        bool left = rng_.chance(0.5f);
        debrisTmp_.push_back({left ? s->pile.step() * 0.5f : s->pile.width() - s->pile.step() * 0.5f,
                              s->pile.step(), mpp});
      }
      spawnDebris(*s, debrisTmp_, Vec2(0, 30.f * s->scale), 25.f * s->scale);
    }
    if (s->spillAcc > 50.f * mpp) s->spillAcc = 0;

    if ((s_.snowing) && s->scatter.canGrow(now_)) {
      float cap = s->pile.params().maxHeight;
      float rate = 0.035f * s->scale * s_.growth * s_.intensity;
      float mean = s->pile.mass() / static_cast<float>(std::max(1, s->pile.columns()));
      float target = std::max(s->regrowTarget, 0.3f * cap);
      float boost = 1.f;
      if (s->regrowTarget > 0.f) {
        if (mean < target) boost = 1.f + 6.f * (1.f - mean / target);
        else s->regrowTarget = 0.f;
      }
      s->pile.settle(rate * boost, dt, &s->exposed);
    }
    if (s_.melt) s->pile.melt(0.02f * s->scale, dt);

    // Snowman wobble: damped spring.
    float k = 60.f, c = 7.f;
    s->wobbleVel += (-k * s->wobble - c * s->wobbleVel) * dt;
    s->wobble += s->wobbleVel * dt;
    s->wobble = clampf(s->wobble, -0.6f, 0.6f);
  }
}

void World::updateParticles(float dt) {
  float bottomAll = -1e9f;
  for (const auto& m : monitors_) bottomAll = std::max(bottomAll, static_cast<float>(m.rect.bottom));
  const float t = static_cast<float>(now_);
  for (auto& p : particles_) {
    if (p.kind == ParticleKind::Debris) {
      p.life -= dt;
      if (p.life <= 0.f) {
        p.maxLife = -1;
        continue;
      }
      int mi = MonitorIndexAt(monitors_, p.x, p.y);
      float scale = mi >= 0 ? monitors_[static_cast<size_t>(mi)].scale : 1.f;
      p.vy += kDebrisGravity * scale * dt;
      float drag = std::max(0.f, 1.f - 0.9f * dt);
      p.vx *= drag;
      p.x += p.vx * dt;
      p.prevY = p.y;
      p.y += p.vy * dt;
      float lifeFrac = p.life / p.maxLife;
      p.alpha = 0.95f * std::min(1.f, lifeFrac / 0.4f);
      if (p.y > bottomAll + 60.f) p.maxLife = -1;
      continue;
    }
    if (p.monitor >= monitors_.size()) {
      p.maxLife = -1;
      continue;
    }
    if (monitorPaused(p.monitor)) continue;
    const MonitorInfo& m = monitors_[p.monitor];
    const LayerSpec& L = kLayers[p.layer];
    float scale = m.scale;
    float speedMul = s_.reducedMotion ? 0.6f : 1.f;
    float swayAmp = s_.reducedMotion ? 0.f : s_.sway * 0.6f * L.sway * scale;
    float vy = s_.fallSpeed * L.speed * (0.75f + 0.5f * p.r2) * scale * speedMul;
    if (p.kind == ParticleKind::Confetti) vy *= 0.8f;
    float vx = windNow_ * L.wind * scale + std::sin(t * p.sway + p.phase) * swayAmp;
    // A cursor draught is a persistent offset on top of the wind, fading out.
    const float breezeDecay = std::exp(-2.2f * dt);
    p.breezeX *= breezeDecay;
    p.breezeY *= breezeDecay;
    p.vx = vx + p.breezeX;
    p.vy = vy + p.breezeY;
    p.size = lerpf(s_.flakeMin, s_.flakeMax, p.r1) * L.size * scale;
    p.prevY = p.y;
    // Move with the full velocity: the cursor draught (breezeX/Y) must actually
    // push the flake, not just sit in the velocity field.
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    if (p.kind == ParticleKind::Confetti) p.angle += p.spin * dt;

    if (p.kind == ParticleKind::Flake && s_.piles && p.layer >= 1) {
      bool landed = false;
      for (Surface* s : active_) {
        if (p.x < s->rect.left || p.x >= s->rect.right) continue;
        float sy = SurfaceY(*s, p.x);
        if (p.prevY <= sy && p.y >= sy) {
          int col = s->pile.columnAt(p.x - static_cast<float>(s->rect.left));
          if (col < static_cast<int>(s->exposed.size()) && !s->exposed[static_cast<size_t>(col)]) continue;
          if (s->scatter.detached()) continue;  // a flying pile does not catch flakes
          float sizeDip = p.size / scale;
          s->pile.deposit(p.x - static_cast<float>(s->rect.left), 0.18f * sizeDip * sizeDip * scale * s_.growth,
                          &s->exposed);
          landed = true;
          break;
        }
      }
      if (landed) {
        spawnFlake(p, p.monitor, false);
        continue;
      }
    }
    if (p.y > static_cast<float>(m.rect.bottom) + 20.f * scale) {
      spawnFlake(p, p.monitor, false);
      continue;
    }
    float left = static_cast<float>(m.rect.left), right = static_cast<float>(m.rect.right);
    if (p.x < left - 30.f * scale) p.x = right + 20.f * scale;
    else if (p.x > right + 30.f * scale) p.x = left - 20.f * scale;
  }
  particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                  [](const Particle& p) { return p.maxLife < 0; }),
                   particles_.end());
}

void World::updateSparkles() {
  sparkles_.erase(std::remove_if(sparkles_.begin(), sparkles_.end(),
                                 [&](const Sparkle& s) { return now_ - s.t0 > s.dur; }),
                  sparkles_.end());
  if (!s_.sparkle || s_.reducedMotion) {
    sparkles_.clear();
    return;
  }
  // At most ~2.5 new glints per second in total, each fading in and out over
  // ~1 s: far below the 3 flashes/s photosensitivity limit.
  if (now_ >= nextSparkle_ && !active_.empty() && sparkles_.size() < 8) {
    nextSparkle_ = now_ + rng_.range(0.4f, 1.2f);
    Surface* s = active_[static_cast<size_t>(rng_.rangeInt(0, static_cast<int>(active_.size()) - 1))];
    if (!s->pile.empty()) {
      int col = rng_.rangeInt(0, s->pile.columns() - 1);
      if (s->pile.height(col) > 3.f * s->scale &&
          (col >= static_cast<int>(s->exposed.size()) || s->exposed[static_cast<size_t>(col)])) {
        Sparkle sp;
        sp.surface = s->id;
        sp.localX = s->pile.columnCenter(col);
        sp.size = rng_.range(3.f, 6.f) * s->scale;
        sp.t0 = now_;
        sp.dur = rng_.range(0.9f, 1.4f);
        sparkles_.push_back(sp);
      }
    }
  }
  for (auto& sp : sparkles_) {
    auto it = surfaces_.find(sp.surface);
    if (it == surfaces_.end()) {
      sp.dur = 0;
      continue;
    }
    const Surface& s = it->second;
    sp.x = static_cast<float>(s.rect.left) + sp.localX;
    sp.y = SurfaceY(s, sp.x) + 1.f * s.scale;
  }
}

void World::update(double now, float dt, const std::vector<WindowInput>& windows) {
  now_ = now;
  dt = clampf(dt, 0.f, 0.1f);
  if (monitors_.empty()) return;
  syncSurfaces(windows, dt);
  updateGusts(dt);
  updateCursor(dt);
  updatePiles(dt);
  ensureFlakes();
  updateParticles(dt);
  updateSparkles();
  firstFrame_ = false;
}

WorldStats World::stats() const {
  WorldStats st;
  st.surfaces = static_cast<int>(surfaces_.size());
  st.activeSurfaces = static_cast<int>(active_.size());
  for (const auto& p : particles_) {
    if (p.kind == ParticleKind::Debris) ++st.debris;
    else ++st.flakes;
  }
  for (const auto& [id, s] : surfaces_) st.totalMass += s.pile.mass();
  st.bursts = bursts_;
  return st;
}

}  // namespace snow
