#include "motion.h"

#include <algorithm>
#include <cmath>

namespace snow {

void MotionTracker::reset(double t, Vec2d pos) {
  head_ = 0;
  count_ = 0;
  buf_[0] = {t, pos};
  head_ = 1;
  count_ = 1;
  last_ = pos;
  lastT_ = t;
  lastChangeT_ = t;
  vel_ = acc_ = prevVel_ = Vec2d();
  stepInterval_ = 0.008;
  externalInterval_ = 0;
  prevAccT_ = t;
}

void MotionTracker::noteInputEvent(double t, double interval) {
  if (t > lastChangeT_) lastChangeT_ = t;
  if (t > lastEventT_) lastEventT_ = t;
  if (interval > 0.0003 && interval < 0.1) externalInterval_ = interval;
}

double MotionTracker::stillTimeout() const {
  double interval = externalInterval_ > 0 ? externalInterval_ : stepInterval_;
  return clampd(1.6 * interval, p_.minStillTimeout, p_.maxStillTimeout);
}

bool MotionTracker::moving(double now) const {
  if (count_ == 0) return false;
  return (now - lastChangeT_) <= stillTimeout() && vel_.length() > 1.0;
}

void MotionTracker::fitVelocity(double now, Vec2d& outV, bool& ok) const {
  ok = false;
  double st = 0, sx = 0, sy = 0;
  int n = 0;
  double tMin = 1e300, tMax = -1e300;
  for (size_t k = 0; k < count_; ++k) {
    const Sample& s = buf_[(head_ + kCap - 1 - k) % kCap];
    if (now - s.t > p_.fitWindow) break;
    st += s.t;
    sx += s.p.x;
    sy += s.p.y;
    tMin = std::min(tMin, s.t);
    tMax = std::max(tMax, s.t);
    ++n;
  }
  if (n < 2 || tMax - tMin < 0.003) return;
  double mt = st / n, mx = sx / n, my = sy / n;
  double stt = 0, stx = 0, sty = 0;
  for (size_t k = 0; k < static_cast<size_t>(n); ++k) {
    const Sample& s = buf_[(head_ + kCap - 1 - k) % kCap];
    double dt = s.t - mt;
    stt += dt * dt;
    stx += dt * (s.p.x - mx);
    sty += dt * (s.p.y - my);
  }
  if (stt <= 1e-12) return;
  outV = Vec2d(stx / stt, sty / stt);
  ok = true;
}

void MotionTracker::addSample(double t, Vec2d pos) {
  if (count_ == 0) {
    reset(t, pos);
    return;
  }
  if (t < lastT_) t = lastT_;
  double dtS = t - lastT_;
  bool changed = std::fabs(pos.x - last_.x) > 0.01 || std::fabs(pos.y - last_.y) > 0.01;
  prevSampleT_ = lastT_;
  changedLast_ = changed;
  if (changed) {
    double interval = t - lastChangeT_;
    if (interval > 0.0003 && interval < 0.1) stepInterval_ = stepInterval_ * 0.8 + interval * 0.2;
    if (t > lastChangeT_) lastChangeT_ = t;
  }
  buf_[head_] = {t, pos};
  head_ = (head_ + 1) % kCap;
  if (count_ < kCap) ++count_;
  last_ = pos;
  lastT_ = t;

  Vec2d raw;
  bool ok = false;
  fitVelocity(t, raw, ok);
  bool stopped = (t - lastChangeT_) > stillTimeout();
  if (dtS <= 0) return;
  if (stopped) {
    // Hard stop: lingering velocity would make the prediction overshoot.
    vel_ = Vec2d();
  } else if (ok) {
    double a = 1.0 - std::exp(-dtS / p_.emaTau);
    vel_ = vel_ + (raw - vel_) * a;
  }
  Vec2d rawAcc = (vel_ - prevVel_) * (1.0 / dtS);
  double aa = 1.0 - std::exp(-dtS / p_.accelTau);
  acc_ = acc_ + (rawAcc - acc_) * aa;
  prevVel_ = vel_;
}

Vec2d MotionTracker::predict(double tTarget, double now) const {
  if (!moving(now)) return last_;
  double lead = clampd(tTarget - lastT_, 0.0, p_.maxPredict);
  return last_ + vel_ * lead;
}

Vec2d MotionTracker::predictStepped(double tTarget, double now) const {
  if (!moving(now)) return last_;
  double iv = externalInterval_ > 0 ? externalInterval_ : stepInterval_;
  // Fine-grained input (>= ~400 Hz mice): event-time jitter is comparable to the
  // step, so the smooth linear extrapolation measured better (harness, CI VM).
  if (iv < 0.0025) return predict(tTarget, now);
  // Phase of the step train: the event time of the latest step when it belongs to
  // what the sample saw; otherwise the sample time (the event is still in flight).
  double phase = lastChangeT_;
  if (lastEventT_ > 0 && (lastEventT_ > prevSampleT_ || !changedLast_)) phase = lastEventT_;
  double since = std::min(tTarget, lastT_ + p_.maxPredict) - phase;
  double steps = std::floor(since / iv);
  if (steps < 1.0) return last_;
  return last_ + vel_ * (steps * iv);
}

ScatterEvent ScatterDetector::update(double now, double dt, double speed, double accel) {
  ScatterEvent ev;
  if (dt < 0) dt = 0;
  recentPeak_ = std::max(speed, recentPeak_ * std::exp(-dt / 0.15));
  bool still = speed < p_.stillSpeed;
  if (!still) {
    lastMotion_ = now;
    stillSince_ = now;
  }
  wasStill_ = still;
  bool jolt = accel > p_.accelOn && recentPeak_ > p_.joltMinSpeed;

  if (!detached_) {
    if ((speed > p_.speedOn || jolt) && now - lastBurst_ >= p_.refractory) {
      detached_ = true;
      ev.burst = true;
      ev.fromJolt = jolt && speed <= p_.speedOn;
      double peak = std::max(speed, recentPeak_);
      double over = (peak - p_.speedOff) / std::max(1.0, p_.speedOn);
      ev.fraction = static_cast<float>(clampd(0.45 + 0.55 * over, 0.45, 1.0));
      if (ev.fromJolt) ev.fraction = static_cast<float>(clampd(0.4 * accel / p_.accelOn, 0.3, 0.9));
      lastBurst_ = now;
      ++bursts_;
    }
  } else {
    if (speed < p_.speedOff && !jolt) {
      detached_ = false;  // hysteresis: only re-attach well below the trigger speed
    } else if (speed > p_.speedOn) {
      // Sustained fast motion keeps shedding what is left.
      ev.fraction = static_cast<float>(clampd((speed - p_.speedOn) / p_.speedOn * 2.0 * dt + 0.6 * dt, 0.0, 0.5));
    }
    if (jolt && now - lastBurst_ >= p_.refractory * 3) {
      ev.burst = true;
      ev.fromJolt = true;
      ev.fraction = 0.6f;
      lastBurst_ = now;
      ++bursts_;
    }
  }
  return ev;
}

bool ScatterDetector::canGrow(double now) const {
  return !detached_ && wasStill_ && (now - stillSince_) >= p_.regrowDelay;
}

}  // namespace snow
