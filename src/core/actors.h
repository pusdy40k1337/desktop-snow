// Phase-3 visitors: Santa peeking in, the gift train with an elf, and the yeti
// that hunts a snowman. Pure state machines (portable); drawing lives in the
// Windows renderer. Everything is time-based and cheap.
#pragma once
#include <cstdint>
#include <vector>
#include "geom.h"
#include "rng.h"
#include "world.h"

namespace snow {

enum class ActorKind : int { Santa = 0, Train = 1, Yeti = 2 };

struct SantaState {
  bool active = false;
  int monitor = 0;
  bool fromRight = true;
  float y = 0;        // absolute y of the face centre
  float edgeX = 0;    // absolute x of the screen edge he peeks from
  float peek = 0;     // 0 hidden .. 1 fully in
  float wave = 0;     // arm angle, radians
  float blink = 0;    // 0 open .. 1 closed
  float scale = 1;
};

struct TrainState {
  bool active = false;
  int monitor = 0;
  float x = 0;        // absolute x of the locomotive front
  float groundY = 0;  // rails (absolute y)
  int dir = 1;        // +1 left->right
  float wheel = 0;    // wheel rotation, radians
  float scale = 1;
  float length = 0;   // whole train length, px
  double t = 0;       // seconds since start (smoke puffs)
};

struct YetiState {
  bool active = false;
  int monitor = 0;
  int phase = 0;      // 0 walk in, 1 look up, 2 jump, 3 fall, 4 shrug, 5 walk out
  float x = 0, groundY = 0;
  float lift = 0;     // px above the ground (jump)
  int dir = 1;        // facing
  float walk = 0;     // walk cycle phase
  float scale = 1;
  float shrug = 0;    // 0..1
  uint32_t target = 0;
  float targetX = 0, targetY = 0;
};

struct SnowmanSpot {
  uint32_t id = 0;
  Vec2 base;          // absolute position of the snowman's feet
  int monitor = 0;
};

struct ActorSettings {
  bool santa = true, train = true, yeti = true;
  bool reducedMotion = false;
  float minGap = 150;   // s between visits
  float maxGap = 420;
};

class ActorSystem {
 public:
  ActorSystem();
  void configure(const ActorSettings& s) { s_ = s; }
  void seed(uint64_t s) { rng_.reseed(s); }
  void update(double now, float dt, const std::vector<MonitorInfo>& monitors,
              const std::vector<SnowmanSpot>& snowmen);
  // Manual trigger (tray "Surprise me"). Returns false if not possible now.
  bool trigger(ActorKind k, double now, const std::vector<MonitorInfo>& monitors,
               const std::vector<SnowmanSpot>& snowmen);
  const SantaState& santa() const { return santa_; }
  const TrainState& train() const { return train_; }
  const YetiState& yeti() const { return yeti_; }
  bool anyActive() const { return santa_.active || train_.active || yeti_.active; }
  // The yeti reached the top of its jump: the snowman should hop away.
  bool popSnowmanScare(uint32_t& id, float& dir);
  // The yeti landed: small snow puff at (x, y).
  bool popLanding(Vec2& at);

 private:
  bool startSanta(double now, const std::vector<MonitorInfo>& m);
  bool startTrain(double now, const std::vector<MonitorInfo>& m);
  bool startYeti(double now, const std::vector<MonitorInfo>& m, const std::vector<SnowmanSpot>& sm);
  void scheduleNext(double now);
  ActorSettings s_;
  Rng rng_;
  SantaState santa_;
  TrainState train_;
  YetiState yeti_;
  double santaT0_ = 0, trainT0_ = 0, yetiPhaseT0_ = 0;
  double nextVisit_ = -1;
  bool scarePending_ = false, landingPending_ = false, scareDone_ = false;
  uint32_t scareId_ = 0;
  float scareDir_ = 1;
  Vec2 landingAt_;
  float yetiJumpFromY_ = 0, yetiPeak_ = 0;
};

}  // namespace snow
