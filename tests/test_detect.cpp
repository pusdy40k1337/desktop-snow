// Unit tests for the SnowHarness pixel classification, offset detection,
// statistics and sweep generators (portable code only).
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "harness/detect.h"
#include "harness/sweeps.h"
#include "test.h"

using namespace snowharness;

namespace {

struct Image {
  int w;
  int h;
  int stride;
  std::vector<uint8_t> px;

  Image(int width, int height, int padding = 0)
      : w(width), h(height), stride(width * 4 + padding), px(static_cast<size_t>(stride) * height, 0) {}

  bool In(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  uint8_t* At(int x, int y) { return &px[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4]; }
  void Set(int x, int y, uint8_t b, uint8_t g, uint8_t r) {
    if (!In(x, y)) return;
    uint8_t* p = At(x, y);
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
  }
  void Fill(int l, int t, int r, int b, uint8_t cb, uint8_t cg, uint8_t cr) {
    for (int y = t; y < b; ++y)
      for (int x = l; x < r; ++x) Set(x, y, cb, cg, cr);
  }
  // Premultiplied BGRA (128,0,128,128) composited "over" the existing pixel.
  void BlendMagenta(int x, int y) {
    if (!In(x, y)) return;
    uint8_t* p = At(x, y);
    auto over = [](int src, int dst) { return static_cast<uint8_t>(src + (dst * 127 + 127) / 255); };
    p[0] = over(128, p[0]);
    p[1] = over(0, p[1]);
    p[2] = over(128, p[2]);
  }
  // The overlay's outline around the rect it believes in (right/bottom exclusive).
  void Outline(int l, int t, int r, int b) {
    for (int x = l - 1; x <= r; ++x) {
      BlendMagenta(x, t - 1);
      BlendMagenta(x, b);
    }
    for (int y = t; y < b; ++y) {
      BlendMagenta(l - 1, y);
      BlendMagenta(r, y);
    }
  }
  PixelClass ClassAt(int x, int y) {
    const uint8_t* p = At(x, y);
    return Classify(p[0], p[1], p[2]);
  }
};

constexpr int kOriginX = 1000;
constexpr int kOriginY = 400;
constexpr int kWinW = 480;
constexpr int kWinH = 320;

struct Scene {
  Image img;
  int winL;  // ROI-relative window rect
  int winT;
  explicit Scene(int padding = 0, int roiW = 800, int roiH = 600) : img(roiW, roiH, padding), winL(150), winT(120) {}

  void DrawWindow() { img.Fill(winL, winT, winL + kWinW, winT + kWinH, 255, 0, 0); }
  void DrawOutline(int dx, int dy) { img.Outline(winL + dx, winT + dy, winL + dx + kWinW, winT + dy + kWinH); }

  Detection Detect(int hintDx = 0, int hintDy = 0) const {
    return DetectOffsets(img.px.data(), img.w, img.h, img.stride, kOriginX, kOriginY, kOriginX + winL + hintDx,
                         kOriginY + winT + hintDy, kWinW, kWinH);
  }
  // Scan positions (ROI-relative) for the default hint.
  int ColCenter() const { return winL + kWinW / 2; }
  int RowCenter() const { return winT + kWinH / 2; }
};

Scene MakeScene(int dx, int dy, int padding = 0) {
  Scene s(padding);
  s.DrawWindow();
  s.DrawOutline(dx, dy);
  return s;
}

std::string Describe(int dx, int dy, const Detection& d) {
  return "dx=" + std::to_string(dx) + " dy=" + std::to_string(dy) + " -> validX=" + std::to_string(d.validX) +
         " validY=" + std::to_string(d.validY) + " got dx=" + std::to_string(d.dx) + " dy=" + std::to_string(d.dy);
}

}  // namespace

TEST(Classify_ExactColors) {
  CHECK(Classify(0, 0, 0) == PixelClass::Black);
  CHECK(Classify(255, 0, 0) == PixelClass::Blue);
  CHECK(Classify(128, 0, 128) == PixelClass::MagentaOverBlack);
  CHECK(Classify(255, 0, 128) == PixelClass::MagentaOverBlue);
  CHECK(Classify(255, 255, 255) == PixelClass::Other);
  CHECK(Classify(0, 255, 0) == PixelClass::Other);
  CHECK(Classify(0, 0, 255) == PixelClass::Other);  // pure red
  CHECK(Classify(64, 0, 64) == PixelClass::Other);
  CHECK(Classify(128, 0, 0) == PixelClass::Other);
}

TEST(Classify_Tolerance) {
  CHECK(Classify(6, 6, 6) == PixelClass::Black);
  CHECK(Classify(7, 0, 0) == PixelClass::Other);
  CHECK(Classify(249, 6, 6) == PixelClass::Blue);
  CHECK(Classify(248, 0, 0) == PixelClass::Other);
  CHECK(Classify(122, 6, 134) == PixelClass::MagentaOverBlack);
  CHECK(Classify(121, 0, 128) == PixelClass::Other);
  CHECK(Classify(128, 0, 135) == PixelClass::Other);
  CHECK(Classify(250, 3, 133) == PixelClass::MagentaOverBlue);
  CHECK(Classify(255, 7, 128) == PixelClass::Other);
  CHECK(Classify(255, 0, 128, 0) == PixelClass::MagentaOverBlue);
  CHECK(Classify(254, 0, 128, 0) == PixelClass::Other);
}

TEST(Composite_MatchesContract) {
  Image img(2, 1);
  img.Set(1, 0, 255, 0, 0);
  img.BlendMagenta(0, 0);
  img.BlendMagenta(1, 0);
  CHECK(img.At(0, 0)[0] == 128 && img.At(0, 0)[1] == 0 && img.At(0, 0)[2] == 128);
  CHECK(img.At(1, 0)[0] == 255 && img.At(1, 0)[1] == 0 && img.At(1, 0)[2] == 128);
}

TEST(Detect_AllOffsets) {
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      Scene s = MakeScene(dx, dy);
      Detection d = s.Detect();
      const std::string what = Describe(dx, dy, d);
      CHECK_MSG(d.validX && d.validY, what);
      CHECK_MSG(d.dx == dx && d.dy == dy, what);
      CHECK_MSG(d.windowLeft == kOriginX + s.winL && d.windowTop == kOriginY + s.winT, what);
      CHECK_MSG(d.outlineLeft == kOriginX + s.winL + dx && d.outlineTop == kOriginY + s.winT + dy, what);
      CHECK_MSG(d.linesX == 3 && d.linesY == 3, what);
    }
  }
}

TEST(Detect_OutlineOverlapsFirstRowAndColumn) {
  Scene s = MakeScene(1, 1);
  // The outline lies on the window's first row/column: composited over blue.
  CHECK(s.img.ClassAt(s.ColCenter(), s.winT) == PixelClass::MagentaOverBlue);
  CHECK(s.img.ClassAt(s.winL, s.RowCenter()) == PixelClass::MagentaOverBlue);
  CHECK(s.img.ClassAt(s.ColCenter(), s.winT - 1) == PixelClass::Black);
  Detection d = s.Detect();
  CHECK(d.validX && d.validY);
  CHECK(d.dx == 1 && d.dy == 1);
  CHECK(d.windowTop == kOriginY + s.winT);  // MagentaOverBlue counts as window
  CHECK(d.windowLeft == kOriginX + s.winL);
}

TEST(Detect_GapAboveWindow) {
  Scene s = MakeScene(-2, -2);
  CHECK(s.img.ClassAt(s.ColCenter(), s.winT - 3) == PixelClass::MagentaOverBlack);
  CHECK(s.img.ClassAt(s.ColCenter(), s.winT - 2) == PixelClass::Black);
  CHECK(s.img.ClassAt(s.ColCenter(), s.winT - 1) == PixelClass::Black);
  Detection d = s.Detect();
  CHECK(d.validX && d.validY);
  CHECK(d.dx == -2 && d.dy == -2);
}

TEST(Detect_LargeOffsetsWithinRange) {
  Scene s = MakeScene(-50, 40);
  Detection d = s.Detect();
  CHECK(d.validX && d.validY);
  CHECK(d.dx == -50);
  CHECK(d.dy == 40);
}

TEST(Detect_HintDisplacedAndStridePadding) {
  Scene s = MakeScene(2, -1, 12);
  Detection d = s.Detect(60, -40);  // lagging hint still puts every scan line on the window
  CHECK(d.validX && d.validY);
  CHECK(d.dx == 2 && d.dy == -1);
  CHECK(d.linesX == 3 && d.linesY == 3);
}

TEST(Detect_NoiseTolerated) {
  Scene s = MakeScene(1, -1);
  const int cols[3] = {s.ColCenter() - kWinW / 4, s.ColCenter(), s.ColCenter() + kWinW / 4};
  const int rows[3] = {s.RowCenter() - kWinH / 4, s.RowCenter(), s.RowCenter() + kWinH / 4};
  // Deterministic "Other" noise everywhere except on the scan lines.
  uint32_t state = 12345;
  const uint8_t palette[4][3] = {{255, 255, 255}, {0, 200, 0}, {100, 100, 100}, {0, 220, 220}};
  for (int i = 0; i < 4000; ++i) {
    state = state * 1664525u + 1013904223u;
    const int x = static_cast<int>((state >> 8) % static_cast<uint32_t>(s.img.w));
    state = state * 1664525u + 1013904223u;
    const int y = static_cast<int>((state >> 8) % static_cast<uint32_t>(s.img.h));
    if (x == cols[0] || x == cols[1] || x == cols[2] || y == rows[0] || y == rows[1] || y == rows[2]) continue;
    const uint8_t* c = palette[(state >> 4) % 4];
    s.img.Set(x, y, c[0], c[1], c[2]);
  }
  // Targeted noise on the scan lines that must not fool the detector.
  s.img.Set(cols[1], s.winT - 20, 255, 0, 0);                // lone blue pixel above the window
  s.img.Set(cols[0], s.winT - 30, 255, 0, 0);                // two-pixel blue run
  s.img.Set(cols[0], s.winT - 29, 255, 0, 0);
  s.img.Set(cols[2], s.winT - 80, 128, 0, 128);              // magenta beyond the search range
  s.img.Set(s.winL - 90, rows[1], 255, 255, 255);            // white pixel left of the window
  s.img.Set(s.winL + 200, rows[2], 0, 255, 0);               // green pixel inside the window
  s.img.Set(s.winL - 100, rows[0], 128, 0, 128);             // magenta beyond the search range
  Detection d = s.Detect();
  CHECK(d.validX && d.validY);
  CHECK(d.dx == 1 && d.dy == -1);
}

TEST(Detect_OneCorruptedLinePerAxisStillValid) {
  Scene s = MakeScene(0, 2);
  // Overwrite the outline pixel where it crosses one scan column and one scan row.
  s.img.Set(s.ColCenter() + kWinW / 4, s.winT + 1, 255, 255, 255);
  s.img.Set(s.winL - 1, s.RowCenter(), 0, 255, 0);
  Detection d = s.Detect();
  CHECK(d.validX && d.validY);
  CHECK(d.dx == 0 && d.dy == 2);
  CHECK(d.linesY == 2);
  CHECK(d.linesX == 2);
}

TEST(Detect_TwoLinesReportLargerOffset) {
  Scene s;
  s.DrawWindow();
  // Top outline at dy=0 left of the centre column, dy=+1 from just right of it.
  const int split = s.ColCenter() + 1;
  for (int x = s.winL - 1; x <= s.winL + kWinW; ++x) s.img.BlendMagenta(x, x < split ? s.winT - 1 : s.winT);
  s.img.Set(s.ColCenter(), s.winT - 1, 255, 255, 255);  // corrupt the centre column
  Detection d = s.Detect();
  CHECK(d.linesY == 2);
  CHECK(d.validY);
  CHECK(d.dy == 1);
}

TEST(Detect_NoOutlineInvalid) {
  Scene s;
  s.DrawWindow();
  Detection d = s.Detect();
  CHECK(!d.validX && !d.validY);
  CHECK(d.linesX == 0 && d.linesY == 0);
  // The window itself is still located for diagnostics.
  CHECK(d.windowLeft == kOriginX + s.winL && d.windowTop == kOriginY + s.winT);
}

TEST(Detect_NoWindowInvalid) {
  Scene s;
  s.DrawOutline(0, 0);
  Detection d = s.Detect();
  CHECK(!d.validX && !d.validY);
}

TEST(Detect_OutlineBeyondSearchRangeInvalid) {
  Scene s = MakeScene(0, -70);
  Detection d = s.Detect();
  CHECK(!d.validY);
  CHECK(d.validX && d.dx == 0);
}

TEST(Detect_WindowEdgeAtRoiBorderInvalid) {
  Scene s;
  s.winT = 0;  // the window extends above the ROI: its top edge is not visible
  s.DrawWindow();
  s.DrawOutline(0, 0);
  Detection d = s.Detect();
  CHECK(!d.validY);
  CHECK(d.validX && d.dx == 0);
}

TEST(Detect_DisagreeingColumnsInvalid) {
  Scene s = MakeScene(0, 0);
  const int col = s.ColCenter() + kWinW / 4;
  s.img.Set(col, s.winT - 1, 0, 0, 0);  // move this column's outline 3 px lower
  s.img.BlendMagenta(col, s.winT + 2);
  Detection d = s.Detect();
  CHECK(!d.validY);
  CHECK(d.validX);
}

TEST(Detect_TwoOutlinesEquidistantInvalid) {
  Scene s = MakeScene(0, 0);
  for (int x = s.winL; x < s.winL + kWinW; ++x) s.img.BlendMagenta(x, s.winT + 1);  // second row at dy=+2
  Detection d = s.Detect();
  CHECK(!d.validY);
}

TEST(Detect_ThickOutlineInvalid) {
  Scene s = MakeScene(0, 0);
  for (int x = s.winL - 1; x <= s.winL + kWinW; ++x) s.img.BlendMagenta(x, s.winT - 2);
  Detection d = s.Detect();
  CHECK(!d.validY);
}

TEST(Detect_PerpendicularLineOnScanColumn) {
  // dx = -W/4 puts the overlay's right vertical line exactly on the right scan column.
  Scene s = MakeScene(-kWinW / 4, 2);
  CHECK(s.img.ClassAt(s.ColCenter() + kWinW / 4, s.RowCenter()) == PixelClass::MagentaOverBlue);
  Detection d = s.Detect();
  CHECK(d.validY);
  CHECK(d.dy == 2);
  CHECK(d.linesY == 2);
  CHECK(!d.validX);  // 120 px is beyond the outline search range
}

TEST(Detect_ScanLinesOutsideRoi) {
  Scene s = MakeScene(1, 1);
  // Hint far to the left: every scan column falls outside the ROI.
  Detection d = DetectOffsets(s.img.px.data(), s.img.w, s.img.h, s.img.stride, kOriginX, kOriginY,
                              kOriginX + s.winL - 560, kOriginY + s.winT, kWinW, kWinH);
  CHECK(!d.validY && d.linesY == 0);
  CHECK(d.validX && d.dx == 1);
  CHECK(DetectOffsets(nullptr, 10, 10, 40, 0, 0, 0, 0, 10, 10).validX == false);
}

TEST(BrightEdges_FramedWindow) {
  Image img(700, 500);
  img.Fill(40, 30, 660, 470, 20, 20, 20);     // drop shadow, below the threshold
  img.Fill(50, 40, 650, 460, 60, 60, 60);     // 1-px border
  img.Fill(51, 41, 649, 459, 255, 255, 255);  // client area
  BrightEdges e = DetectBrightEdges(img.px.data(), img.w, img.h, img.stride, 100, 200, 100 + 350, 200 + 250);
  CHECK(e.valid);
  CHECK(e.left == 150 && e.top == 240 && e.right == 750 && e.bottom == 660);
}

TEST(BrightEdges_TouchingBorderInvalid) {
  Image img(300, 200);
  img.Fill(0, 20, 280, 180, 255, 255, 255);
  BrightEdges e = DetectBrightEdges(img.px.data(), img.w, img.h, img.stride, 0, 0, 150, 100);
  CHECK(!e.valid);
  Image dark(50, 50);
  CHECK(!DetectBrightEdges(dark.px.data(), 50, 50, dark.stride, 0, 0, 25, 25).valid);
}

TEST(BrightEdges_DarkTitleBarWithLowThreshold) {
  // Windows 10 with a dark accent on title bars, over the black backdrop: the
  // 1-px border and the caption are dark but not black; the shadow stays black.
  Image img(700, 500);
  img.Fill(50, 40, 650, 460, 30, 30, 30);     // 1-px border
  img.Fill(51, 41, 649, 71, 32, 24, 20);      // dark caption
  img.Fill(51, 71, 649, 459, 255, 255, 255);  // client area
  BrightEdges hi = DetectBrightEdges(img.px.data(), img.w, img.h, img.stride, 0, 0, 350, 250);
  CHECK(hi.valid && hi.top == 71 && hi.left == 51);  // the old threshold misses border and caption
  BrightEdges lo = DetectBrightEdges(img.px.data(), img.w, img.h, img.stride, 0, 0, 350, 250, 12);
  CHECK(lo.valid);
  CHECK(lo.left == 50 && lo.top == 40 && lo.right == 650 && lo.bottom == 460);
}

TEST(CountBrightPixels_SnowOverBlack) {
  Image img(100, 80);
  CHECK(CountBrightPixels(img.px.data(), img.w, img.h, img.stride) == 0);
  img.Fill(10, 10, 14, 14, 200, 200, 200);  // a flake
  img.Fill(50, 50, 52, 52, 40, 40, 40);     // too dim to count
  CHECK(CountBrightPixels(img.px.data(), img.w, img.h, img.stride) == 16);
  CHECK(CountBrightPixels(nullptr, 10, 10, 40) == 0);
}

TEST(Stats_Percentile) {
  std::vector<double> v;
  for (int i = 20; i >= 1; --i) v.push_back(i);
  CHECK_NEAR(PercentileNearestRank(v, 95), 19, 0);
  CHECK_NEAR(PercentileNearestRank(v, 100), 20, 0);
  CHECK_NEAR(PercentileNearestRank(v, 0), 1, 0);
  CHECK_NEAR(PercentileNearestRank({1, 2, 3, 4}, 50), 2, 0);
  CHECK_NEAR(PercentileNearestRank({7}, 95), 7, 0);
  CHECK(std::isnan(PercentileNearestRank({}, 95)));
}

TEST(Stats_AxisStats) {
  AxisStats s = ComputeAxisStats({0, 0, 1, -1, 2, -3});
  CHECK(s.count == 6);
  CHECK_NEAR(s.meanAbs, 7.0 / 6.0, 1e-9);
  CHECK_NEAR(s.bias, -1.0 / 6.0, 1e-9);
  CHECK_NEAR(s.maxAbs, 3, 0);
  CHECK_NEAR(s.p95Abs, 3, 0);
  CHECK_NEAR(s.pctWithin1, 400.0 / 6.0, 1e-9);
  AxisStats e = ComputeAxisStats({});
  CHECK(e.count == 0 && e.meanAbs == 0);
}

namespace {
FrameSample Sample(double t, int dx, int dy, bool vx = true, bool vy = true, int ol = 100, int ot = 200) {
  FrameSample f;
  f.tMs = t;
  f.dx = dx;
  f.dy = dy;
  f.validX = vx;
  f.validY = vy;
  f.outlineLeft = ol;
  f.outlineTop = ot;
  return f;
}
}  // namespace

TEST(Stats_SummarizeStill) {
  std::vector<FrameSample> f = {Sample(0, 0, 0), Sample(10, 0, 0), Sample(20, 0, 0, false, true)};
  SweepSummary s = Summarize(f, 2000, -1, 0);
  CHECK(s.frames == 3 && s.validBoth == 2 && s.invalid == 1);
  CHECK(s.validX == 2 && s.validY == 3);
  CHECK(s.distinctPositions == 1 && s.maxAbsD == 0);
  CHECK(EvaluateAcceptance(s, true, false, nullptr));
  f.push_back(Sample(30, 1, 0, true, true, 101, 200));
  s = Summarize(f, 2000, -1, 0);
  CHECK(s.distinctPositions == 2 && s.maxAbsD == 1);
  std::vector<std::string> reasons;
  CHECK(!EvaluateAcceptance(s, true, false, &reasons));
  CHECK(reasons.size() == 1);
}

TEST(Stats_SummarizeStop) {
  std::vector<FrameSample> f = {Sample(700, 5, 0),  Sample(800, 3, 0), Sample(850, -2, 0),
                                Sample(900, 1, 0),  Sample(1000, 0, 2), Sample(1100, 9, 0, false, true),
                                Sample(1200, 0, 1)};
  SweepSummary s = Summarize(f, 2300, 800, 0);
  CHECK(s.framesOffAfterStop == 3);
  CHECK_NEAR(s.settleMs, 200, 1e-9);
  CHECK(s.drift == 0);
  SweepSummary none = Summarize({Sample(900, 0, 0)}, 2300, 800, 0);
  CHECK(none.framesOffAfterStop == 0 && none.settleMs == 0);
}

TEST(Stats_SummarizeDrift) {
  std::vector<FrameSample> f = {Sample(0, 0, 0),    Sample(400, 0, 0),   Sample(999, 0, 1),  Sample(2000, 0, 5),
                                Sample(3000, 0, 1), Sample(3999, 0, 1),  Sample(3500, 0, 9, true, false)};
  SweepSummary s = Summarize(f, 4000, -1, 1000);
  CHECK(s.firstWindowN == 3 && s.lastWindowN == 2);
  CHECK_NEAR(s.firstWindowMeanDy, 1.0 / 3.0, 1e-9);
  CHECK_NEAR(s.lastWindowMeanDy, 1.0, 1e-9);
  CHECK_NEAR(s.drift, 2.0 / 3.0, 1e-9);
  std::vector<std::string> reasons;
  CHECK(!EvaluateAcceptance(s, false, true, &reasons));  // drift 0.67 > 0.5 and p95|dy| = 5 > 1
  CHECK(reasons.size() == 2);
  SweepSummary empty = Summarize({Sample(0, 0, 0)}, 4000, -1, 1000);
  CHECK(std::isnan(empty.drift));
  CHECK(!EvaluateAcceptance(empty, false, true, nullptr));
}

TEST(Stats_Acceptance) {
  std::vector<FrameSample> f;
  for (int i = 0; i < 100; ++i) f.push_back(Sample(i * 10.0, i < 96 ? 1 : 4, i < 95 ? -1 : 0));
  SweepSummary s = Summarize(f, 1000, -1, 0);
  CHECK_NEAR(s.x.p95Abs, 1, 0);
  CHECK(EvaluateAcceptance(s, false, false, nullptr));
  f[0].dx = 3;  // 5 of 100 now exceed 1 -> p95 still 1
  CHECK(EvaluateAcceptance(Summarize(f, 1000, -1, 0), false, false, nullptr));
  f[1].dx = 3;  // 6 of 100 -> p95 = 3
  std::vector<std::string> reasons;
  CHECK(!EvaluateAcceptance(Summarize(f, 1000, -1, 0), false, false, &reasons));
  CHECK(reasons.size() == 1 && reasons[0].find("dx") != std::string::npos);
  std::vector<std::string> none;
  CHECK(!EvaluateAcceptance(Summarize({}, 1000, -1, 0), false, false, &none));
  CHECK(none.size() == 2);
}

TEST(Sweeps_StayInBounds) {
  const SweepArea areas[] = {MakeSweepArea(16, 96, 1904, 1064, kWinW, kWinH),
                             MakeSweepArea(16, 96, 1350, 712, kWinW, kWinH),  // narrow 1366x728 work area
                             MakeSweepArea(-1904, 96, -16, 1064, kWinW, kWinH)};
  for (const SweepArea& a : areas) {
    CHECK(a.Valid());
    for (double scale : {1.0, 0.5}) {
      for (SweepKind k : kAllSweeps) {
        const double dur = SweepDurationSec(k, scale);
        for (double t = 0; t <= dur; t += 0.001) {
          SweepPoint p = SweepPosition(k, a, t, scale);
          if (p.x < a.minX || p.x > a.maxX || p.y < a.minY || p.y > a.maxY) {
            CHECK_MSG(false, std::string(SweepName(k)) + " out of bounds at t=" + std::to_string(t));
            break;
          }
        }
      }
    }
  }
}

TEST(Sweeps_Shapes) {
  const SweepArea a = MakeSweepArea(16, 96, 1904, 1064, kWinW, kWinH);
  CHECK(a.minX == 16 && a.maxX == 1424 && a.minY == 96 && a.maxY == 744);
  CHECK(a.CenterX() == 720 && a.CenterY() == 420);

  SweepPoint p0 = SweepPosition(SweepKind::Still, a, 0, 1);
  SweepPoint p1 = SweepPosition(SweepKind::Still, a, 1.7, 1);
  CHECK(p0.x == 720 && p0.y == 420 && p1.x == 720 && p1.y == 420);

  p1 = SweepPosition(SweepKind::Slow, a, 1.0, 1);
  CHECK(p1.x == 870 && p1.y == 570);
  p1 = SweepPosition(SweepKind::Slow, a, 2.5, 1);  // y reflected off maxY: 420 + 375 = 795 -> 693
  CHECK(p1.x == 1095 && p1.y == 693);

  p1 = SweepPosition(SweepKind::Fast, a, 0.1, 1);
  CHECK(p1.x == 900 && p1.y == 480);
  p1 = SweepPosition(SweepKind::Fast, a, 0.5, 1);  // 720 + 900 = 1620 -> reflected to 1228
  CHECK(p1.x == 1228 && p1.y == 720);

  p0 = SweepPosition(SweepKind::Sine, a, 0, 1);
  CHECK(p0.x == 720 && p0.y == 420);
  p1 = SweepPosition(SweepKind::Sine, a, 0.4, 1);  // quarter period of x
  CHECK(p1.x == 720 + 493);

  const double stopT = StopInstantSec(1.0);
  CHECK_NEAR(stopT, 0.8, 1e-12);
  p0 = SweepPosition(SweepKind::Stop, a, 0, 1);
  const SweepPoint pStop = SweepPosition(SweepKind::Stop, a, stopT, 1);
  CHECK(pStop.x - p0.x == 975);  // 225 px accelerating + 750 px cruising
  CHECK(SweepPosition(SweepKind::Stop, a, 2.29, 1).x == pStop.x);
  CHECK(SweepPosition(SweepKind::Stop, a, 1.2, 1).y == a.CenterY());
  const int v = SweepPosition(SweepKind::Stop, a, 0.7, 1).x - SweepPosition(SweepKind::Stop, a, 0.5, 1).x;
  CHECK(v >= 299 && v <= 301);  // 1500 px/s cruise

  for (double t = 0; t < 4; t += 0.25) CHECK(SweepPosition(SweepKind::VSweep, a, t, 1).x == a.CenterX());
  CHECK(SweepPosition(SweepKind::VSweep, a, 0.2, 1).y == 600);

  CHECK_NEAR(SweepDurationSec(SweepKind::Stop, 1.0), 2.3, 1e-12);
  CHECK_NEAR(SweepDurationSec(SweepKind::Stop, 0.5), 1.15, 1e-12);
  CHECK_NEAR(SweepDurationSec(SweepKind::Sine, 0.5), 2.0, 1e-12);
  CHECK_NEAR(StopInstantSec(0.5), 0.4, 1e-12);

  CHECK_NEAR(FoldInto(12, 0, 10), 8, 1e-12);
  CHECK_NEAR(FoldInto(-3, 0, 10), 3, 1e-12);
  CHECK_NEAR(FoldInto(25, 0, 10), 5, 1e-12);
  CHECK_NEAR(FoldInto(5, 3, 3), 3, 1e-12);

  SweepKind k = SweepKind::Still;
  CHECK(ParseSweepName("vsweep", &k) && k == SweepKind::VSweep);
  CHECK(!ParseSweepName("bogus", &k));
}
