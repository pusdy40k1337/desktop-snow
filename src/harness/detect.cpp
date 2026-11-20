#include "detect.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

namespace snowharness {

namespace {

bool Near(int v, int target, int tol) { return v >= target - tol && v <= target + tol; }

struct LineHit {
  bool edgeFound = false;
  bool ok = false;  // edge and an unambiguous outline found
  int edge = 0;     // index along the line of the first window pixel
  int outline = 0;  // index of the 1-px outline
  int delta = 0;    // (outline + 1) - edge
};

// Scans `count` pixels starting at `first`, advancing `step` bytes per pixel.
LineHit ScanLine(const uint8_t* first, int count, std::ptrdiff_t step) {
  auto cls = [&](int i) {
    const uint8_t* p = first + static_cast<std::ptrdiff_t>(i) * step;
    return Classify(p[0], p[1], p[2]);
  };
  auto magenta = [&](int i) { return IsMagentaClass(cls(i)); };

  LineHit h;
  int edge = -1;
  int run = 0;
  for (int i = 0; i < count; ++i) {
    if (IsWindowClass(cls(i))) {
      if (++run == kMinWindowRun) {
        edge = i - kMinWindowRun + 1;
        break;
      }
    } else {
      run = 0;
    }
  }
  // Index 0 means the window starts at the ROI border: its real edge may lie outside.
  if (edge <= 0) return h;
  h.edgeFound = true;
  h.edge = edge;

  const int lo = std::max(0, edge - kOutlineSearchRange);
  const int hi = std::min(count - 1, edge + kOutlineSearchRange);
  int bestDist = std::numeric_limits<int>::max();
  int bestStart = -1;
  int bestLen = 0;
  bool tie = false;
  for (int i = lo; i <= hi; ++i) {
    if (!magenta(i)) continue;
    // Measure the full run, including any part outside the search window, so a
    // perpendicular outline line crossing the scan line is never mistaken for
    // an isolated 1-px row.
    int s = i;
    while (s > 0 && magenta(s - 1)) --s;
    int e = i;
    while (e + 1 < count && magenta(e + 1)) ++e;
    const int dist = (edge >= s && edge <= e) ? 0 : std::min(std::abs(s - edge), std::abs(e - edge));
    if (dist < bestDist) {
      bestDist = dist;
      bestStart = s;
      bestLen = e - s + 1;
      tie = false;
    } else if (dist == bestDist) {
      tie = true;
    }
    i = e;
  }
  if (bestStart < 0 || tie || bestLen != 1) return h;
  h.ok = true;
  h.outline = bestStart;
  h.delta = (bestStart + 1) - edge;
  return h;
}

struct AxisResult {
  bool valid = false;
  bool edgeFound = false;
  int edge = 0;
  int outline = 0;
  int delta = 0;
  int lines = 0;
};

AxisResult Combine(const std::vector<LineHit>& all) {
  AxisResult r;
  std::vector<int> edges;
  std::vector<LineHit> hits;
  for (const LineHit& h : all) {
    if (h.edgeFound) edges.push_back(h.edge);
    if (h.ok) hits.push_back(h);
  }
  if (!edges.empty()) {
    std::sort(edges.begin(), edges.end());
    r.edgeFound = true;
    r.edge = edges[(edges.size() - 1) / 2];
  }
  r.lines = static_cast<int>(hits.size());
  if (hits.size() < 2) return r;
  std::sort(hits.begin(), hits.end(), [](const LineHit& a, const LineHit& b) { return a.delta < b.delta; });
  if (hits.back().delta - hits.front().delta > 1) return r;
  const auto [minEdge, maxEdge] = std::minmax_element(hits.begin(), hits.end(), [](const LineHit& a, const LineHit& b) {
    return a.edge < b.edge;
  });
  if (maxEdge->edge - minEdge->edge > 1) return r;

  const size_t n = hits.size();
  const LineHit* pick = &hits[n / 2];
  if (n % 2 == 0) {
    const LineHit& a = hits[n / 2 - 1];
    const LineHit& b = hits[n / 2];
    pick = std::abs(a.delta) >= std::abs(b.delta) ? &a : &b;
  }
  r.valid = true;
  r.edge = pick->edge;
  r.outline = pick->outline;
  r.delta = pick->delta;
  return r;
}

}  // namespace

PixelClass Classify(uint8_t b, uint8_t g, uint8_t r, int tol) {
  if (g > tol) return PixelClass::Other;
  const bool rZero = r <= tol;
  const bool rHalf = Near(r, 128, tol);
  const bool bZero = b <= tol;
  const bool bHalf = Near(b, 128, tol);
  const bool bFull = b >= 255 - tol;
  if (rZero && bZero) return PixelClass::Black;
  if (rZero && bFull) return PixelClass::Blue;
  if (rHalf && bHalf) return PixelClass::MagentaOverBlack;
  if (rHalf && bFull) return PixelClass::MagentaOverBlue;
  return PixelClass::Other;
}

const char* PixelClassName(PixelClass c) {
  switch (c) {
    case PixelClass::Black: return "Black";
    case PixelClass::Blue: return "Blue";
    case PixelClass::MagentaOverBlack: return "MagentaOverBlack";
    case PixelClass::MagentaOverBlue: return "MagentaOverBlue";
    case PixelClass::Other: return "Other";
  }
  return "?";
}

Detection DetectOffsets(const uint8_t* bgra, int width, int height, int strideBytes, int originX, int originY,
                        int hintLeft, int hintTop, int hintWidth, int hintHeight) {
  Detection d;
  if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4) return d;

  const int cx = hintLeft + hintWidth / 2 - originX;
  const int cy = hintTop + hintHeight / 2 - originY;
  const int qw = hintWidth / 4;
  const int qh = hintHeight / 4;

  std::vector<LineHit> colHits;
  for (int col : {cx - qw, cx, cx + qw}) {
    if (col < 0 || col >= width) continue;
    colHits.push_back(ScanLine(bgra + static_cast<std::ptrdiff_t>(col) * 4, height, strideBytes));
  }
  std::vector<LineHit> rowHits;
  for (int row : {cy - qh, cy, cy + qh}) {
    if (row < 0 || row >= height) continue;
    rowHits.push_back(ScanLine(bgra + static_cast<std::ptrdiff_t>(row) * strideBytes, width, 4));
  }

  const AxisResult ay = Combine(colHits);
  d.linesY = ay.lines;
  if (ay.edgeFound) d.windowTop = originY + ay.edge;
  if (ay.valid) {
    d.validY = true;
    d.outlineTop = originY + ay.outline + 1;
    d.dy = d.outlineTop - d.windowTop;
  }

  const AxisResult ax = Combine(rowHits);
  d.linesX = ax.lines;
  if (ax.edgeFound) d.windowLeft = originX + ax.edge;
  if (ax.valid) {
    d.validX = true;
    d.outlineLeft = originX + ax.outline + 1;
    d.dx = d.outlineLeft - d.windowLeft;
  }
  return d;
}

BrightEdges DetectBrightEdges(const uint8_t* bgra, int width, int height, int strideBytes, int originX, int originY,
                              int scanX, int scanY, int threshold) {
  BrightEdges e;
  if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4) return e;
  const int sx = scanX - originX;
  const int sy = scanY - originY;
  if (sx < 0 || sx >= width || sy < 0 || sy >= height) return e;
  auto bright = [&](int x, int y) {
    const uint8_t* p = bgra + static_cast<std::ptrdiff_t>(y) * strideBytes + static_cast<std::ptrdiff_t>(x) * 4;
    return p[0] > threshold || p[1] > threshold || p[2] > threshold;
  };
  int top = -1, bottom = -1, left = -1, right = -1;
  for (int y = 0; y < height && top < 0; ++y)
    if (bright(sx, y)) top = y;
  for (int y = height - 1; y >= 0 && bottom < 0; --y)
    if (bright(sx, y)) bottom = y;
  for (int x = 0; x < width && left < 0; ++x)
    if (bright(x, sy)) left = x;
  for (int x = width - 1; x >= 0 && right < 0; --x)
    if (bright(x, sy)) right = x;
  if (top <= 0 || left <= 0 || bottom < 0 || right < 0 || bottom >= height - 1 || right >= width - 1) return e;
  e.valid = true;
  e.left = originX + left;
  e.top = originY + top;
  e.right = originX + right + 1;
  e.bottom = originY + bottom + 1;
  return e;
}

int CountBrightPixels(const uint8_t* bgra, int width, int height, int strideBytes, int threshold) {
  if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4) return 0;
  int n = 0;
  for (int y = 0; y < height; ++y) {
    const uint8_t* p = bgra + static_cast<std::ptrdiff_t>(y) * strideBytes;
    for (int x = 0; x < width; ++x, p += 4)
      if (p[0] > threshold || p[1] > threshold || p[2] > threshold) ++n;
  }
  return n;
}

double PercentileNearestRank(std::vector<double> values, double p) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(values.begin(), values.end());
  const double n = static_cast<double>(values.size());
  long rank = static_cast<long>(std::ceil(std::clamp(p, 0.0, 100.0) / 100.0 * n));
  rank = std::clamp(rank, 1L, static_cast<long>(values.size()));
  return values[static_cast<size_t>(rank - 1)];
}

AxisStats ComputeAxisStats(const std::vector<int>& deltas) {
  AxisStats s;
  s.count = static_cast<int>(deltas.size());
  if (deltas.empty()) return s;
  std::vector<double> abs;
  abs.reserve(deltas.size());
  double sumAbs = 0, sum = 0;
  int within = 0;
  for (int d : deltas) {
    const double a = std::fabs(static_cast<double>(d));
    abs.push_back(a);
    sumAbs += a;
    sum += d;
    if (a <= 1.0) ++within;
    s.maxAbs = std::max(s.maxAbs, a);
  }
  const double n = static_cast<double>(deltas.size());
  s.meanAbs = sumAbs / n;
  s.bias = sum / n;
  s.pctWithin1 = 100.0 * within / n;
  s.p95Abs = PercentileNearestRank(std::move(abs), 95.0);
  return s;
}

SweepSummary Summarize(const std::vector<FrameSample>& frames, double durationMs, double stopInstantMs,
                       double driftWindowMs) {
  SweepSummary s;
  s.frames = static_cast<int>(frames.size());
  std::vector<int> xs, ys;
  std::set<std::pair<int, int>> positions;
  double lastOffT = 0;
  double sumFirst = 0, sumLast = 0;
  for (const FrameSample& f : frames) {
    if (f.validX) xs.push_back(f.dx);
    if (f.validY) ys.push_back(f.dy);
    if (f.validX && f.validY) {
      ++s.validBoth;
      positions.insert({f.outlineLeft, f.outlineTop});
    }
    if (f.validX) s.maxAbsD = std::max(s.maxAbsD, std::abs(f.dx));
    if (f.validY) s.maxAbsD = std::max(s.maxAbsD, std::abs(f.dy));
    if (stopInstantMs >= 0 && f.tMs >= stopInstantMs) {
      const bool off = (f.validX && std::abs(f.dx) > 1) || (f.validY && std::abs(f.dy) > 1);
      if (off) {
        ++s.framesOffAfterStop;
        lastOffT = std::max(lastOffT, f.tMs);
      }
    }
    if (driftWindowMs > 0 && f.validY) {
      if (f.tMs >= 0 && f.tMs < driftWindowMs) {
        sumFirst += f.dy;
        ++s.firstWindowN;
      }
      if (f.tMs >= durationMs - driftWindowMs && f.tMs <= durationMs) {
        sumLast += f.dy;
        ++s.lastWindowN;
      }
    }
  }
  s.validX = static_cast<int>(xs.size());
  s.validY = static_cast<int>(ys.size());
  s.invalid = s.frames - s.validBoth;
  s.x = ComputeAxisStats(xs);
  s.y = ComputeAxisStats(ys);
  s.distinctPositions = static_cast<int>(positions.size());
  if (s.framesOffAfterStop > 0) s.settleMs = lastOffT - stopInstantMs;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  s.firstWindowMeanDy = s.firstWindowN ? sumFirst / s.firstWindowN : nan;
  s.lastWindowMeanDy = s.lastWindowN ? sumLast / s.lastWindowN : nan;
  s.drift = (s.firstWindowN && s.lastWindowN) ? std::fabs(s.lastWindowMeanDy - s.firstWindowMeanDy) : nan;
  if (driftWindowMs <= 0) {
    s.firstWindowMeanDy = s.lastWindowMeanDy = s.drift = 0;
  }
  return s;
}

bool EvaluateAcceptance(const SweepSummary& s, bool isStill, bool checkDrift, std::vector<std::string>* reasons,
                        const AcceptanceLimits& limits) {
  bool ok = true;
  char buf[160];
  auto fail = [&](const char* text) {
    ok = false;
    if (reasons) reasons->emplace_back(text);
  };
  if (s.validX == 0) {
    fail("no valid dx frames");
  } else if (s.x.p95Abs > limits.maxP95) {
    std::snprintf(buf, sizeof buf, "p95|dx|=%.0f > %.0f", s.x.p95Abs, limits.maxP95);
    fail(buf);
  }
  if (s.validY == 0) {
    fail("no valid dy frames");
  } else if (s.y.p95Abs > limits.maxP95) {
    std::snprintf(buf, sizeof buf, "p95|dy|=%.0f > %.0f", s.y.p95Abs, limits.maxP95);
    fail(buf);
  }
  if (isStill) {
    if (s.distinctPositions != 1) {
      std::snprintf(buf, sizeof buf, "still: %d distinct outline positions (want 1)", s.distinctPositions);
      fail(buf);
    }
    if (s.maxAbsD > limits.maxStillD) {
      std::snprintf(buf, sizeof buf, "still: max|d|=%d > %.0f", s.maxAbsD, limits.maxStillD);
      fail(buf);
    }
  }
  if (checkDrift) {
    if (!std::isfinite(s.drift)) {
      fail("drift not measurable (no valid dy in first or last window)");
    } else if (s.drift > limits.maxDrift) {
      std::snprintf(buf, sizeof buf, "drift=%.2f px > %.2f", s.drift, limits.maxDrift);
      fail(buf);
    }
  }
  return ok;
}

}  // namespace snowharness
