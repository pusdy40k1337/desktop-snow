#include "actors.h"

#include <algorithm>
#include <cmath>

namespace snow {

namespace {
constexpr double kSantaDur = 8.0;
constexpr float kTrainSpeed = 120.f;  // DIP/s
constexpr float kYetiWalk = 170.f;    // DIP/s
}  // namespace

ActorSystem::ActorSystem() : rng_(0xA11CE5EEDULL) {}

void ActorSystem::scheduleNext(double now) { nextVisit_ = now + rng_.range(s_.minGap, s_.maxGap); }

bool ActorSystem::startSanta(double now, const std::vector<MonitorInfo>& m) {
  if (m.empty()) return false;
  int i = rng_.rangeInt(0, static_cast<int>(m.size()) - 1);
  const MonitorInfo& mon = m[static_cast<size_t>(i)];
  santa_ = SantaState();
  santa_.active = true;
  santa_.monitor = i;
  santa_.fromRight = rng_.chance(0.5f);
  santa_.scale = mon.scale;
  santa_.y = static_cast<float>(mon.work.top) + static_cast<float>(mon.work.height()) * rng_.range(0.25f, 0.65f);
  santa_.edgeX = static_cast<float>(santa_.fromRight ? mon.rect.right : mon.rect.left);
  santaT0_ = now;
  return true;
}

bool ActorSystem::startTrain(double now, const std::vector<MonitorInfo>& m) {
  if (m.empty()) return false;
  int i = rng_.rangeInt(0, static_cast<int>(m.size()) - 1);
  const MonitorInfo& mon = m[static_cast<size_t>(i)];
  train_ = TrainState();
  train_.active = true;
  train_.monitor = i;
  train_.scale = mon.scale;
  train_.dir = rng_.chance(0.5f) ? 1 : -1;
  train_.length = 330.f * mon.scale;
  train_.groundY = static_cast<float>(mon.work.bottom);
  train_.x = static_cast<float>(train_.dir > 0 ? mon.rect.left : mon.rect.right);
  trainT0_ = now;
  (void)trainT0_;
  return true;
}

bool ActorSystem::startYeti(double now, const std::vector<MonitorInfo>& m, const std::vector<SnowmanSpot>& sm) {
  if (m.empty() || sm.empty()) return false;
  const SnowmanSpot& target = sm[static_cast<size_t>(rng_.rangeInt(0, static_cast<int>(sm.size()) - 1))];
  if (target.monitor < 0 || target.monitor >= static_cast<int>(m.size())) return false;
  const MonitorInfo& mon = m[static_cast<size_t>(target.monitor)];
  yeti_ = YetiState();
  yeti_.active = true;
  yeti_.monitor = target.monitor;
  yeti_.scale = mon.scale;
  yeti_.groundY = static_cast<float>(mon.work.bottom);
  bool fromLeft = target.base.x > static_cast<float>(mon.rect.left + mon.rect.width() / 2) ? false : true;
  if (rng_.chance(0.3f)) fromLeft = !fromLeft;
  yeti_.dir = fromLeft ? 1 : -1;
  yeti_.x = fromLeft ? static_cast<float>(mon.rect.left) - 70.f * mon.scale
                     : static_cast<float>(mon.rect.right) + 70.f * mon.scale;
  yeti_.target = target.id;
  yeti_.targetX = target.base.x;
  yeti_.targetY = target.base.y;
  yeti_.phase = 0;
  yetiPhaseT0_ = now;
  scareDone_ = false;
  return true;
}

bool ActorSystem::trigger(ActorKind k, double now, const std::vector<MonitorInfo>& monitors,
                          const std::vector<SnowmanSpot>& snowmen) {
  if (monitors.empty()) return false;
  switch (k) {
    case ActorKind::Santa:
      if (santa_.active) return true;
      return startSanta(now, monitors);
    case ActorKind::Train:
      if (train_.active) return true;
      return startTrain(now, monitors);
    case ActorKind::Yeti:
      if (yeti_.active) return true;
      return startYeti(now, monitors, snowmen);
  }
  return false;
}

void ActorSystem::update(double now, float dt, const std::vector<MonitorInfo>& monitors,
                         const std::vector<SnowmanSpot>& snowmen) {
  if (nextVisit_ < 0) nextVisit_ = now + rng_.range(60.f, 150.f);
  if (monitors.empty()) {
    santa_.active = train_.active = yeti_.active = false;
    return;
  }

  // Automatic visits: one at a time, never in reduced-motion mode.
  if (!s_.reducedMotion && !anyActive() && now >= nextVisit_) {
    ActorKind options[3];
    int n = 0;
    if (s_.santa) options[n++] = ActorKind::Santa;
    if (s_.train) options[n++] = ActorKind::Train;
    if (s_.yeti && !snowmen.empty()) options[n++] = ActorKind::Yeti;
    if (n > 0) trigger(options[rng_.rangeInt(0, n - 1)], now, monitors, snowmen);
    scheduleNext(now);
  }

  if (santa_.active) {
    if (santa_.monitor >= static_cast<int>(monitors.size())) {
      santa_.active = false;
    } else {
      float t = static_cast<float>(now - santaT0_);
      if (t >= kSantaDur) {
        santa_.active = false;
      } else {
        if (t < 1.2f) santa_.peek = easeOutCubic(t / 1.2f);
        else if (t < 6.6f) santa_.peek = 1.f;
        else santa_.peek = 1.f - easeInOut((t - 6.6f) / 1.4f);
        santa_.wave = (t > 1.4f && t < 6.4f && !s_.reducedMotion) ? 0.45f * std::sin((t - 1.4f) * 7.f) : 0.f;
        float bt = std::fmod(t, 3.1f);
        santa_.blink = bt < 0.14f ? std::sin(bt / 0.14f * kPi) : 0.f;
      }
    }
  }

  if (train_.active) {
    if (train_.monitor >= static_cast<int>(monitors.size())) {
      train_.active = false;
    } else {
      const MonitorInfo& mon = monitors[static_cast<size_t>(train_.monitor)];
      float v = kTrainSpeed * train_.scale;
      train_.groundY = static_cast<float>(mon.work.bottom);
      train_.x += static_cast<float>(train_.dir) * v * dt;
      train_.t += dt;
      train_.wheel += static_cast<float>(train_.dir) * v * dt / (9.f * train_.scale);
      if ((train_.dir > 0 && train_.x - train_.length > static_cast<float>(mon.rect.right)) ||
          (train_.dir < 0 && train_.x + train_.length < static_cast<float>(mon.rect.left)))
        train_.active = false;
    }
  }

  if (yeti_.active) {
    if (yeti_.monitor >= static_cast<int>(monitors.size())) {
      yeti_.active = false;
      return;
    }
    const MonitorInfo& mon = monitors[static_cast<size_t>(yeti_.monitor)];
    yeti_.groundY = static_cast<float>(mon.work.bottom);
    bool found = false;
    for (const auto& s : snowmen) {
      if (s.id == yeti_.target) {
        yeti_.targetX = s.base.x;
        yeti_.targetY = s.base.y;
        found = true;
      }
    }
    float sc = yeti_.scale;
    float t = static_cast<float>(now - yetiPhaseT0_);
    auto next = [&](int phase) {
      yeti_.phase = phase;
      yetiPhaseT0_ = now;
    };
    switch (yeti_.phase) {
      case 0: {  // walk in towards the snowman
        if (!found) {
          next(4);
          break;
        }
        float stopX = yeti_.targetX - static_cast<float>(yeti_.dir) * 34.f * sc;
        float d = stopX - yeti_.x;
        yeti_.dir = d >= 0 ? 1 : -1;
        float stepX = kYetiWalk * sc * dt;
        if (std::fabs(d) <= stepX) {
          yeti_.x = stopX;
          next(1);
        } else {
          yeti_.x += static_cast<float>(yeti_.dir) * stepX;
          yeti_.walk += dt * 9.f;
        }
        if (t > 30.f) next(4);
        break;
      }
      case 1:  // looks up at it
        if (t > 0.8f) {
          next(2);
          yetiJumpFromY_ = yeti_.groundY;
          float h = yeti_.groundY - yeti_.targetY;
          yetiPeak_ = clampf(h + 10.f * sc, 60.f * sc, 420.f * sc);
        }
        break;
      case 2: {  // jump
        const float dur = 0.9f;
        float u = clampf(t / dur, 0.f, 1.f);
        yeti_.lift = 4.f * yetiPeak_ * u * (1.f - u);
        if (u >= 0.45f && !scareDone_) {
          scareDone_ = true;
          scarePending_ = true;
          scareId_ = yeti_.target;
          scareDir_ = static_cast<float>(yeti_.dir);
        }
        if (u >= 1.f) {
          yeti_.lift = 0;
          landingPending_ = true;
          landingAt_ = Vec2(yeti_.x, yeti_.groundY);
          next(4);
        }
        break;
      }
      case 3:
        next(4);
        break;
      case 4: {  // shrug
        const float dur = 1.4f;
        yeti_.shrug = std::sin(clampf(t / dur, 0.f, 1.f) * kPi);
        if (t >= dur) {
          yeti_.shrug = 0;
          float cx = static_cast<float>(mon.rect.left + mon.rect.width() / 2);
          yeti_.dir = yeti_.x < cx ? -1 : 1;
          next(5);
        }
        break;
      }
      case 5: {  // walk out
        yeti_.x += static_cast<float>(yeti_.dir) * kYetiWalk * 1.1f * sc * dt;
        yeti_.walk += dt * 10.f;
        if (yeti_.x < static_cast<float>(mon.rect.left) - 90.f * sc ||
            yeti_.x > static_cast<float>(mon.rect.right) + 90.f * sc)
          yeti_.active = false;
        break;
      }
      default:
        yeti_.active = false;
    }
  }
}

bool ActorSystem::popSnowmanScare(uint32_t& id, float& dir) {
  if (!scarePending_) return false;
  scarePending_ = false;
  id = scareId_;
  dir = scareDir_;
  return true;
}

bool ActorSystem::popLanding(Vec2& at) {
  if (!landingPending_) return false;
  landingPending_ = false;
  at = landingAt_;
  return true;
}

}  // namespace snow
