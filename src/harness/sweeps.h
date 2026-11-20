// Scripted window-movement sweeps for SnowHarness. Portable and header-only so
// the unit tests can exercise it without linking anything else.
#pragma once
#include <algorithm>
#include <cmath>
#include <string_view>

namespace snowharness {

enum class SweepKind { Still, Slow, Fast, Sine, Stop, VSweep };

inline constexpr SweepKind kAllSweeps[] = {SweepKind::Still, SweepKind::Slow, SweepKind::Fast,
                                           SweepKind::Sine,  SweepKind::Stop, SweepKind::VSweep};

inline const char* SweepName(SweepKind k) {
  switch (k) {
    case SweepKind::Still: return "still";
    case SweepKind::Slow: return "slow";
    case SweepKind::Fast: return "fast";
    case SweepKind::Sine: return "sine";
    case SweepKind::Stop: return "stop";
    case SweepKind::VSweep: return "vsweep";
  }
  return "?";
}

inline bool ParseSweepName(std::string_view name, SweepKind* out) {
  for (SweepKind k : kAllSweeps) {
    if (name == SweepName(k)) {
      *out = k;
      return true;
    }
  }
  return false;
}

namespace sweep {
inline constexpr double kStillSec = 2.0;
inline constexpr double kSlowSec = 3.0;
inline constexpr double kFastSec = 3.0;
inline constexpr double kSineSec = 4.0;
inline constexpr double kVSweepSec = 4.0;
inline constexpr double kStopAccelSec = 0.3;
inline constexpr double kStopCruiseSec = 0.5;
inline constexpr double kStopHoldSec = 1.5;
inline constexpr double kInterSweepHoldSec = 0.5;

inline constexpr double kSlowSpeed = 150.0;  // px/s on both axes
inline constexpr double kFastSpeedX = 1800.0;
inline constexpr double kFastSpeedY = 600.0;
inline constexpr double kStopSpeed = 1500.0;
inline constexpr double kVSweepSpeed = 900.0;
inline constexpr double kSinePeriodX = 1.6;
inline constexpr double kSinePeriodY = 2.3;
inline constexpr double kSineAmpX = 0.35;  // fraction of the free width
inline constexpr double kSineAmpY = 0.25;  // fraction of the free height
inline constexpr double kPi = 3.14159265358979323846;
}  // namespace sweep

// Allowed range of the window's top-left corner, inclusive.
struct SweepArea {
  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;
  int FreeWidth() const { return maxX - minX; }
  int FreeHeight() const { return maxY - minY; }
  int CenterX() const { return minX + FreeWidth() / 2; }
  int CenterY() const { return minY + FreeHeight() / 2; }
  bool Valid() const { return maxX >= minX && maxY >= minY; }
};

// Bounds are the rectangle the whole window must stay inside (right/bottom exclusive).
inline SweepArea MakeSweepArea(int boundsLeft, int boundsTop, int boundsRight, int boundsBottom, int winW,
                               int winH) {
  return SweepArea{boundsLeft, boundsTop, boundsRight - winW, boundsBottom - winH};
}

struct SweepPoint {
  int x = 0;
  int y = 0;
};

// `scale` is 1.0 normally and 0.5 with --quick: it scales durations (including
// the phases of `stop`) but never speeds or sine periods.
inline double SweepDurationSec(SweepKind k, double scale) {
  switch (k) {
    case SweepKind::Still: return sweep::kStillSec * scale;
    case SweepKind::Slow: return sweep::kSlowSec * scale;
    case SweepKind::Fast: return sweep::kFastSec * scale;
    case SweepKind::Sine: return sweep::kSineSec * scale;
    case SweepKind::Stop: return (sweep::kStopAccelSec + sweep::kStopCruiseSec + sweep::kStopHoldSec) * scale;
    case SweepKind::VSweep: return sweep::kVSweepSec * scale;
  }
  return 0;
}

// Time at which `stop` halts instantly.
inline double StopInstantSec(double scale) { return (sweep::kStopAccelSec + sweep::kStopCruiseSec) * scale; }

// Reflects an unbounded coordinate into [lo, hi] (triangle wave).
inline double FoldInto(double s, double lo, double hi) {
  const double span = hi - lo;
  if (span <= 0) return lo;
  double m = std::fmod(s - lo, 2 * span);
  if (m < 0) m += 2 * span;
  return m <= span ? lo + m : lo + 2 * span - m;
}

inline SweepPoint SweepPosition(SweepKind k, const SweepArea& a, double t, double scale) {
  t = std::max(0.0, t);
  const double cx = a.CenterX();
  const double cy = a.CenterY();
  double x = cx;
  double y = cy;
  switch (k) {
    case SweepKind::Still:
      break;
    case SweepKind::Slow:
      x = FoldInto(cx + sweep::kSlowSpeed * t, a.minX, a.maxX);
      y = FoldInto(cy + sweep::kSlowSpeed * t, a.minY, a.maxY);
      break;
    case SweepKind::Fast:
      x = FoldInto(cx + sweep::kFastSpeedX * t, a.minX, a.maxX);
      y = FoldInto(cy + sweep::kFastSpeedY * t, a.minY, a.maxY);
      break;
    case SweepKind::Sine:
      x = cx + sweep::kSineAmpX * a.FreeWidth() * std::sin(2 * sweep::kPi * t / sweep::kSinePeriodX);
      y = cy + sweep::kSineAmpY * a.FreeHeight() * std::sin(2 * sweep::kPi * t / sweep::kSinePeriodY);
      break;
    case SweepKind::Stop: {
      const double accel = sweep::kStopAccelSec * scale;
      const double cruise = sweep::kStopCruiseSec * scale;
      const double v = sweep::kStopSpeed;
      const double dAccel = 0.5 * v * accel;
      const double travel = dAccel + v * cruise;
      const double start = std::max(static_cast<double>(a.minX), cx - travel / 2);
      double s = travel;
      if (t < accel) {
        s = 0.5 * (v / accel) * t * t;
      } else if (t < accel + cruise) {
        s = dAccel + v * (t - accel);
      }
      // On monitors narrower than the travel the path reflects off the bound.
      x = FoldInto(start + s, a.minX, a.maxX);
      break;
    }
    case SweepKind::VSweep:
      y = FoldInto(cy + sweep::kVSweepSpeed * t, a.minY, a.maxY);
      break;
  }
  SweepPoint p;
  p.x = std::clamp(static_cast<int>(std::lround(x)), a.minX, std::max(a.minX, a.maxX));
  p.y = std::clamp(static_cast<int>(std::lround(y)), a.minY, std::max(a.minY, a.maxY));
  return p;
}

}  // namespace snowharness
