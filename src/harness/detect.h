// Portable pixel classification, edge/offset detection and summary statistics
// for SnowHarness. No Windows headers: unit-tested on any platform.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace snowharness {

// Per-channel tolerance used by Classify (DWM composition rounding).
inline constexpr int kColorTolerance = 6;
// Search range (rows/columns) around the window edge for the overlay outline.
inline constexpr int kOutlineSearchRange = 64;
// Consecutive window-coloured pixels required to accept a window edge.
inline constexpr int kMinWindowRun = 3;

enum class PixelClass { Black, Blue, MagentaOverBlack, MagentaOverBlue, Other };

// Classifies a composited screen pixel. Expected values:
//   Black (0,0,0), Blue (R0,G0,B255), MagentaOverBlack (R128,G0,B128),
//   MagentaOverBlue (R128,G0,B255); anything else is Other.
PixelClass Classify(uint8_t b, uint8_t g, uint8_t r, int tolerance = kColorTolerance);

inline bool IsWindowClass(PixelClass c) { return c == PixelClass::Blue || c == PixelClass::MagentaOverBlue; }
inline bool IsMagentaClass(PixelClass c) {
  return c == PixelClass::MagentaOverBlack || c == PixelClass::MagentaOverBlue;
}

const char* PixelClassName(PixelClass c);

// Result of DetectOffsets. All coordinates are absolute screen pixels.
// outlineLeft/outlineTop are the edge the overlay believes the window has
// (outline column/row + 1), so 0 offset means perfect alignment and a positive
// dy means the overlay is drawn lower than the real window edge.
struct Detection {
  bool validX = false;
  bool validY = false;
  int windowLeft = 0;
  int windowTop = 0;
  int outlineLeft = 0;
  int outlineTop = 0;
  int dx = 0;
  int dy = 0;
  int linesX = 0;  // scan rows that produced a usable horizontal result
  int linesY = 0;  // scan columns that produced a usable vertical result
};

// Finds the blue window's top/left edge and the overlay's magenta outline in a
// top-down BGRA region of interest whose top-left pixel is at absolute
// (originX, originY). The hint (commanded window rect, absolute) places the
// scan lines: three columns at hint centre x and +/- a quarter of the width,
// three rows at hint centre y and +/- a quarter of the height.
//
// An axis is valid when at least two scan lines succeed, their offsets differ
// by at most 1 px and their window edges differ by at most 1 px. With three
// lines the median line is reported; with two the one with the larger |offset|
// (never under-reports misalignment).
Detection DetectOffsets(const uint8_t* bgra, int width, int height, int strideBytes, int originX, int originY,
                        int hintLeft, int hintTop, int hintWidth, int hintHeight);

// Visible bounds of a bright object on a dark background, found by scanning
// from the ROI edges inwards along the column scanX and the row scanY
// (absolute) for the first pixel with any channel > threshold.
// right/bottom are exclusive, like RECT. Invalid if an edge touches the ROI
// border (the object may extend beyond the ROI) or nothing bright is found.
struct BrightEdges {
  bool valid = false;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};
BrightEdges DetectBrightEdges(const uint8_t* bgra, int width, int height, int strideBytes, int originX, int originY,
                              int scanX, int scanY, int threshold = 40);

// Pixels with any channel above `threshold` (snow over a black window).
int CountBrightPixels(const uint8_t* bgra, int width, int height, int strideBytes, int threshold = 60);

// ---- Statistics ---------------------------------------------------------

// Nearest-rank percentile (p in [0,100]); NaN for an empty input.
double PercentileNearestRank(std::vector<double> values, double p);

struct AxisStats {
  int count = 0;
  double meanAbs = 0;
  double p95Abs = 0;
  double maxAbs = 0;
  double bias = 0;        // mean signed offset
  double pctWithin1 = 0;  // percentage of samples with |d| <= 1
};
AxisStats ComputeAxisStats(const std::vector<int>& deltas);

struct FrameSample {
  double tMs = 0;  // relative to the sweep start
  bool validX = false;
  bool validY = false;
  int dx = 0;
  int dy = 0;
  int outlineLeft = 0;
  int outlineTop = 0;
};

struct SweepSummary {
  int frames = 0;
  int validBoth = 0;
  int invalid = 0;  // frames - validBoth
  int validX = 0;
  int validY = 0;
  AxisStats x;
  AxisStats y;
  // Jitter (meaningful for `still`): distinct (outlineLeft, outlineTop) among
  // frames valid on both axes, and max(|dx|,|dy|) over valid axes.
  int distinctPositions = 0;
  int maxAbsD = 0;
  // Settling (meaningful for `stop`): frames at or after the stop instant with
  // |d| > 1 on a valid axis, and the time from the stop instant to the last one.
  int framesOffAfterStop = 0;
  double settleMs = 0;
  // Drift (meaningful for `vsweep`): mean signed dy in the first and last
  // drift window; drift = |last - first|. NaN when a window has no valid dy.
  double firstWindowMeanDy = 0;
  double lastWindowMeanDy = 0;
  int firstWindowN = 0;
  int lastWindowN = 0;
  double drift = 0;
};

// stopInstantMs < 0 disables the settling metrics; driftWindowMs <= 0 disables drift.
SweepSummary Summarize(const std::vector<FrameSample>& frames, double durationMs, double stopInstantMs,
                       double driftWindowMs);

struct AcceptanceLimits {
  double maxP95 = 1.0;     // p95 of |dx| and |dy|
  double maxStillD = 1.0;  // max |d| while still
  double maxDrift = 0.5;   // px
};

// Appends a human-readable reason for every violated limit; returns true if none.
bool EvaluateAcceptance(const SweepSummary& s, bool isStill, bool checkDrift, std::vector<std::string>* reasons,
                        const AcceptanceLimits& limits = AcceptanceLimits{});

}  // namespace snowharness
