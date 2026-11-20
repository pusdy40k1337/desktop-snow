// Window motion estimation, one-frame-ahead prediction and the scatter detector.
// Times are seconds (any monotonic clock), positions physical px.
#pragma once
#include <array>
#include <cstddef>
#include "geom.h"

namespace snow {

struct MotionParams {
  double fitWindow = 0.060;     // s, least-squares velocity window
  double emaTau = 0.020;        // s, velocity smoothing time constant
  double accelTau = 0.030;      // s, acceleration smoothing
  double minStillTimeout = 0.006;  // s, lower bound of the adaptive "stopped" timeout
  double maxStillTimeout = 0.045;  // s, upper bound
  double maxPredict = 0.034;    // s, never extrapolate further than this
};

class MotionTracker {
 public:
  explicit MotionTracker(const MotionParams& p = MotionParams()) : p_(p) {}
  void setParams(const MotionParams& p) { p_ = p; }
  void reset(double t, Vec2d pos);
  // Feed the window position observed at time t. Repeated positions are fine
  // (input devices move windows in steps); they are what reveals a stop.
  void addSample(double t, Vec2d pos);
  // Optional: precise time of the latest position-change event (WinEvent
  // LOCATIONCHANGE) and the measured interval between such events. Makes stop
  // detection faster than per-frame sampling alone.
  void noteInputEvent(double t, double interval);

  bool initialized() const { return count_ > 0; }
  Vec2d position() const { return last_; }      // last observed position
  double lastSampleTime() const { return lastT_; }
  double lastChangeTime() const { return lastChangeT_; }
  Vec2d velocity() const { return vel_; }       // smoothed px/s (0 when stopped)
  Vec2d acceleration() const { return acc_; }   // smoothed px/s^2
  double speed() const { return vel_.length(); }
  double accelMagnitude() const { return acc_.length(); }
  // Typical interval between position changes (input step period), seconds.
  double stepInterval() const { return stepInterval_; }
  double stillTimeout() const;
  bool moving(double now) const;
  // Position extrapolated to tTarget. Exactly position() when not moving, so a
  // still window never jitters.
  Vec2d predict(double tTarget, double now) const;
  // Step-aware variant: input devices move windows in periodic steps, so only
  // whole steps expected before tTarget are added (none if the next step is
  // due after tTarget). Falls back to predict() without an input interval.
  Vec2d predictStepped(double tTarget, double now) const;

 private:
  struct Sample { double t; Vec2d p; };
  void fitVelocity(double now, Vec2d& outV, bool& ok) const;
  MotionParams p_;
  static constexpr size_t kCap = 32;
  std::array<Sample, kCap> buf_{};
  size_t head_ = 0, count_ = 0;
  Vec2d last_, vel_, acc_, prevVel_;
  double lastT_ = 0, lastChangeT_ = 0, prevAccT_ = 0;
  double stepInterval_ = 0.008;
  double externalInterval_ = 0;
  double lastEventT_ = 0;     // time of the latest input step reported by events
  double prevSampleT_ = 0;
  bool changedLast_ = false;  // the latest sample saw a new position
};

struct ScatterParams {
  double speedOn = 900.0;       // px/s: detach above this
  double speedOff = 600.0;      // px/s: re-attach below this (hysteresis)
  double accelOn = 22000.0;     // px/s^2: jolt trigger (sudden start/stop/shake)
  double joltMinSpeed = 350.0;  // px/s: jolts below this speed are ignored (noise)
  double stillSpeed = 8.0;      // px/s: "still" for regrowth
  double regrowDelay = 1.5;     // s still before the pile grows again
  double refractory = 0.12;     // s between discrete scatter bursts
};

struct ScatterEvent {
  bool burst = false;        // a discrete detach burst this frame
  float fraction = 0.f;      // fraction of the pile to throw (burst or continuous)
  bool fromJolt = false;
};

class ScatterDetector {
 public:
  explicit ScatterDetector(const ScatterParams& p = ScatterParams()) : p_(p) {}
  void setParams(const ScatterParams& p) { p_ = p; }
  const ScatterParams& params() const { return p_; }
  ScatterEvent update(double now, double dt, double speed, double accel);
  bool detached() const { return detached_; }
  bool canGrow(double now) const;
  double stillSince() const { return stillSince_; }
  int bursts() const { return bursts_; }

 private:
  ScatterParams p_;
  bool detached_ = false;
  double stillSince_ = 0;
  bool wasStill_ = true;
  double lastBurst_ = -1e9;
  double lastMotion_ = -1e9;
  double recentPeak_ = 0;
  int bursts_ = 0;
};

}  // namespace snow
