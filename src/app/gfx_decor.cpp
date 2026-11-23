// Procedural Direct2D artwork: Christmas trees, snowmen, Santa, the gift
// train, the yeti and the decorate-mode palette. Everything is vector-drawn
// from path geometries built once in unit space and placed with transforms.
#include "gfx_decor.h"

#include "../core/knock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

namespace snow::gfx {
namespace {

constexpr float kRad2Deg = 180.f / kPi;

const Theme& ThemeOf(const DecorFrame& f) { return f.theme ? *f.theme : GetTheme(std::string("classic")); }

int ModIdx(int i, int n) {
  const int r = i % n;
  return r < 0 ? r + n : r;
}

float EaseOutBack(float t) {
  t = clampf(t, 0.f, 1.f);
  const float c1 = 1.70158f, c3 = c1 + 1.f;
  const float u = t - 1.f;
  return 1.f + c3 * u * u * u + c1 * u * u;
}

float Hash01(int k) {
  const float v = std::sin(static_cast<float>(k) * 12.9898f) * 43758.5453f;
  return v - std::floor(v);
}

// ---- tree layout (units: 1 = tree height; x centred, y = v - 1) ------------
constexpr float kSlope = 0.49f * 0.72f / 0.78f;  // foliage half-width per unit of v
float HalfWidth(float v) { return kSlope * (v - 0.08f); }
constexpr float kTierBottom[3] = {0.38f, 0.64f, 0.86f};
constexpr int kTierScallops[3] = {3, 4, 5};
constexpr float kTierDepth[3] = {0.020f, 0.024f, 0.028f};
constexpr int kHemSeg = 10;
constexpr uint32_t kTierGreen[3] = {0x45B368, 0x38A35A, 0x2C8F4B};
constexpr float kGarlandV[3] = {0.25f, 0.47f, 0.71f};
constexpr float kGarlandSag[3] = {0.045f, 0.058f, 0.070f};
constexpr int kGarlandBulbs[3] = {5, 7, 9};

void GarlandPoint(int g, float t, float& x, float& y) {
  const float w = 0.9f * HalfWidth(kGarlandV[g]);
  x = w * (2.f * t - 1.f);
  y = kGarlandV[g] + kGarlandSag[g] * 4.f * t * (1.f - t) - 1.f;
}

constexpr float kTrainDesignLen = 316.f;

// Santa hat (face radius = 1): the cone is shifted up so its base hides under the fur trim.
constexpr float kHatDy = -0.24f;
constexpr float kTrimTop = -0.90f, kTrimBottom = -0.46f;
constexpr float kPomX = 1.78f, kPomY = -0.86f + kHatDy;

ComPtr<ID2D1PathGeometry> BuildFur(ID2D1Factory1* fac, float cx, float cy, float rx, float ry, int n, float inner,
                                   float jitter, int seed) {
  PathBuilder pb(fac);
  for (int k = 0; k < n; ++k) {
    const float a0 = 2.f * kPi * static_cast<float>(k) / static_cast<float>(n);
    const float a1 = a0 + kPi / static_cast<float>(n);
    const float a2 = a0 + 2.f * kPi / static_cast<float>(n);
    const float j0 = 1.f + jitter * (Hash01(k + seed * 131) - 0.5f) * 2.f;
    const float j2 = 1.f + jitter * (Hash01(((k + 1) % n) + seed * 131) - 0.5f) * 2.f;
    if (k == 0) pb.begin(cx + rx * j0 * std::cos(a0), cy + ry * j0 * std::sin(a0));
    pb.line(cx + rx * inner * std::cos(a1), cy + ry * inner * std::sin(a1));
    pb.line(cx + rx * j2 * std::cos(a2), cy + ry * j2 * std::sin(a2));
  }
  pb.end();
  return pb.finish();
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime and cached resources

void DecorRenderer::Reset(ID2D1DeviceContext* dc, ID2D1Factory1* factory) {
  dc_ = dc;
  factory_ = factory;
  radial_.reset();
  built_ = false;
  roundCap_.Reset();
  candyDash_.Reset();
  for (auto& g : treeTier_) g.Reset();
  for (auto& g : treeFrost_) g.Reset();
  for (auto& g : garland_) g.Reset();
  star_.Reset();
  candy_.Reset();
  carrot_.Reset();
  bucket_.Reset();
  beanie_.Reset();
  beard_.Reset();
  santaHat_.Reset();
  elfEar_.Reset();
  elfHat_.Reset();
  yetiBody_.Reset();
  yetiHead_.Reset();
  yetiHorn_.Reset();
  brush_.reset(dc);
}

void DecorRenderer::ensureGeometry() {
  if (built_ || !factory_) return;
  built_ = true;

  ID2D1Factory* fac = factory_;
  {
    D2D1_STROKE_STYLE_PROPERTIES sp{};
    sp.startCap = D2D1_CAP_STYLE_ROUND;
    sp.endCap = D2D1_CAP_STYLE_ROUND;
    sp.dashCap = D2D1_CAP_STYLE_ROUND;
    sp.lineJoin = D2D1_LINE_JOIN_ROUND;
    sp.miterLimit = 2.f;
    sp.dashStyle = D2D1_DASH_STYLE_SOLID;
    sp.dashOffset = 0.f;
    fac->CreateStrokeStyle(&sp, nullptr, 0, &roundCap_);
    D2D1_STROKE_STYLE_PROPERTIES dp = sp;
    dp.startCap = D2D1_CAP_STYLE_FLAT;
    dp.endCap = D2D1_CAP_STYLE_FLAT;
    dp.dashCap = D2D1_CAP_STYLE_FLAT;
    dp.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    const float dashes[2] = {1.f, 1.f};
    fac->CreateStrokeStyle(&dp, dashes, 2, &candyDash_);
  }

  // Foliage: three full-width tiers sharing the apex, with scalloped hems; the
  // frost is a wedge of snow lying along each hem that thickens towards the tips.
  for (int j = 0; j < 3; ++j) {
    const float b = kTierBottom[j], d = kTierDepth[j];
    const int n = kTierScallops[j], total = n * kHemSeg;
    const float cornerV = b - d;
    const float half = HalfWidth(cornerV) + 0.008f;
    const float apexY = 0.08f - 1.f;
    float hx[64], hy[64];
    for (int i = 0; i <= total; ++i) {
      const float tt = static_cast<float>(i) / static_cast<float>(total);
      const float sc = tt * static_cast<float>(n);
      const float fr = sc - std::floor(sc);
      hx[i] = half - 2.f * half * tt;
      hy[i] = b - d + d * std::sin(kPi * fr) - 1.f;
    }
    PathBuilder tb(factory_);
    tb.begin(0.f, apexY);
    for (int i = 0; i <= total; ++i) tb.line(hx[i], hy[i]);
    tb.end();
    treeTier_[j] = tb.finish();

    PathBuilder fb(factory_);
    fb.begin(hx[0], hy[0]);
    for (int i = 1; i <= total; ++i) fb.line(hx[i], hy[i]);
    const float slopeY = (cornerV - 0.08f) / half;
    for (int i = total; i >= 0; --i) {
      const float ax = std::fabs(hx[i]);
      const float ramp = smoothstep(0.30f * half, 0.95f * half, ax);
      const float thick = 0.034f * ramp * (0.8f + 0.2f * std::sin(static_cast<float>(i) * 1.9f));
      float yy = hy[i] - thick;
      const float sideY = apexY + ax * slopeY;
      if (yy < sideY) yy = sideY;
      fb.line(hx[i], yy);
    }
    fb.end();
    treeFrost_[j] = fb.finish();
  }

  for (int g = 0; g < 3; ++g) {
    PathBuilder gb(factory_);
    constexpr int kN = 24;
    for (int i = 0; i <= kN; ++i) {
      float x, y;
      GarlandPoint(g, static_cast<float>(i) / static_cast<float>(kN), x, y);
      if (i == 0) {
        gb.begin(x, y, false);
      } else {
        gb.line(x, y);
      }
    }
    gb.end(false);
    garland_[g] = gb.finish();
  }

  {  // five-point star, outer radius 1
    PathBuilder sb(factory_);
    for (int k = 0; k < 10; ++k) {
      const float a = -kPi / 2.f + static_cast<float>(k) * kPi / 5.f;
      const float r = (k % 2 == 0) ? 1.f : 0.42f;
      if (k == 0) {
        sb.begin(r * std::cos(a), r * std::sin(a));
      } else {
        sb.line(r * std::cos(a), r * std::sin(a));
      }
    }
    sb.end();
    star_ = sb.finish();
  }

  {  // candy cane centre line, height 1, hook to the right
    PathBuilder cb(factory_);
    const float k = 0.5523f * 0.25f;
    cb.begin(0.f, 0.5f, false);
    cb.line(0.f, -0.25f);
    cb.cubic(0.f, -0.25f - k, 0.25f - k, -0.5f, 0.25f, -0.5f);
    cb.cubic(0.25f + k, -0.5f, 0.5f, -0.25f - k, 0.5f, -0.25f);
    cb.line(0.5f, -0.12f);
    cb.end(false);
    candy_ = cb.finish();
  }

  {  // carrot nose: base at x=0, tip at x=1
    PathBuilder nb(factory_);
    nb.begin(0.f, -0.2f);
    nb.quad(0.5f, -0.16f, 1.f, 0.04f);
    nb.quad(0.5f, 0.16f, 0.f, 0.2f);
    nb.end();
    carrot_ = nb.finish();
  }

  {  // beanie dome: upper half of the unit ellipse
    PathBuilder bb(factory_);
    bb.begin(-1.f, 0.f);
    bb.cubic(-1.f, -0.56f, -0.55f, -1.f, 0.f, -1.f);
    bb.cubic(0.55f, -1.f, 1.f, -0.56f, 1.f, 0.f);
    bb.end();
    beanie_ = bb.finish();
  }

  {  // bucket hat (tree units)
    PathBuilder bb(factory_);
    bb.begin(-0.09f, -0.99f);
    bb.line(0.09f, -0.99f);
    bb.line(0.068f, -0.84f);
    bb.line(-0.068f, -0.84f);
    bb.end();
    bucket_ = bb.finish();
  }

  {  // Santa's beard (face radius = 1): scalloped lower outline, soft top edge
    auto cubicAt = [](const float* c, float t, float& ox, float& oy) {
      const float u = 1.f - t;
      ox = u * u * u * c[0] + 3.f * u * u * t * c[2] + 3.f * u * t * t * c[4] + t * t * t * c[6];
      oy = u * u * u * c[1] + 3.f * u * u * t * c[3] + 3.f * u * t * t * c[5] + t * t * t * c[7];
    };
    const float segA[8] = {-1.02f, 0.05f, -1.14f, 0.55f, -0.98f, 1.12f, -0.55f, 1.46f};
    const float segB[8] = {-0.55f, 1.46f, -0.32f, 1.64f, -0.14f, 1.74f, 0.f, 1.78f};
    float px[24], py[24];
    int n = 0;
    for (int i = 0; i <= 6; ++i, ++n) cubicAt(segA, static_cast<float>(i) / 6.f, px[n], py[n]);
    for (int i = 1; i <= 3; ++i, ++n) cubicAt(segB, static_cast<float>(i) / 3.f, px[n], py[n]);
    const int half = n;  // left half incl. the bottom centre point
    for (int i = half - 2; i >= 0; --i, ++n) {
      px[n] = -px[i];
      py[n] = py[i];
    }
    PathBuilder bb(factory_);
    bb.begin(px[0], py[0]);
    for (int i = 0; i + 1 < n; ++i) {
      const float mx = 0.5f * (px[i] + px[i + 1]), my = 0.5f * (py[i] + py[i + 1]);
      float nx = mx, ny = my - 0.4f;
      const float len = std::sqrt(nx * nx + ny * ny);
      if (len > 1e-4f) {
        nx /= len;
        ny /= len;
      }
      bb.quad(mx + nx * 0.11f, my + ny * 0.11f, px[i + 1], py[i + 1]);
    }
    bb.cubic(0.95f, 0.50f, 0.55f, 0.56f, 0.f, 0.42f);
    bb.cubic(-0.55f, 0.56f, -0.95f, 0.50f, px[0], py[0]);
    bb.end();
    beard_ = bb.finish();
  }

  {  // Santa hat, tip drooping towards +x (face radius = 1)
    PathBuilder hb(factory_);
    const float d = kHatDy;
    hb.begin(-0.98f, -0.40f + d);
    hb.cubic(-1.08f, -1.05f + d, -0.55f, -1.62f + d, 0.25f, -1.68f + d);
    hb.cubic(0.95f, -1.72f + d, 1.55f, -1.50f + d, 1.84f, -1.05f + d);
    hb.cubic(1.90f, -0.95f + d, 1.82f, -0.80f + d, 1.66f, -0.74f + d);
    hb.cubic(1.45f, -0.90f + d, 1.10f, -0.95f + d, 0.97f, -0.55f + d);
    hb.line(0.98f, -0.40f + d);
    hb.end();
    santaHat_ = hb.finish();
  }

  {  // elf ear (pointing to +x) and hat (curled tip to +x)
    PathBuilder eb(factory_);
    eb.begin(0.f, -0.45f);
    eb.quad(0.55f, -0.5f, 1.f, -0.2f);
    eb.quad(0.5f, 0.1f, 0.f, 0.45f);
    eb.end();
    elfEar_ = eb.finish();
    PathBuilder hb(factory_);
    hb.begin(-1.f, 0.f);
    hb.cubic(-1.f, -1.f, -0.2f, -1.9f, 0.9f, -2.35f);
    hb.cubic(0.6f, -1.7f, 1.f, -0.8f, 1.f, 0.f);
    hb.end();
    elfHat_ = hb.finish();
  }

  // Yeti (unit space: height 1, feet at y=0): jagged fur outlines and a horn.
  yetiBody_ = BuildFur(factory_, 0.f, -0.50f, 0.30f, 0.27f, 30, 0.86f, 0.05f, 1);
  yetiHead_ = BuildFur(factory_, 0.02f, -0.80f, 0.17f, 0.145f, 22, 0.84f, 0.05f, 2);
  {
    PathBuilder hb(factory_);
    hb.begin(-0.5f, 0.f);
    hb.cubic(-0.5f, -0.8f, -0.2f, -1.5f, 0.4f, -2.f);
    hb.cubic(0.5f, -1.2f, 0.55f, -0.5f, 0.5f, 0.f);
    hb.end();
    yetiHorn_ = hb.finish();
  }
}

void DecorRenderer::shadedEllipse(GradKind k, const D2D1_COLOR_F& c, float cx, float cy, float rx, float ry, float ox,
                                  float oy) {
  ID2D1RadialGradientBrush* br = radial_.get(dc_, k, c);
  if (!br) return;
  br->SetCenter(Pt(cx, cy));
  br->SetGradientOriginOffset(Pt(ox, oy));
  br->SetRadiusX(rx * 1.12f);
  br->SetRadiusY(ry * 1.12f);
  br->SetOpacity(1.f);
  dc_->FillEllipse(D2D1::Ellipse(Pt(cx, cy), rx, ry), br);
}

void DecorRenderer::shadedGeometry(ID2D1Geometry* g, GradKind k, const D2D1_COLOR_F& c, float cx, float cy, float rx,
                                   float ry, float ox, float oy) {
  if (!g) return;
  ID2D1RadialGradientBrush* br = radial_.get(dc_, k, c);
  if (!br) return;
  br->SetCenter(Pt(cx, cy));
  br->SetGradientOriginOffset(Pt(ox, oy));
  br->SetRadiusX(rx);
  br->SetRadiusY(ry);
  br->SetOpacity(1.f);
  dc_->FillGeometry(g, br);
}

void DecorRenderer::glow(const D2D1_COLOR_F& c, float cx, float cy, float rx, float ry, float opacity) {
  if (opacity <= 0.002f) return;
  ID2D1RadialGradientBrush* br = radial_.get(dc_, GradKind::Glow, c);
  if (!br) return;
  br->SetCenter(Pt(cx, cy));
  br->SetGradientOriginOffset(Pt(0.f, 0.f));
  br->SetRadiusX(rx);
  br->SetRadiusY(ry);
  br->SetOpacity(clampf(opacity, 0.f, 1.f));
  dc_->FillEllipse(D2D1::Ellipse(Pt(cx, cy), rx, ry), br);
}

// ---------------------------------------------------------------------------
// Christmas tree

void DecorRenderer::DrawTree(const DecorFrame& f, const TreeDecor& tree, D2D1_POINT_2F base, float height,
                             bool highlight) {
  if (!dc_ || height < 4.f) return;
  const float grow = easeOutCubic(tree.grow);
  if (grow <= 0.002f) return;
  ensureGeometry();
  const Theme& th = ThemeOf(f);
  const float s = height * grow;
  const float px = 1.f / s;
  const float night = clampf(f.night, 0.f, 1.f);
  const float wu = TreeWidth(height) / height;

  TransformScope ts(dc_);
  ts.set(MakeM(s, 0.f, 0.f, s, base.x, base.y));
  const Painter p(dc_, brush_, roundCap_.Get());

  const float tw = 0.5f * 0.14f * wu;  // half trunk width
  if (highlight) {
    const D2D1_COLOR_F acc = Hex(0x8CD9FF);
    for (int j = 0; j < 3; ++j) {
      p.stroke(treeTier_[j].Get(), Fade(acc, 0.14f), 11.f * px);
      p.stroke(treeTier_[j].Get(), Fade(acc, 0.26f), 6.f * px);
      p.stroke(treeTier_[j].Get(), Fade(acc, 0.85f), 2.f * px);
    }
    p.roundedStroke(-tw, -0.17f, tw, 0.f, 0.01f, Fade(acc, 0.85f), 2.f * px);
  }

  if (!f.treeBodyFromSprite) {
  // Trunk and the snow mound it stands in.
  p.rounded(-tw, -0.17f, tw, 0.f, 0.01f, Hex(0x7E4F2D));
  p.rounded(tw * 0.2f, -0.17f, tw, 0.f, 0.01f, Hex(0x000000, 0.2f));
  p.line(-0.35f * tw, -0.14f, -0.35f * tw, -0.05f, Hex(0x4A2E19, 0.55f), 1.f * px);
  p.line(0.1f * tw, -0.12f, 0.1f * tw, -0.03f, Hex(0x4A2E19, 0.45f), 1.f * px);
  p.roundedStroke(-tw, -0.17f, tw, 0.f, 0.01f, Hex(0x4A2E19, 0.85f), 1.f * px);
  p.ellipse(0.f, -0.011f, 0.095f, 0.011f, Hex(0xF2F7FF, 0.96f));

  // Foliage, lowest tier first; each hem casts a soft shade on the tier below.
  for (int j = 2; j >= 0; --j) {
    ID2D1PathGeometry* geo = treeTier_[j].Get();
    if (!geo) continue;
    if (j < 2) {
      TransformScope sh(dc_);
      sh.set(MakeM(1.f, 0.f, 0.f, 1.f, 0.f, 0.016f));
      p.fill(geo, Hex(0x0B3B1E, 0.30f));
    }
    const float topV = (j == 0) ? 0.14f : kTierBottom[j - 1];
    const float midY = 0.5f * (topV + kTierBottom[j]) - 1.f;
    shadedGeometry(geo, GradKind::Shade, Hex(kTierGreen[j]), -0.04f, midY, 0.42f, 0.26f, -0.07f, -0.05f);
    p.stroke(geo, Hex(0x14502B, 0.8f), 1.1f * px);
    p.fill(treeFrost_[j].Get(), Hex(0xF4F8FF, 0.96f));
  }
  }  // tree body (skipped when a PNG sprite replaces it)

  if (tree.lights) {
    const D2D1_COLOR_F wire = Hex(0x14381F, 0.85f);
    for (int g = 0; g < 3; ++g) p.stroke(garland_[g].Get(), wire, 1.1f * px);
    const int nl = static_cast<int>(th.lights.size());
    for (int g = 0; g < 3; ++g) {
      for (int i = 0; i < kGarlandBulbs[g]; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(kGarlandBulbs[g]);
        float x, y;
        GarlandPoint(g, t, x, y);
        const int k = g * 3 + i;
        const D2D1_COLOR_F c = ToD2D(th.lights[static_cast<size_t>(ModIdx(k, nl))]);
        const float rate = 0.35f + 0.11f * static_cast<float>(k % 4);
        const float br = Twinkle(f.time, 0.61f * static_cast<float>(k), rate, f.reducedMotion);
        const float rb = 0.0125f;
        const float by = y + rb + 0.003f;
        glow(c, x, by, rb * (3.0f + 2.8f * night), rb * (3.0f + 2.8f * night), (0.30f + 0.5f * night) * br);
        p.rounded(x - 0.0035f, y - 0.002f, x + 0.0035f, y + 0.006f, 0.001f, Hex(0x1B2A1F));
        p.circle(x, by, rb, MixC(Darken(c, 0.5f), Lighten(c, 0.3f), br));
        p.circle(x - 0.003f, by - 0.004f, rb * 0.32f, Hex(0xFFFFFF, 0.6f * br));
      }
    }
  }

  const int nOrn = static_cast<int>(th.ornaments.size());
  int index = 0;
  for (const Ornament& o : tree.ornaments) {
    const float x = (o.u - 0.5f) * wu;
    const float y = o.v - 1.f;
    const D2D1_COLOR_F c = ToD2D(th.ornaments[static_cast<size_t>(ModIdx(o.color, nOrn))]);
    if (o.kind == OrnamentKind::CandyCane) {
      const float ch = 0.09f;
      const float flip = (index & 1) ? -1.f : 1.f;
      TransformScope cs(dc_);
      cs.set(MakeM(ch * flip, 0.f, 0.f, ch, x - 0.25f * ch * flip, y));
      const float w = 0.25f;
      const float ol = px / ch;
      D2D1_COLOR_F stripe = c;
      if (std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}) < 0.15f) stripe = Hex(0xD8283A);
      p.stroke(candy_.Get(), Hex(0x5A1A22, 0.75f), w + 2.f * ol);
      p.stroke(candy_.Get(), Hex(0xFFFFFF), w);
      if (candyDash_ && candy_) {
        if (auto* sb = brush_(stripe)) dc_->DrawGeometry(candy_.Get(), sb, w, candyDash_.Get());
      }
    } else {
      const float r = 0.035f;
      p.rounded(x - 0.26f * r, y - 1.28f * r, x + 0.26f * r, y - 0.82f * r, 0.12f * r, Hex(0xE6B93C));
      p.ring(x, y - 1.62f * r, 0.26f * r, Hex(0xE6B93C), 0.7f * px);
      shadedEllipse(GradKind::Shade, c, x, y, r, r, -0.34f * r, -0.4f * r);
      p.ring(x, y, r, Darken(c, 0.5f), 0.9f * px);
      p.circle(x - 0.36f * r, y - 0.42f * r, 0.24f * r, Hex(0xFFFFFF, 0.7f));
    }
    ++index;
  }

  if (tree.star) {
    const D2D1_COLOR_F sc = ToD2D(th.star);
    const float pulse = Twinkle(f.time, 0.2f, 0.4f, f.reducedMotion, 0.6f);
    const float R = 0.052f, cx = 0.f, cy = 0.06f - 1.f;
    glow(sc, cx, cy, 0.17f, 0.17f, (0.5f + 0.35f * pulse) * (0.8f + 0.2f * night));
    TransformScope ss(dc_);
    ss.set(MakeM(R, 0.f, 0.f, R, cx, cy));
    shadedGeometry(star_.Get(), GradKind::Shade, sc, 0.f, 0.f, 1.15f, 1.15f, -0.2f, -0.25f);
    p.stroke(star_.Get(), Fade(Darken(sc, 0.55f), 0.9f), 1.f * px / R);
    const float sp = 0.5f * pulse;
    p.line(-1.8f, 0.f, 1.8f, 0.f, Hex(0xFFFFFF, 0.35f * sp), 0.5f * px / R);
    p.line(0.f, -1.8f, 0.f, 1.8f, Hex(0xFFFFFF, 0.35f * sp), 0.5f * px / R);
  }
}

// ---------------------------------------------------------------------------
// Snowman

void DecorRenderer::DrawSnowman(const DecorFrame& f, const SnowmanDecor& sm, D2D1_POINT_2F base, float height,
                                float build, float wobble, bool highlight) {
  if (!dc_ || height < 4.f) return;
  build = clampf(build, 0.f, 1.f);
  if (build <= 0.001f) return;
  ensureGeometry();
  const Theme& th = ThemeOf(f);
  const float px = 1.f / height;
  const float wob = f.reducedMotion ? 0.f : wobble;

  TransformScope ts(dc_);
  ts.set(Xf(height, height, wob, base.x, base.y));
  const Painter p(dc_, brush_, roundCap_.Get());

  const float rb = 0.200f, rm = 0.150f, rh = 0.112f;
  const float cbY = -rb, cmY = -0.515f, chY = -0.747f;
  const float sB = EaseOutBack(build / 0.3f);
  const float sM = EaseOutBack((build - 0.3f) / 0.25f);
  const float sH = EaseOutBack((build - 0.55f) / 0.25f);
  const float acc = EaseOutBack((build - 0.8f) / 0.2f);
  const float accC = std::min(acc, 1.f);

  const D2D1_COLOR_F snowC = Hex(0xF8FBFF);
  const D2D1_COLOR_F outline = Hex(0x7F96BE, 0.85f);
  const D2D1_COLOR_F coal = Hex(0x24242C);

  if (highlight) {
    const D2D1_COLOR_F hc = Hex(0x8CD9FF);
    const float rr[3] = {rb * sB, rm * sM, rh * sH};
    const float yy[3] = {cbY, cmY, chY};
    for (int i = 0; i < 3; ++i) {
      if (rr[i] <= 0.001f) continue;
      p.ring(0.f, yy[i], rr[i], Fade(hc, 0.14f), 11.f * px);
      p.ring(0.f, yy[i], rr[i], Fade(hc, 0.26f), 6.f * px);
      p.ring(0.f, yy[i], rr[i], Fade(hc, 0.85f), 2.f * px);
    }
  }

  auto ball = [&](float cy, float r, float scale) {
    const float rr = r * scale;
    if (rr <= 0.002f) return;
    shadedEllipse(GradKind::Snow, snowC, 0.f, cy, rr, rr, -0.30f * rr, -0.36f * rr);
    p.ring(0.f, cy, rr, outline, 1.1f * px);
  };
  ball(cbY, rb, sB);
  if (sB > 0.01f) {  // the snow mound stays level while the snowman leans
    TransformScope ms(dc_);
    ms.set(Xf(1.f, 1.f, -wob, 0.f, 0.f));
    shadedEllipse(GradKind::Snow, Hex(0xF4F8FF), 0.f, -0.03f, 0.255f * sB, 0.032f * sB, -0.05f, -0.01f);
  }
  ball(cmY, rm, sM);
  ball(chY, rh, sH);
  if (acc <= 0.001f) return;

  // Stick arms with twigs.
  auto stick = [&](float x0, float y0, float x1, float y1) {
    x1 = x0 + (x1 - x0) * accC;
    y1 = y0 + (y1 - y0) * accC;
    const D2D1_COLOR_F wood = Hex(0x6A4426);
    const float w = std::max(0.014f, 1.7f * px);
    p.line(x0, y0, x1, y1, wood, w);
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float ux = dx / len, uy = dy / len;
    auto twig = [&](float sx, float sy, float ang, float l) {
      const float c = std::cos(ang), sn = std::sin(ang);
      p.line(sx, sy, sx + (ux * c - uy * sn) * l, sy + (ux * sn + uy * c) * l, wood, w * 0.75f);
    };
    twig(x0 + dx * 0.55f, y0 + dy * 0.55f, -0.8f, 0.05f);
    twig(x1, y1, 0.55f, 0.032f);
    twig(x1, y1, -0.45f, 0.032f);
  };
  stick(-0.135f, cmY - 0.005f, -0.250f, cmY - 0.075f);
  stick(0.135f, cmY - 0.005f, 0.250f, cmY - 0.115f);

  for (int i = -1; i <= 1; ++i) {
    const float by = cmY + 0.047f * static_cast<float>(i);
    const float r = 0.0155f * accC;
    p.circle(0.f, by, r, coal);
    p.circle(-0.004f, by - 0.004f, r * 0.3f, Hex(0xFFFFFF, 0.45f));
  }

  {  // scarf: band around the neck and a hanging tail
    const D2D1_COLOR_F sc = Hex(DecorColorRgb(ModIdx(sm.scarf, kDecorColorCount)));
    TransformScope as(dc_);
    as.set(ScaleAbout(accC, 0.f, -0.65f));
    const D2D1_COLOR_F edge = Fade(Darken(sc, 0.55f), 0.9f);
    p.rounded(0.04f, -0.64f, 0.10f, -0.50f, 0.014f, sc);
    p.roundedStroke(0.04f, -0.64f, 0.10f, -0.50f, 0.014f, edge, 1.f * px);
    p.line(0.045f, -0.508f, 0.095f, -0.508f, Lighten(sc, 0.5f), 0.012f);
    for (int i = 0; i < 4; ++i) {
      const float fx = 0.05f + 0.014f * static_cast<float>(i);
      p.line(fx, -0.502f, fx, -0.488f, Darken(sc, 0.8f), 0.006f);
    }
    p.rounded(-0.115f, -0.676f, 0.115f, -0.626f, 0.025f, sc);
    p.rounded(-0.115f, -0.676f, 0.115f, -0.658f, 0.018f, Hex(0xFFFFFF, 0.14f));
    p.line(-0.05f, -0.668f, -0.05f, -0.634f, Lighten(sc, 0.5f), 0.011f);
    p.line(0.012f, -0.668f, 0.012f, -0.634f, Lighten(sc, 0.5f), 0.011f);
    p.roundedStroke(-0.115f, -0.676f, 0.115f, -0.626f, 0.025f, edge, 1.f * px);
  }

  // Face: coal eyes and smile, carrot nose, rosy cheeks.
  glow(Hex(0xFF8DA0), -0.066f, chY + 0.026f, 0.032f, 0.026f, 0.5f * accC);
  glow(Hex(0xFF8DA0), 0.066f, chY + 0.026f, 0.032f, 0.026f, 0.5f * accC);
  for (int side = -1; side <= 1; side += 2) {
    const float ex = 0.04f * static_cast<float>(side);
    p.circle(ex, chY - 0.016f, 0.0125f * accC, coal);
    p.circle(ex - 0.003f, chY - 0.02f, 0.004f * accC, Hex(0xFFFFFF, 0.6f));
  }
  for (int i = 0; i < 5; ++i) {
    const float u = (static_cast<float>(i) - 2.f) / 2.f;
    p.circle(0.0225f * (static_cast<float>(i) - 2.f), chY + 0.044f + 0.013f * (1.f - u * u), 0.0075f * accC, coal);
  }
  if (carrot_) {
    TransformScope ns(dc_);
    ns.set(Xf(0.09f * accC, 0.09f * accC, 0.12f, -0.004f, chY + 0.012f));
    p.fill(carrot_.Get(), Hex(0xF28A25));
    p.line(0.25f, -0.05f, 0.6f, -0.01f, Hex(0xB85A12, 0.7f), 0.035f);
    p.line(0.18f, 0.07f, 0.5f, 0.07f, Hex(0xB85A12, 0.6f), 0.03f);
    p.stroke(carrot_.Get(), Hex(0xA04A0E, 0.85f), 1.f * px / (0.09f * accC));
  }

  // Hat, dropped on with a little bounce and a jaunty tilt.
  TransformScope drop(dc_);
  drop.set(MakeM(1.f, 0.f, 0.f, 1.f, 0.f, -(1.f - acc) * 0.2f));
  TransformScope tilt(dc_);
  tilt.set(RotAbout(0.09f, 0.f, -0.86f));
  if (sm.hat == 1) {
    const D2D1_COLOR_F black = Hex(0x25252D);
    p.ellipse(0.f, -0.862f, 0.108f, 0.017f, Hex(0x2F2F38));
    p.ringE(0.f, -0.862f, 0.108f, 0.017f, Hex(0x0E0E12, 0.8f), 1.f * px);
    p.rounded(-0.063f, -0.995f, 0.063f, -0.866f, 0.012f, black);
    p.rect(-0.063f, -0.896f, 0.063f, -0.872f, Hex(DecorColorRgb(ModIdx(sm.hatColor, kDecorColorCount))));
    p.rect(-0.014f, -0.896f, 0.014f, -0.872f, Hex(0xE8B93E));
    p.rect(-0.046f, -0.985f, -0.036f, -0.905f, Hex(0xFFFFFF, 0.14f));
    p.ellipse(0.f, -0.995f, 0.063f, 0.011f, Hex(0x383842));
    p.roundedStroke(-0.063f, -0.995f, 0.063f, -0.866f, 0.012f, Hex(0x0E0E12, 0.8f), 1.f * px);
  } else if (sm.hat == 2) {
    const D2D1_COLOR_F sc = Hex(DecorColorRgb(ModIdx(sm.hatColor, kDecorColorCount)));
    const D2D1_COLOR_F edge = Fade(Darken(sc, 0.55f), 0.9f);
    {  // knitted dome (unit half-ellipse) with ribs
      TransformScope ds(dc_);
      ds.set(MakeM(0.105f, 0.f, 0.f, 0.09f, 0.f, -0.85f));
      p.fill(beanie_.Get(), sc);
      for (int i = -2; i <= 2; ++i) {
        const float bx = 0.4f * static_cast<float>(i);
        const float tx = 0.55f * bx;
        p.line(tx, -0.97f * std::sqrt(1.f - tx * tx), bx, -0.04f, Darken(sc, 0.82f), 0.08f);
      }
      p.stroke(beanie_.Get(), edge, 1.f * px / 0.09f);
    }
    p.rounded(-0.113f, -0.872f, 0.113f, -0.836f, 0.014f, Lighten(sc, 0.35f));
    for (int i = -3; i <= 3; ++i) {
      const float rx = 0.03f * static_cast<float>(i);
      p.line(rx, -0.868f, rx, -0.84f, Darken(sc, 0.8f), 0.007f);
    }
    p.roundedStroke(-0.113f, -0.872f, 0.113f, -0.836f, 0.014f, edge, 1.f * px);
    shadedEllipse(GradKind::Shade, Lighten(sc, 0.55f), 0.f, -0.953f, 0.03f, 0.03f, -0.01f, -0.012f);
    p.ring(0.f, -0.953f, 0.03f, edge, 1.f * px);
  } else if (sm.hat == 3) {
    p.fill(bucket_.Get(), Hex(0x8E9AAA));
    p.rect(-0.062f, -0.925f, 0.062f, -0.905f, Hex(DecorColorRgb(ModIdx(sm.hatColor, kDecorColorCount))));
    p.rect(-0.062f, -0.92f, -0.05f, -0.85f, Hex(0xFFFFFF, 0.16f));
    p.line(-0.08f, -0.94f, 0.08f, -0.94f, Hex(0x4C5666, 0.7f), 0.006f);
    p.line(-0.074f, -0.905f, 0.074f, -0.905f, Hex(0x4C5666, 0.7f), 0.006f);
    p.stroke(bucket_.Get(), Hex(0x3E4756, 0.9f), 1.1f * px);
    p.ellipse(0.f, -0.99f, 0.094f, 0.013f, Hex(0xB4BFCC));
    p.ellipse(0.f, -0.99f, 0.079f, 0.008f, Hex(0x3A4252));
    p.ringE(0.f, -0.99f, 0.094f, 0.013f, Hex(0x3E4756, 0.9f), 1.f * px);
  }
}

// ---------------------------------------------------------------------------
// A snowman breaking apart after the gift train hits it

void DecorRenderer::DrawSnowmanBroken(const DecorFrame& f, const SnowmanDecor& sm, D2D1_POINT_2F base,
                                      float height, double t, int dir, uint32_t seed) {
  (void)f;  // theme-independent: the parts keep the snowman's own colours
  if (!dc_ || height < 4.f) return;
  ensureGeometry();
  const float px = 1.f / height;
  const D2D1_COLOR_F snowC = Hex(0xF8FBFF);
  const D2D1_COLOR_F outline = Hex(0x7F96BE, 0.85f);
  const D2D1_COLOR_F coal = Hex(0x24242C);
  const float rb = 0.200f, rm = 0.150f, rh = 0.112f;
  const float cbY = -rb, cmY = -0.515f, chY = -0.747f;
  const D2D1_COLOR_F scarfCol = Hex(DecorColorRgb(ModIdx(sm.scarf, kDecorColorCount)));
  const D2D1_COLOR_F hatCol = Hex(DecorColorRgb(ModIdx(sm.hatColor, kDecorColorCount)));
  const Painter p(dc_, brush_, roundCap_.Get());

  // Places one part: its unit-space pivot lands at base + pose, rotated.
  auto drawPart = [&](KnockPart part, auto&& body) {
    const int i = static_cast<int>(part);
    const PartPose pose = KnockPartAt(t, dir, seed, part, height);  // height = real floor for the fall
    const float pivotX = kPartPivotX[i] * height;
    const float pivotY = kPartPivotY[i] * height;
    const float c = std::cos(pose.rot), sn = std::sin(pose.rot);
    const float tx = base.x + pivotX + pose.dx - (pivotX * c - pivotY * sn);
    const float ty = base.y + pivotY + pose.dy - (pivotX * sn + pivotY * c);
    TransformScope ts(dc_);
    ts.set(Xf(height, height, pose.rot, tx, ty));
    body();
  };

  auto ball = [&](KnockPart part, float cy, float r) {
    drawPart(part, [&]() {
      shadedEllipse(GradKind::Snow, snowC, 0.f, cy, r, r, -0.30f * r, -0.36f * r);
      p.ring(0.f, cy, r, outline, 1.1f * px);
    });
  };

  auto arm = [&](KnockPart part, float x0, float y0, float x1, float y1) {
    drawPart(part, [&]() {
      const D2D1_COLOR_F wood = Hex(0x6A4426);
      const float w = std::max(0.014f, 1.7f * px);
      p.line(x0, y0, x1, y1, wood, w);
      const float dx = x1 - x0, dy = y1 - y0;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 1e-4f) return;
      const float ux = dx / len, uy = dy / len;
      auto twig = [&](float sx, float sy, float ang, float l) {
        const float c = std::cos(ang), sn = std::sin(ang);
        p.line(sx, sy, sx + (ux * c - uy * sn) * l, sy + (ux * sn + uy * c) * l, wood, w * 0.75f);
      };
      twig(x0 + dx * 0.55f, y0 + dy * 0.55f, -0.8f, 0.05f);
      twig(x1, y1, 0.55f, 0.032f);
      twig(x1, y1, -0.45f, 0.032f);
    });
  };

  arm(KnockPart::ArmLeft, -0.135f, cmY - 0.005f, -0.250f, cmY - 0.075f);
  arm(KnockPart::ArmRight, 0.135f, cmY - 0.005f, 0.250f, cmY - 0.115f);
  ball(KnockPart::Bottom, cbY, rb);
  ball(KnockPart::Middle, cmY, rm);
  ball(KnockPart::Top, chY, rh);

  drawPart(KnockPart::Buttons, [&]() {
    for (int i = -1; i <= 1; ++i) {
      const float by = cmY + 0.047f * static_cast<float>(i);
      p.circle(0.f, by, 0.0155f, coal);
      p.circle(-0.004f, by - 0.004f, 0.0155f * 0.3f, Hex(0xFFFFFF, 0.45f));
    }
  });

  drawPart(KnockPart::Scarf, [&]() {
    const D2D1_COLOR_F edge = Fade(Darken(scarfCol, 0.55f), 0.9f);
    p.rounded(0.04f, -0.64f, 0.10f, -0.50f, 0.014f, scarfCol);
    p.roundedStroke(0.04f, -0.64f, 0.10f, -0.50f, 0.014f, edge, 1.f * px);
    p.line(0.045f, -0.508f, 0.095f, -0.508f, Lighten(scarfCol, 0.5f), 0.012f);
    for (int i = 0; i < 4; ++i) {
      const float fx = 0.05f + 0.014f * static_cast<float>(i);
      p.line(fx, -0.502f, fx, -0.488f, Darken(scarfCol, 0.8f), 0.006f);
    }
    p.rounded(-0.115f, -0.676f, 0.115f, -0.626f, 0.025f, scarfCol);
    p.rounded(-0.115f, -0.676f, 0.115f, -0.658f, 0.018f, Hex(0xFFFFFF, 0.14f));
    p.line(-0.05f, -0.668f, -0.05f, -0.634f, Lighten(scarfCol, 0.5f), 0.011f);
    p.line(0.012f, -0.668f, 0.012f, -0.634f, Lighten(scarfCol, 0.5f), 0.011f);
    p.roundedStroke(-0.115f, -0.676f, 0.115f, -0.626f, 0.025f, edge, 1.f * px);
  });

  drawPart(KnockPart::Face, [&]() {
    glow(Hex(0xFF8DA0), -0.066f, chY + 0.026f, 0.032f, 0.026f, 0.5f);
    glow(Hex(0xFF8DA0), 0.066f, chY + 0.026f, 0.032f, 0.026f, 0.5f);
    for (int side = -1; side <= 1; side += 2) {
      const float ex = 0.04f * static_cast<float>(side);
      p.circle(ex, chY - 0.016f, 0.0125f, coal);
      p.circle(ex - 0.003f, chY - 0.02f, 0.004f, Hex(0xFFFFFF, 0.6f));
    }
    for (int i = 0; i < 5; ++i) {
      const float u = (static_cast<float>(i) - 2.f) / 2.f;
      p.circle(0.0225f * (static_cast<float>(i) - 2.f), chY + 0.044f + 0.013f * (1.f - u * u), 0.0075f, coal);
    }
    if (carrot_) {
      TransformScope ns(dc_);
      ns.set(Xf(0.09f, 0.09f, 0.12f, -0.004f, chY + 0.012f));
      p.fill(carrot_.Get(), Hex(0xF28A25));
      p.line(0.25f, -0.05f, 0.6f, -0.01f, Hex(0xB85A12, 0.7f), 0.035f);
      p.line(0.18f, 0.07f, 0.5f, 0.07f, Hex(0xB85A12, 0.6f), 0.03f);
      p.stroke(carrot_.Get(), Hex(0xA04A0E, 0.85f), 1.f * px / 0.09f);
    }
  });

  drawPart(KnockPart::Hat, [&]() {
    if (sm.hat == 1) {
      const D2D1_COLOR_F black = Hex(0x25252D);
      p.ellipse(0.f, -0.862f, 0.108f, 0.017f, Hex(0x2F2F38));
      p.ringE(0.f, -0.862f, 0.108f, 0.017f, Hex(0x0E0E12, 0.8f), 1.f * px);
      p.rounded(-0.063f, -0.995f, 0.063f, -0.866f, 0.012f, black);
      p.rect(-0.063f, -0.896f, 0.063f, -0.872f, hatCol);
      p.rect(-0.014f, -0.896f, 0.014f, -0.872f, Hex(0xE8B93E));
      p.rect(-0.046f, -0.985f, -0.036f, -0.905f, Hex(0xFFFFFF, 0.14f));
      p.ellipse(0.f, -0.995f, 0.063f, 0.011f, Hex(0x383842));
      p.roundedStroke(-0.063f, -0.995f, 0.063f, -0.866f, 0.012f, Hex(0x0E0E12, 0.8f), 1.f * px);
    } else if (sm.hat == 2) {
      const D2D1_COLOR_F edge = Fade(Darken(hatCol, 0.55f), 0.9f);
      {
        TransformScope ds(dc_);
        ds.set(MakeM(0.105f, 0.f, 0.f, 0.09f, 0.f, -0.85f));
        p.fill(beanie_.Get(), hatCol);
        p.stroke(beanie_.Get(), edge, 1.f * px / 0.09f);
      }
      p.rounded(-0.113f, -0.872f, 0.113f, -0.836f, 0.014f, Lighten(hatCol, 0.35f));
      p.roundedStroke(-0.113f, -0.872f, 0.113f, -0.836f, 0.014f, edge, 1.f * px);
      shadedEllipse(GradKind::Shade, Lighten(hatCol, 0.55f), 0.f, -0.953f, 0.03f, 0.03f, -0.01f, -0.012f);
      p.ring(0.f, -0.953f, 0.03f, edge, 1.f * px);
    } else if (sm.hat == 3) {
      p.fill(bucket_.Get(), Hex(0x8E9AAA));
      p.rect(-0.062f, -0.925f, 0.062f, -0.905f, hatCol);
      p.rect(-0.062f, -0.92f, -0.05f, -0.85f, Hex(0xFFFFFF, 0.16f));
      p.line(-0.08f, -0.94f, 0.08f, -0.94f, Hex(0x4C5666, 0.7f), 0.006f);
      p.line(-0.074f, -0.905f, 0.074f, -0.905f, Hex(0x4C5666, 0.7f), 0.006f);
      p.stroke(bucket_.Get(), Hex(0x3E4756, 0.9f), 1.1f * px);
      p.ellipse(0.f, -0.99f, 0.094f, 0.013f, Hex(0xB4BFCC));
      p.ellipse(0.f, -0.99f, 0.079f, 0.008f, Hex(0x3A4252));
      p.ringE(0.f, -0.99f, 0.094f, 0.013f, Hex(0x3E4756, 0.9f), 1.f * px);
    }
  });
}

// ---------------------------------------------------------------------------
// Santa peeking in from a monitor edge

void DecorRenderer::DrawSanta(const DecorFrame& f, const SantaState& s) {
  if (!dc_ || s.peek <= 0.002f) return;
  ensureGeometry();
  const float R = 44.f * (s.scale > 0.f ? s.scale : 1.f);
  const float dirX = s.fromRight ? -1.f : 1.f;  // direction into the screen
  const float offset = lerpf(-2.5f, 0.5f, clampf(s.peek, 0.f, 1.f));
  const float cx = s.edgeX + dirX * offset * R;
  const float px = 1.f / R;
  const float wave = f.reducedMotion ? 0.f : s.wave;
  const float blink = clampf(s.blink, 0.f, 1.f);

  TransformScope ts(dc_);
  ts.set(MakeM(dirX * R, 0.f, 0.f, R, cx, s.y));
  const Painter p(dc_, brush_, roundCap_.Get());

  const D2D1_COLOR_F skin = Hex(0xF7CDAA);
  const D2D1_COLOR_F skinEdge = Hex(0xB9805E, 0.85f);
  const D2D1_COLOR_F red = Hex(0xD22A33);
  const D2D1_COLOR_F redEdge = Hex(0x7E1520);
  const D2D1_COLOR_F fur = Hex(0xFFFFFF);
  const D2D1_COLOR_F furEdge = Hex(0x93A6C8, 0.85f);

  // Ears and face.
  p.circle(-1.0f, 0.1f, 0.2f, skin);
  p.circle(1.0f, 0.1f, 0.2f, skin);
  p.ring(-1.0f, 0.1f, 0.2f, skinEdge, 1.f * px);
  p.ring(1.0f, 0.1f, 0.2f, skinEdge, 1.f * px);
  shadedEllipse(GradKind::Soft, skin, 0.f, 0.f, 1.f, 1.f, -0.3f, -0.35f);
  p.ring(0.f, 0.f, 1.f, skinEdge, 1.2f * px);
  glow(Hex(0xFF7A8A), -0.56f, 0.2f, 0.34f, 0.28f, 0.6f);
  glow(Hex(0xFF7A8A), 0.56f, 0.2f, 0.34f, 0.28f, 0.6f);

  // Beard and moustache.
  shadedGeometry(beard_.Get(), GradKind::Snow, fur, 0.f, 0.9f, 1.3f, 1.3f, -0.2f, -0.4f);
  p.stroke(beard_.Get(), furEdge, 1.1f * px);
  for (int side = -1; side <= 1; side += 2) {
    const float mx = 0.3f * static_cast<float>(side);
    TransformScope ms(dc_);
    ms.set(RotAbout(0.22f * static_cast<float>(side), mx, 0.4f));
    shadedEllipse(GradKind::Snow, fur, mx, 0.4f, 0.38f, 0.2f, -0.08f, -0.06f);
    p.ringE(mx, 0.4f, 0.38f, 0.2f, furEdge, 1.f * px);
  }
  p.ellipse(0.f, 0.68f, 0.14f, 0.075f, Hex(0x5A1620));
  p.ellipse(0.f, 0.72f, 0.075f, 0.035f, Hex(0xE0606E));

  // Nose, eyes, brows.
  shadedEllipse(GradKind::Soft, Hex(0xF2907F), 0.f, 0.17f, 0.18f, 0.17f, -0.05f, -0.06f);
  p.ring(0.f, 0.17f, 0.18f, Hex(0xA8574C, 0.8f), 1.f * px);
  for (int side = -1; side <= 1; side += 2) {
    const float ex = 0.38f * static_cast<float>(side);
    const float ry = 0.115f * (1.f - blink) + 0.018f;
    p.ellipse(ex, -0.12f, 0.085f, ry, Hex(0x1E2A44));
    if (blink < 0.5f) p.circle(ex - 0.03f, -0.12f - 0.04f * (1.f - blink), 0.03f, Hex(0xFFFFFF, 0.9f));
    TransformScope bs(dc_);
    bs.set(RotAbout(0.14f * static_cast<float>(side), ex, -0.36f));
    p.ellipse(ex, -0.36f, 0.23f, 0.085f, fur);
    p.ringE(ex, -0.36f, 0.23f, 0.085f, furEdge, 1.f * px);
  }

  {  // waving arm (upper arm hidden beyond the edge) with a mitten
    const float ex = 1.25f, ey = 2.2f;
    p.limb(-1.8f, 2.6f, ex, ey, 0.6f, 1.2f * px, red, redEdge);
    TransformScope as(dc_);
    as.set(Xf(1.f, 1.f, wave, ex, ey));
    p.limb(0.f, 0.f, 0.f, -0.95f, 0.58f, 1.2f * px, red, redEdge);
    p.circle(0.f, 0.f, 0.29f, red);
    p.rounded(-0.4f, -1.14f, 0.4f, -0.9f, 0.12f, fur);
    p.roundedStroke(-0.4f, -1.14f, 0.4f, -0.9f, 0.12f, furEdge, 1.f * px);
    p.ellipse(-0.3f, -1.32f, 0.14f, 0.2f, Hex(0x3A2A2C));
    p.circle(0.f, -1.42f, 0.34f, Hex(0x3A2A2C));
    p.circle(0.1f, -1.52f, 0.12f, Hex(0xFFFFFF, 0.16f));
  }

  // Hat: red cone with drooping tip, fur trim and pompom.
  shadedGeometry(santaHat_.Get(), GradKind::Shade, red, 0.4f, -1.2f + kHatDy, 1.5f, 1.2f, -0.3f, -0.3f);
  p.stroke(santaHat_.Get(), redEdge, 1.2f * px);
  p.rounded(-1.13f, kTrimTop, 1.13f, kTrimBottom, 0.22f, fur);
  p.rounded(-1.13f, kTrimBottom - 0.16f, 1.13f, kTrimBottom, 0.12f, Hex(0xE4ECFA, 0.7f));
  p.roundedStroke(-1.13f, kTrimTop, 1.13f, kTrimBottom, 0.22f, furEdge, 1.f * px);
  shadedEllipse(GradKind::Snow, fur, kPomX, kPomY, 0.28f, 0.28f, -0.08f, -0.1f);
  for (int i = 0; i < 6; ++i) {
    const float a = static_cast<float>(i) * kPi / 3.f + 0.3f;
    p.circle(kPomX + 0.23f * std::cos(a), kPomY + 0.23f * std::sin(a), 0.1f, fur);
  }
  shadedEllipse(GradKind::Snow, fur, kPomX, kPomY, 0.22f, 0.22f, -0.06f, -0.08f);
}

// ---------------------------------------------------------------------------
// Gift train

void DecorRenderer::DrawTrain(const DecorFrame& f, const TrainState& t) {
  if (!dc_) return;
  ensureGeometry();
  const Theme& th = ThemeOf(f);
  const float len = t.length > 1.f ? t.length : 330.f * (t.scale > 0.f ? t.scale : 1.f);
  const float s = len / kTrainDesignLen;
  const float m = t.dir < 0 ? -1.f : 1.f;
  const float px = 1.f / s;

  TransformScope ts(dc_);
  ts.set(MakeM(m * s, 0.f, 0.f, s, t.x, t.groundY));
  const Painter p(dc_, brush_, roundCap_.Get());

  const D2D1_COLOR_F red = Hex(0xC8312F), redDark = Hex(0x7E1B22);
  const D2D1_COLOR_F black = Hex(0x2B2B34), blackEdge = Hex(0x12121A);
  const D2D1_COLOR_F gold = Hex(0xE8B93E), goldDark = Hex(0x9C7218);
  const int no = static_cast<int>(th.ornaments.size());
  auto orn = [&](int i) { return ToD2D(th.ornaments[static_cast<size_t>(ModIdx(i, no))]); };

  auto wheel = [&](float cx, float r, float ang, const D2D1_COLOR_F& disc) {
    const float cy = -r;
    p.circle(cx, cy, r, Hex(0x1E1E26));
    p.circle(cx, cy, r - 1.6f, gold);
    p.circle(cx, cy, r - 2.8f, disc);
    for (int k = 0; k < 3; ++k) {
      const float a = ang + static_cast<float>(k) * kPi / 3.f;
      const float dx = std::cos(a) * (r - 2.8f), dy = std::sin(a) * (r - 2.8f);
      p.line(cx - dx, cy - dy, cx + dx, cy + dy, Hex(0x1E1E26, 0.85f), 1.5f);
    }
    p.circle(cx, cy, r * 0.28f, gold);
    p.circle(cx, cy, r * 0.12f, Hex(0x1E1E26));
  };

  auto gift = [&](float x, float baseY, float w, float h, const D2D1_COLOR_F& c) {
    const float l = x - w * 0.5f, r = x + w * 0.5f, top = baseY - h;
    const D2D1_COLOR_F ribbon = (c.r + c.g + c.b > 2.1f) ? Hex(0xD8283A) : Hex(0xFFF6DD);
    p.rect(l, top, r, baseY, c);
    p.rect(r - w * 0.22f, top, r, baseY, Hex(0x000000, 0.14f));
    p.rect(l - 1.f, top, r + 1.f, top + 3.f, Lighten(c, 0.25f));
    p.rect(x - 2.f, top, x + 2.f, baseY, ribbon);
    p.rect(l, baseY - h * 0.5f - 2.f, r, baseY - h * 0.5f + 2.f, Fade(ribbon, 0.9f));
    p.ellipse(x - 3.2f, top - 2.2f, 3.6f, 2.4f, ribbon);
    p.ellipse(x + 3.2f, top - 2.2f, 3.6f, 2.4f, ribbon);
    p.circle(x, top - 1.2f, 1.9f, Darken(ribbon, 0.82f));
    p.line(l, baseY, r, baseY, Fade(Darken(c, 0.5f), 0.9f), 1.f * px);
  };

  // Rail under the wheels.
  p.line(-kTrainDesignLen - 24.f, 1.8f, 24.f, 1.8f, Hex(0x3A3F4A, 0.55f), 3.f);

  // Wagons: B (rear) then A (behind the engine).
  const float wagonW = 90.f;
  const float xA = -216.f, xB = -314.f;
  for (int wi = 1; wi >= 0; --wi) {
    const float wx = wi == 0 ? xA : xB;
    const D2D1_COLOR_F body = wi == 0 ? Hex(0x2F8F5B) : Hex(0x3B6FB6);
    if (wi == 0) {  // elf riding on the first wagon, waving
      const float ex = wx + 22.f;
      const float wv = f.reducedMotion ? 0.7f : 0.7f + 0.5f * std::sin(static_cast<float>(t.t) * 6.f);
      const D2D1_COLOR_F elfSkin = Hex(0xF5C9A6);
      const D2D1_COLOR_F elfGreen = Hex(0x2E8B4A), elfGreenEdge = Hex(0x14502B);
      const D2D1_COLOR_F elfRed = Hex(0xD22A33);
      p.limb(ex - 7.f, -63.f, ex - 11.f, -50.f, 4.5f, 0.8f, elfRed, redDark);
      p.rounded(ex - 8.f, -67.f, ex + 8.f, -44.f, 4.f, elfGreen);
      p.roundedStroke(ex - 8.f, -67.f, ex + 8.f, -44.f, 4.f, elfGreenEdge, 1.f);
      p.rect(ex - 8.f, -58.f, ex + 8.f, -54.f, elfRed);
      p.rect(ex - 2.5f, -58.5f, ex + 2.5f, -53.5f, gold);
      p.rounded(ex - 7.f, -69.f, ex + 7.f, -64.f, 2.f, elfRed);
      for (int i = -1; i <= 1; ++i) p.circle(ex + 4.f * static_cast<float>(i), -66.5f, 1.2f, Hex(0xFFFFFF));
      p.limb(ex + 7.f, -63.f, ex + 7.f + 15.f * std::sin(wv), -63.f - 15.f * std::cos(wv), 4.5f, 0.8f, elfRed, redDark);
      p.circle(ex + 7.f + 17.f * std::sin(wv), -63.f - 17.f * std::cos(wv), 3.4f, elfSkin);
      for (int side = -1; side <= 1; side += 2) {  // pointy ears
        TransformScope es(dc_);
        es.set(MakeM(6.5f * static_cast<float>(side), 0.f, 0.f, 6.5f, ex + 7.5f * static_cast<float>(side), -75.5f));
        p.fill(elfEar_.Get(), elfSkin);
        p.stroke(elfEar_.Get(), Hex(0xA8704E, 0.8f), 1.f * px / 6.5f);
      }
      shadedEllipse(GradKind::Soft, elfSkin, ex, -76.f, 8.5f, 8.5f, -2.5f, -3.f);
      p.ring(ex, -76.f, 8.5f, Hex(0xA8704E, 0.8f), 1.f);
      glow(Hex(0xFF7A8A), ex - 5.2f, -73.5f, 3.4f, 2.8f, 0.55f);
      glow(Hex(0xFF7A8A), ex + 5.2f, -73.5f, 3.4f, 2.8f, 0.55f);
      p.circle(ex - 3.f, -77.5f, 1.15f, Hex(0x1E2A44));
      p.circle(ex + 3.f, -77.5f, 1.15f, Hex(0x1E2A44));
      p.circle(ex, -75.f, 1.1f, Hex(0xF2907F));
      p.ellipse(ex, -72.f, 2.5f, 1.4f, Hex(0x7E1B22));
      {  // hat with a white band and a golden bell
        TransformScope hs(dc_);
        hs.set(MakeM(9.f, 0.f, 0.f, 9.f, ex - 0.5f, -83.f));
        p.fill(elfHat_.Get(), elfGreen);
        p.stroke(elfHat_.Get(), elfGreenEdge, 1.f * px / 9.f);
      }
      p.rounded(ex - 9.8f, -86.f, ex + 9.2f, -81.f, 2.5f, Hex(0xFFFFFF));
      p.roundedStroke(ex - 9.8f, -86.f, ex + 9.2f, -81.f, 2.5f, Hex(0x93A6C8, 0.8f), 1.f);
      p.circle(ex + 7.6f, -104.f, 2.6f, gold);
      p.circle(ex + 6.9f, -104.8f, 0.9f, Hex(0xFFFFFF, 0.7f));
      p.ring(ex + 7.6f, -104.f, 2.6f, goldDark, 0.8f);
    }
    if (wi == 0) {
      gift(wx + 56.f, -50.f, 20.f, 24.f, orn(0));
      gift(wx + 77.f, -50.f, 22.f, 16.f, orn(2));
    } else {
      gift(wx + 18.f, -50.f, 22.f, 18.f, orn(3));
      gift(wx + 46.f, -50.f, 26.f, 26.f, orn(1));
      gift(wx + 46.f, -76.f, 14.f, 12.f, orn(4));
      gift(wx + 74.f, -50.f, 20.f, 20.f, orn(5));
    }
    p.rounded(wx + 2.f, -49.f, wx + wagonW - 2.f, -24.f, 2.f, body);
    p.line(wx + 3.f, -41.f, wx + wagonW - 3.f, -41.f, Hex(0x000000, 0.2f), 1.2f);
    p.line(wx + 3.f, -33.f, wx + wagonW - 3.f, -33.f, Hex(0x000000, 0.2f), 1.2f);
    p.rect(wx + wagonW * 0.62f, -49.f, wx + wagonW - 2.f, -24.f, Hex(0x000000, 0.12f));
    p.roundedStroke(wx + 2.f, -49.f, wx + wagonW - 2.f, -24.f, 2.f, Fade(Darken(body, 0.45f), 0.9f), 1.f);
    p.rounded(wx, -52.f, wx + wagonW, -47.f, 2.f, gold);
    p.rounded(wx + 2.f, -23.f, wx + wagonW - 2.f, -14.f, 2.f, black);
    p.ellipse(wx + wagonW * 0.5f, -52.f, 20.f, 3.2f, Hex(0xF4F8FF, 0.95f));  // snow on the rim
    wheel(wx + 18.f, 9.f, t.wheel * 15.f / 9.f, red);
    wheel(wx + wagonW - 18.f, 9.f, t.wheel * 15.f / 9.f, red);
  }
  p.line(-118.f, -20.f, -126.f, -20.f, black, 4.f);
  p.line(-216.f, -20.f, -224.f, -20.f, black, 4.f);

  // Locomotive.
  p.rounded(-118.f, -34.f, 0.f, -18.f, 3.f, red);
  p.rect(-118.f, -34.f, 0.f, -31.f, Hex(0xFFFFFF, 0.12f));
  p.roundedStroke(-118.f, -34.f, 0.f, -18.f, 3.f, redDark, 1.f);
  p.rounded(-76.f, -76.f, -4.f, -32.f, 14.f, black);
  p.rounded(-72.f, -73.f, -8.f, -66.f, 3.f, Hex(0xFFFFFF, 0.13f));
  for (int i = 0; i < 3; ++i) {
    const float bx = -66.f + 20.f * static_cast<float>(i);
    p.rect(bx, -76.f, bx + 5.f, -32.f, gold);
    p.rect(bx + 3.5f, -76.f, bx + 5.f, -32.f, Hex(0x000000, 0.18f));
  }
  p.rect(-11.f, -75.f, -5.f, -33.f, Hex(0x17171F));
  p.roundedStroke(-76.f, -76.f, -4.f, -32.f, 14.f, blackEdge, 1.f);
  p.rounded(-27.f, -92.f, -13.f, -72.f, 1.5f, black);
  p.rounded(-30.f, -98.f, -10.f, -90.f, 2.f, gold);
  p.rect(-30.f, -92.f, -10.f, -90.f, goldDark);
  p.ellipse(-20.f, -98.f, 10.f, 2.f, Hex(0x3A3A44));
  p.ellipse(-50.f, -76.f, 9.f, 8.f, gold);
  p.ellipse(-52.f, -78.f, 3.5f, 2.5f, Hex(0xFFFFFF, 0.5f));
  p.ringE(-50.f, -76.f, 9.f, 8.f, goldDark, 1.f);
  p.rounded(-118.f, -98.f, -74.f, -32.f, 2.f, red);
  p.rect(-84.f, -98.f, -74.f, -32.f, Hex(0x000000, 0.14f));
  p.roundedStroke(-118.f, -98.f, -74.f, -32.f, 2.f, redDark, 1.f);
  p.rounded(-122.f, -104.f, -70.f, -96.f, 2.f, Hex(0x3A1A20));
  p.ellipse(-96.f, -104.f, 25.f, 4.f, Hex(0xF4F8FF, 0.95f));
  p.rounded(-110.f, -88.f, -84.f, -58.f, 3.f, Hex(0xFFE08A));
  p.line(-97.f, -88.f, -97.f, -58.f, Hex(0x3A1A20), 2.f);
  p.line(-110.f, -73.f, -84.f, -73.f, Hex(0x3A1A20), 2.f);
  p.roundedStroke(-110.f, -88.f, -84.f, -58.f, 3.f, Hex(0x3A1A20), 2.f);
  p.rect(-118.f, -50.f, -74.f, -47.f, gold);
  p.rounded(-4.f, -30.f, 5.f, -14.f, 2.f, gold);  // bumper
  p.circle(0.f, -58.f, 5.f, gold);
  p.circle(0.f, -58.f, 3.2f, Hex(0xFFF3C4));
  glow(Hex(0xFFE9A8), 6.f, -58.f, 34.f, 22.f, 0.5f);
  {
    const float r1 = 15.f, r2 = 9.f;
    const float a = t.wheel;
    const float x1 = -96.f, x2 = -62.f;
    wheel(x1, r1, a, red);
    wheel(x2, r1, a, red);
    wheel(-30.f, r2, a * r1 / r2, red);
    wheel(-12.f, r2, a * r1 / r2, red);
    const float pr = 0.62f * (r1 - 2.8f);
    const float p1x = x1 + pr * std::cos(a), p1y = -r1 + pr * std::sin(a);
    const float p2x = x2 + pr * std::cos(a), p2y = -r1 + pr * std::sin(a);
    p.line(p1x, p1y, p2x, p2y, gold, 2.6f);
    p.circle(p1x, p1y, 1.8f, goldDark);
    p.circle(p2x, p2y, 1.8f, goldDark);
  }

  // Smoke puffs: stateless, phase-shifted copies of one rising puff.
  for (int i = 0; i < 6; ++i) {
    const double u = t.t + 0.4 * static_cast<double>(i);
    const double cyc = std::floor(u / 2.4);
    const float age = static_cast<float>(u - cyc * 2.4);
    const float a = age / 2.4f;
    const int cycle = static_cast<int>(static_cast<long long>(cyc) % 1000003LL);
    const float jx = Hash01(cycle * 7 + i) - 0.5f, jy = Hash01(cycle * 13 + i * 5) - 0.5f;
    const float x = -20.f - 55.f * age + 6.f * jx;
    const float y = -100.f - 26.f * age + 4.f * std::sin(age * 3.f + static_cast<float>(i)) + 4.f * jy;
    const float r = 6.f + 17.f * a;
    const float alpha = 0.75f * std::pow(1.f - a, 1.4f) * std::min(1.f, age / 0.15f);
    glow(Hex(0xDDE3EE), x, y, r, r * 0.9f, alpha);
  }
}

// ---------------------------------------------------------------------------
// Yeti

void DecorRenderer::DrawYeti(const DecorFrame& f, const YetiState& y) {
  (void)f;
  if (!dc_) return;
  ensureGeometry();
  const float sc = y.scale > 0.f ? y.scale : 1.f;
  const float H = 110.f * sc;
  const float dir = y.dir < 0 ? -1.f : 1.f;
  const float px = 1.f / H;
  const int phase = y.phase;
  const bool walking = phase == 0 || phase == 5;
  const bool lookUp = phase == 1 || phase == 2;
  const float armsUp = (phase == 2 || phase == 3) ? 1.f : 0.f;
  const float shrug = clampf(y.shrug, 0.f, 1.f);
  const float bob = walking ? std::fabs(std::sin(y.walk)) * (3.f / 110.f) : 0.f;

  {  // ground shadow, shrinks while airborne
    const float lift = std::max(0.f, y.lift);
    const float k = 1.f / (1.f + lift / H * 1.5f);
    glow(Hex(0x0B1428), y.x, y.groundY - 0.01f * H, 0.40f * H * k, 0.07f * H * k, 0.4f * k);
  }

  TransformScope ts(dc_);
  ts.set(MakeM(dir * H, 0.f, 0.f, H, y.x, y.groundY - std::max(0.f, y.lift)));
  const Painter p(dc_, brush_, roundCap_.Get());

  const D2D1_COLOR_F fur = Hex(0xF4F8FD), furMid = Hex(0xDCE8F6), furEdge = Hex(0x7F97BA);
  const float ol = 1.1f * px;

  // Legs and feet (feet stay on the ground while the body bobs).
  for (int k = 0; k < 2; ++k) {
    const float side = k == 0 ? -1.f : 1.f;
    const float hx = side * 0.11f, hy = -0.30f - bob;
    float fx = hx, fy = -0.054f;
    if (walking) {
      const float ph = y.walk + (k == 0 ? 0.f : kPi);
      fx = hx + 0.09f * std::sin(ph);
      fy = -0.054f - std::max(0.f, std::cos(ph)) * 0.05f;
    } else if (armsUp > 0.f) {
      fx = hx + side * 0.05f;
      fy = -0.09f;
    }
    p.limb(hx, hy, fx, fy, 0.15f, ol, furMid, furEdge);
    p.ellipse(fx + 0.025f, fy + 0.012f, 0.098f, 0.045f, furEdge);
    p.ellipse(fx + 0.025f, fy + 0.012f, 0.09f, 0.038f, Hex(0xE6EEF8));
  }

  // Body (raised a little when shrugging).
  TransformScope bodyScope(dc_);
  bodyScope.set(MakeM(1.f, 0.f, 0.f, 1.f, 0.f, -(bob + shrug * 0.03f)));
  shadedGeometry(yetiBody_.Get(), GradKind::Snow, Hex(0xF6FAFF), -0.06f, -0.60f, 0.38f, 0.36f, -0.06f, -0.08f);
  p.stroke(yetiBody_.Get(), furEdge, 1.2f * px);
  const D2D1_COLOR_F tuft = Hex(0x93AAD0, 0.7f);
  const float chev[5][2] = {{-0.12f, -0.62f}, {0.10f, -0.60f}, {-0.02f, -0.45f}, {0.15f, -0.40f}, {-0.15f, -0.38f}};
  for (const auto& c : chev) {
    p.line(c[0] - 0.035f, c[1] - 0.025f, c[0], c[1], tuft, 1.1f * px);
    p.line(c[0], c[1], c[0] + 0.035f, c[1] - 0.025f, tuft, 1.1f * px);
  }

  // Arms: hang and swing, shrug (palms up) or reach up while airborne.
  for (int k = 0; k < 2; ++k) {
    const float side = k == 0 ? -1.f : 1.f;
    const float sx = side * 0.27f, sy = -0.67f - shrug * 0.01f;
    const float swing = walking ? 0.07f * std::sin(y.walk + (k == 0 ? 0.f : kPi) + kPi) : 0.f;
    float hx = side * 0.37f, hy = -0.34f + swing;
    hx = lerpf(hx, side * 0.47f, shrug);
    hy = lerpf(hy, -0.52f, shrug);
    hx = lerpf(hx, side * 0.30f, armsUp);
    hy = lerpf(hy, -1.0f, armsUp);
    p.limb(sx, sy, hx, hy, 0.15f, ol, fur, furEdge);
    const D2D1_COLOR_F hand = MixC(furMid, Hex(0x9CB8E0), shrug);
    float dx = hx - sx, dy = hy - sy;
    const float dl = std::sqrt(dx * dx + dy * dy);
    if (dl > 1e-4f) {
      dx /= dl;
      dy /= dl;
    }
    p.circle(hx, hy, 0.088f, furEdge);
    p.circle(hx, hy, 0.081f, hand);
    for (int i = -1; i <= 1; ++i) {
      const float fx = hx + dx * 0.06f - dy * 0.04f * static_cast<float>(i);
      const float fy = hy + dy * 0.06f + dx * 0.04f * static_cast<float>(i);
      p.circle(fx, fy, 0.027f, furEdge);
      p.circle(fx, fy, 0.022f, hand);
    }
  }

  // Head (tilts back while looking up), horns first so the fur overlaps them.
  TransformScope headScope(dc_);
  headScope.set(RotAbout(lookUp ? -0.36f : (phase == 3 ? -0.12f : 0.f), 0.f, -0.70f));
  for (int side = -1; side <= 1; side += 2) {
    TransformScope hs(dc_);
    hs.set(Xf(0.05f, 0.05f, 0.28f * static_cast<float>(side), 0.105f * static_cast<float>(side), -0.912f));
    p.fill(yetiHorn_.Get(), Hex(0xF2E7CC));
    p.stroke(yetiHorn_.Get(), Hex(0x8A7C5C, 0.9f), 1.f * px / 0.05f);
  }
  shadedGeometry(yetiHead_.Get(), GradKind::Snow, Hex(0xF6FAFF), -0.02f, -0.82f, 0.22f, 0.2f, -0.03f, -0.04f);
  p.stroke(yetiHead_.Get(), furEdge, 1.2f * px);

  const float fcx = 0.04f, fcy = -0.795f;
  shadedEllipse(GradKind::Shade, Hex(0x5B7392), fcx, fcy, 0.115f, 0.097f, -0.02f, -0.03f);
  const float lookY = lookUp ? -0.011f : 0.f;
  for (int side = -1; side <= 1; side += 2) {
    const float ex = fcx + 0.045f * static_cast<float>(side);
    p.circle(ex, fcy - 0.018f, 0.028f, Hex(0xFFFFFF));
    p.circle(ex + 0.007f, fcy - 0.018f + lookY, 0.0145f, Hex(0x141C2A));
    p.circle(ex + 0.002f, fcy - 0.024f + lookY, 0.005f, Hex(0xFFFFFF, 0.9f));
  }
  {  // brows: surprised when looking up/falling, unsure when shrugging, grumpy otherwise
    const float surprised = (lookUp || phase == 3) ? 1.f : 0.f;
    const float unsure = phase == 4 ? 1.f : 0.f;
    const float outerY = fcy - 0.062f - 0.012f * surprised - 0.004f * unsure + 0.008f * (1.f - surprised - unsure);
    const float innerY = fcy - 0.052f - 0.016f * surprised - 0.022f * unsure - 0.004f * (1.f - surprised - unsure);
    const D2D1_COLOR_F brow = Hex(0x1B2533);
    p.line(fcx - 0.078f, outerY, fcx - 0.016f, innerY, brow, 0.017f);
    p.line(fcx + 0.016f, innerY, fcx + 0.078f, outerY, brow, 0.017f);
  }
  p.ellipse(fcx + 0.014f, fcy + 0.012f, 0.02f, 0.013f, Hex(0x222C3E));
  const float mx = fcx + 0.012f, my = fcy + 0.056f;
  if (phase == 2 || phase == 3) {
    p.ellipse(mx, my, 0.03f, 0.034f, Hex(0x2B1A24));
    p.ellipse(mx, my + 0.014f, 0.017f, 0.012f, Hex(0xB85A6A));
  } else if (phase == 1) {
    p.ellipse(mx, my, 0.022f, 0.02f, Hex(0x2B1A24));
  } else if (phase == 4) {
    p.line(mx - 0.035f, my + 0.004f, mx + 0.035f, my - 0.004f, Hex(0x2B1A24), 0.012f);
  } else {
    p.ellipse(mx, my, 0.048f, 0.022f, Hex(0x2B1A24));
    p.ellipse(mx - 0.022f, my - 0.012f, 0.008f, 0.014f, Hex(0xFFFFFF));
    p.ellipse(mx + 0.022f, my - 0.012f, 0.008f, 0.014f, Hex(0xFFFFFF));
  }
}

// ---------------------------------------------------------------------------
// Decorate-mode palette

void DecorRenderer::DrawPalette(const DecorFrame& f, const PaletteLayout& layout, Tool active,
                                const wchar_t* const* labels, IDWriteTextFormat* labelFormat) {
  if (!dc_ || layout.bar.empty()) return;
  ensureGeometry();
  const float dpi = f.dpiScale > 0.f ? f.dpiScale : 1.f;
  const Painter p(dc_, brush_, roundCap_.Get());

  const RectI& bar = layout.bar;
  const float bl = static_cast<float>(bar.left), bt = static_cast<float>(bar.top);
  const float br = static_cast<float>(bar.right), bb = static_cast<float>(bar.bottom);
  const float barR = std::min(14.f * dpi, 0.5f * static_cast<float>(bar.height()));
  p.rounded(bl, bt + 4.f * dpi, br, bb + 4.f * dpi, barR, Hex(0x000000, 0.18f));
  p.rounded(bl, bt, br, bb, barR, Hex(0x101626, 0.78f));
  p.roundedStroke(bl + 0.5f, bt + 0.5f, br - 0.5f, bb - 0.5f, barR, Hex(0xFFFFFF, 0.16f), 1.f * dpi);

  for (const PaletteButton& btn : layout.buttons) {
    if (btn.rect.empty()) continue;
    const float l = static_cast<float>(btn.rect.left), t = static_cast<float>(btn.rect.top);
    const float r = static_cast<float>(btn.rect.right), b = static_cast<float>(btn.rect.bottom);
    const float w = r - l, h = b - t;
    const float rad = std::min(9.f * dpi, 0.5f * std::min(w, h));
    const bool isActive = btn.tool == active;
    D2D1_COLOR_F fillC = Hex(0xFFFFFF, 0.07f);
    if (isActive) {
      fillC = Hex(0x3A82F5, 0.92f);
    } else if (btn.tool == Tool::Done) {
      fillC = Hex(0x2EA866, 0.78f);
    }
    p.rounded(l, t, r, b, rad, fillC);
    p.roundedStroke(l + 0.5f, t + 0.5f, r - 0.5f, b - 0.5f, rad, isActive ? Hex(0xFFFFFF, 0.6f) : Hex(0xFFFFFF, 0.13f),
                    (isActive ? 1.5f : 1.f) * dpi);

    const float size = std::min(w * 0.56f, h * 0.5f);
    drawIcon(btn.tool, 0.5f * (l + r), t + 0.37f * h, size, f);

    const int idx = static_cast<int>(btn.tool);
    if (labelFormat && labels && idx >= 0 && idx < static_cast<int>(Tool::Count) && labels[idx]) {
      labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      labelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      const D2D1_RECT_F tr = D2D1::RectF(l + 2.f * dpi, t + 0.62f * h, r - 2.f * dpi, b - 2.f * dpi);
      if (auto* tb = brush_(Hex(0xFFFFFF, 0.96f)))
        dc_->DrawText(labels[idx], static_cast<UINT32>(std::wcslen(labels[idx])), labelFormat, tr, tb,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }
}

void DecorRenderer::drawIcon(Tool tool, float cx, float cy, float size, const DecorFrame& f) {
  if (size < 2.f) return;
  const Theme& th = ThemeOf(f);
  TransformScope ts(dc_);
  ts.set(MakeM(size, 0.f, 0.f, size, cx, cy));  // icon box: -0.5 .. 0.5
  const Painter p(dc_, brush_, roundCap_.Get());
  const float px = 1.f / size;
  const D2D1_COLOR_F ink = Hex(0x0A1020, 0.6f);
  auto orn = [&](int i) { return ToD2D(th.ornaments[static_cast<size_t>(ModIdx(i, static_cast<int>(th.ornaments.size())))]); };
  auto lit = [&](int i) { return ToD2D(th.lights[static_cast<size_t>(ModIdx(i, static_cast<int>(th.lights.size())))]); };
  auto scarfC = [&](int i) { return ToD2D(th.scarves[static_cast<size_t>(ModIdx(i, static_cast<int>(th.scarves.size())))]); };

  switch (tool) {
    case Tool::Tree: {
      TransformScope ls(dc_);
      ls.set(MakeM(0.98f, 0.f, 0.f, 0.98f, 0.f, 0.49f));
      const float lp = px / 0.98f;
      p.rect(-0.05f, -0.17f, 0.05f, 0.f, Hex(0x7E4F2D));
      for (int j = 2; j >= 0; --j) {
        p.fill(treeTier_[j].Get(), Hex(kTierGreen[j]));
        p.stroke(treeTier_[j].Get(), Hex(0x14502B, 0.8f), 1.f * lp);
        p.fill(treeFrost_[j].Get(), Hex(0xF4F8FF, 0.95f));
      }
      TransformScope st(dc_);
      st.set(MakeM(0.075f, 0.f, 0.f, 0.075f, 0.f, -0.93f));
      p.fill(star_.Get(), ToD2D(th.star));
      break;
    }
    case Tool::Bauble: {
      const D2D1_COLOR_F c = orn(0);
      p.rounded(-0.085f, -0.31f, 0.085f, -0.22f, 0.03f, Hex(0xE6B93C));
      p.ring(0.f, -0.36f, 0.07f, Hex(0xE6B93C), 0.03f);
      shadedEllipse(GradKind::Shade, c, 0.f, 0.08f, 0.31f, 0.31f, -0.1f, -0.12f);
      p.ring(0.f, 0.08f, 0.31f, Darken(c, 0.5f), 1.2f * px);
      p.circle(-0.11f, -0.03f, 0.075f, Hex(0xFFFFFF, 0.7f));
      break;
    }
    case Tool::Lights: {
      const float ys = 0.30f;
      for (int i = 0; i < 24; ++i) {
        const float t0 = static_cast<float>(i) / 24.f, t1 = static_cast<float>(i + 1) / 24.f;
        p.line(-0.45f + 0.9f * t0, -0.2f + ys * 4.f * t0 * (1.f - t0), -0.45f + 0.9f * t1,
               -0.2f + ys * 4.f * t1 * (1.f - t1), Hex(0xFFFFFF, 0.55f), 1.4f * px);
      }
      for (int i = 0; i < 4; ++i) {
        const float t0 = 0.12f + 0.253f * static_cast<float>(i);
        const float x = -0.45f + 0.9f * t0, y = -0.2f + ys * 4.f * t0 * (1.f - t0) + 0.07f;
        const D2D1_COLOR_F c = lit(i);
        glow(c, x, y, 0.17f, 0.17f, 0.7f);
        p.circle(x, y, 0.065f, Lighten(c, 0.25f));
        p.circle(x - 0.02f, y - 0.025f, 0.02f, Hex(0xFFFFFF, 0.7f));
      }
      break;
    }
    case Tool::CandyCane: {
      TransformScope cs(dc_);
      cs.set(MakeM(0.92f, 0.f, 0.f, 0.92f, -0.23f, 0.f));
      const float w = 0.26f, ol = px / 0.92f;
      p.stroke(candy_.Get(), Hex(0x5A1A22, 0.8f), w + 2.f * ol);
      p.stroke(candy_.Get(), Hex(0xFFFFFF), w);
      if (candyDash_ && candy_) {
        if (auto* sb = brush_(Hex(0xD8283A))) dc_->DrawGeometry(candy_.Get(), sb, w, candyDash_.Get());
      }
      break;
    }
    case Tool::Star: {
      const D2D1_COLOR_F sc = ToD2D(th.star);
      glow(sc, 0.f, 0.03f, 0.55f, 0.55f, 0.5f);
      TransformScope st(dc_);
      st.set(MakeM(0.46f, 0.f, 0.f, 0.46f, 0.f, 0.04f));
      shadedGeometry(star_.Get(), GradKind::Shade, sc, 0.f, 0.f, 1.1f, 1.1f, -0.2f, -0.25f);
      p.stroke(star_.Get(), Fade(Darken(sc, 0.55f), 0.9f), 1.2f * px / 0.46f);
      break;
    }
    case Tool::Snowman: {
      const D2D1_COLOR_F snowC = Hex(0xF8FBFF), edge = Hex(0x7F96BE, 0.9f);
      shadedEllipse(GradKind::Snow, snowC, 0.f, 0.2f, 0.27f, 0.27f, -0.08f, -0.1f);
      p.ring(0.f, 0.2f, 0.27f, edge, 1.2f * px);
      shadedEllipse(GradKind::Snow, snowC, 0.f, -0.15f, 0.2f, 0.2f, -0.06f, -0.08f);
      p.ring(0.f, -0.15f, 0.2f, edge, 1.2f * px);
      p.rounded(-0.1f, -0.5f, 0.1f, -0.33f, 0.02f, Hex(0x25252D));
      p.rounded(-0.16f, -0.355f, 0.16f, -0.315f, 0.02f, Hex(0x25252D));
      p.rect(-0.1f, -0.39f, 0.1f, -0.36f, Hex(0xC8312F));
      p.circle(-0.07f, -0.17f, 0.025f, Hex(0x24242C));
      p.circle(0.07f, -0.17f, 0.025f, Hex(0x24242C));
      p.line(0.f, -0.12f, 0.15f, -0.1f, Hex(0xF28A25), 0.045f);
      p.circle(0.f, 0.15f, 0.025f, Hex(0x24242C));
      p.circle(0.f, 0.27f, 0.025f, Hex(0x24242C));
      break;
    }
    case Tool::Hat: {
      TransformScope hs(dc_);
      const float k = 0.27f;
      hs.set(MakeM(k, 0.f, 0.f, k, -0.465f * k, 1.19f * k));
      const float lp = px / k;
      p.fill(santaHat_.Get(), Hex(0xD22A33));
      p.stroke(santaHat_.Get(), Hex(0x7E1520), 1.2f * lp);
      p.rounded(-1.13f, kTrimTop, 1.13f, kTrimBottom, 0.22f, Hex(0xFFFFFF));
      p.roundedStroke(-1.13f, kTrimTop, 1.13f, kTrimBottom, 0.22f, Hex(0x93A6C8, 0.8f), 1.f * lp);
      p.circle(kPomX, kPomY, 0.3f, Hex(0xFFFFFF));
      p.ring(kPomX, kPomY, 0.3f, Hex(0x93A6C8, 0.8f), 1.f * lp);
      break;
    }
    case Tool::Scarf: {
      const D2D1_COLOR_F sc = scarfC(0);
      TransformScope rs(dc_);
      rs.set(RotAbout(-0.2f, 0.f, 0.f));
      p.rounded(0.06f, -0.04f, 0.26f, 0.42f, 0.04f, sc);
      p.roundedStroke(0.06f, -0.04f, 0.26f, 0.42f, 0.04f, ink, 1.f * px);
      for (int i = 0; i < 4; ++i) p.line(0.09f + 0.045f * static_cast<float>(i), 0.42f, 0.09f + 0.045f * static_cast<float>(i), 0.49f, Darken(sc, 0.85f), 0.02f);
      p.rounded(-0.44f, -0.18f, 0.3f, 0.06f, 0.06f, sc);
      for (int i = 0; i < 4; ++i) p.line(-0.32f + 0.17f * static_cast<float>(i), -0.17f, -0.32f + 0.17f * static_cast<float>(i), 0.05f, Lighten(sc, 0.55f), 0.045f);
      p.roundedStroke(-0.44f, -0.18f, 0.3f, 0.06f, 0.06f, ink, 1.f * px);
      break;
    }
    case Tool::Erase: {
      TransformScope rs(dc_);
      rs.set(RotAbout(-0.6f, 0.f, 0.f));
      p.rounded(-0.4f, -0.17f, 0.4f, 0.17f, 0.06f, Hex(0xF4A3AE));
      p.rounded(0.08f, -0.17f, 0.4f, 0.17f, 0.06f, Hex(0xEDEFF7));
      p.rect(0.08f, -0.17f, 0.2f, 0.17f, Hex(0xEDEFF7));
      p.line(0.08f, -0.17f, 0.08f, 0.17f, Hex(0x4C6FD0), 0.035f);
      p.roundedStroke(-0.4f, -0.17f, 0.4f, 0.17f, 0.06f, ink, 1.2f * px);
      break;
    }
    case Tool::Done: {
      p.line(-0.3f, 0.02f, -0.08f, 0.26f, Hex(0xFFFFFF), 0.14f);
      p.line(-0.08f, 0.26f, 0.34f, -0.24f, Hex(0xFFFFFF), 0.14f);
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Free-standing decorations (placed anywhere on the desktop)

void DecorRenderer::DrawFreeDecor(const DecorFrame& f, const FreeDecor& d, D2D1_POINT_2F centre, float size,
                                  bool highlight) {
  if (!dc_ || size < 6.f) return;
  ensureGeometry();
  const float px = 1.f / size;
  const D2D1_COLOR_F c = Hex(DecorColorRgb(d.color));
  TransformScope ts(dc_);
  ts.set(Xf(size, size, 0.f, centre.x, centre.y));
  const Painter p(dc_, brush_, roundCap_.Get());

  if (highlight) p.ring(0.f, 0.f, 0.60f, Hex(0x8CD9FF, 0.75f), 3.f * px);

  switch (d.kind) {
    case FreeKind::Bauble: {
      const float r = 0.40f;
      p.rounded(-0.09f, -0.62f, 0.09f, -0.47f, 0.03f, Hex(0xE6B93C));
      p.ring(0.f, -0.62f, 0.075f, Hex(0xE6B93C), 0.9f * px);
      shadedEllipse(GradKind::Shade, c, 0.f, 0.f, r, r, -0.14f, -0.16f);
      p.ring(0.f, 0.f, r, Darken(c, 0.5f), 1.1f * px);
      p.circle(-0.15f, -0.17f, 0.10f, Hex(0xFFFFFF, 0.7f));
      break;
    }
    case FreeKind::CandyCane: {
      const float ch = 0.9f;
      TransformScope cs(dc_);
      cs.set(MakeM(ch, 0.f, 0.f, ch, -0.32f * ch, -0.5f));
      const float w = 0.25f;
      const float ol = px / ch;
      D2D1_COLOR_F stripe = c;
      if (std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}) < 0.15f) stripe = Hex(0xD8283A);
      p.stroke(candy_.Get(), Hex(0x5A1A22, 0.75f), w + 2.f * ol);
      p.stroke(candy_.Get(), Hex(0xFFFFFF), w);
      if (candyDash_ && candy_) {
        if (auto* sb = brush_(stripe)) dc_->DrawGeometry(candy_.Get(), sb, w, candyDash_.Get());
      }
      break;
    }
    case FreeKind::Star: {
      TransformScope ss(dc_);
      ss.set(MakeM(0.45f, 0.f, 0.f, 0.45f, 0.f, 0.f));
      shadedGeometry(star_.Get(), GradKind::Shade, c, 0.f, 0.f, 1.15f, 1.15f, -0.2f, -0.25f);
      p.stroke(star_.Get(), Fade(Darken(c, 0.55f), 0.9f), 1.4f * px / 0.45f);
      break;
    }
    case FreeKind::Hat: {
      if (d.variant == 2) {
        const D2D1_COLOR_F edge = Fade(Darken(c, 0.55f), 0.9f);
        TransformScope ds(dc_);
        ds.set(MakeM(0.46f, 0.f, 0.f, 0.40f, 0.f, 0.06f));
        p.fill(beanie_.Get(), c);
        p.stroke(beanie_.Get(), edge, 1.f * px / 0.40f);
        p.rounded(-0.48f, -0.02f, 0.48f, 0.16f, 0.06f, Lighten(c, 0.35f));
        p.roundedStroke(-0.48f, -0.02f, 0.48f, 0.16f, 0.06f, edge, 1.f * px);
        p.circle(0.f, -0.38f, 0.11f, Lighten(c, 0.55f));
        p.ring(0.f, -0.38f, 0.11f, edge, 1.f * px);
      } else if (d.variant == 3) {
        p.rounded(-0.32f, -0.26f, 0.32f, 0.30f, 0.08f, MixC(Hex(0x8E9AAA), c, 0.35f));
        p.roundedStroke(-0.32f, -0.26f, 0.32f, 0.30f, 0.08f, Hex(0x3E4756, 0.9f), 1.1f * px);
        p.ellipse(0.f, -0.23f, 0.30f, 0.09f, Hex(0xDDE5EE));
        p.ringE(0.f, -0.23f, 0.30f, 0.09f, Hex(0x3E4756, 0.9f), 1.f * px);
        p.rounded(-0.31f, -0.02f, 0.31f, 0.10f, 0.05f, c);
      } else {
        p.ellipse(0.f, 0.30f, 0.54f, 0.10f, Hex(0x2F2F38));
        p.ringE(0.f, 0.30f, 0.54f, 0.10f, Hex(0x0E0E12, 0.8f), 1.f * px);
        p.rounded(-0.31f, -0.40f, 0.31f, 0.30f, 0.05f, Hex(0x25252D));
        p.roundedStroke(-0.31f, -0.40f, 0.31f, 0.30f, 0.05f, Hex(0x0E0E12, 0.8f), 1.f * px);
        p.rect(-0.31f, 0.08f, 0.31f, 0.20f, c);
        p.rect(-0.05f, 0.08f, 0.05f, 0.20f, Hex(0xE8B93E));
        p.ellipse(0.f, -0.40f, 0.31f, 0.055f, Hex(0x383842));
      }
      break;
    }
    case FreeKind::Scarf: {
      const D2D1_COLOR_F edge = Fade(Darken(c, 0.55f), 0.9f);
      p.rounded(-0.30f, -0.46f, 0.30f, 0.10f, 0.12f, c);
      p.roundedStroke(-0.30f, -0.46f, 0.30f, 0.10f, 0.12f, edge, 1.f * px);
      for (int i = -2; i <= 2; ++i) {
        const float x = 0.10f * static_cast<float>(i);
        p.line(x, -0.44f, x, 0.08f, Lighten(c, 0.45f), 0.045f);
      }
      for (int i = 0; i < 5; ++i) {
        const float x = -0.27f + 0.135f * static_cast<float>(i);
        p.line(x, 0.10f, x - 0.02f, 0.20f, Darken(c, 0.85f), 0.05f);
      }
      p.rounded(-0.30f, -0.46f, 0.30f, -0.36f, 0.05f, Lighten(c, 0.25f));
      p.roundedStroke(-0.30f, -0.46f, 0.30f, -0.36f, 0.05f, edge, 0.8f * px);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Colour picker popup for hats/scarves

void DecorRenderer::DrawSnowball(const DecorFrame& f, D2D1_POINT_2F c, float r, float spin) {
  if (!dc_ || r < 0.5f) return;
  ensureGeometry();
  const Painter p(dc_, brush_, roundCap_.Get());
  // Soft shadow on the snow it rolls over.
  p.ellipse(c.x, c.y + r * 0.55f, r * 0.9f, r * 0.26f, Hex(0x1A2233, 0.22f));
  shadedEllipse(GradKind::Shade, Hex(0xF4F7FB), c.x, c.y, r, r, -0.28f * r, -0.32f * r);
  p.ring(c.x, c.y, r, Hex(0xCBD8E8, 0.95f), std::max(1.f, r * 0.05f));
  // Crust clumps: fixed on the ball, so they turn with it while it rolls.
  for (int i = 0; i < 6; ++i) {
    const float a = spin + static_cast<float>(i) * 1.0471976f;
    const float rr = r * (0.42f + 0.12f * static_cast<float>(i % 2));
    p.circle(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr * 0.9f, std::max(0.6f, r * 0.055f), Hex(0xFFFFFF, 0.6f));
  }
  p.circle(c.x - r * 0.3f, c.y - r * 0.34f, r * 0.16f, Hex(0xFFFFFF, 0.85f));
}

void DecorRenderer::DrawColorPicker(const DecorFrame& f, const ColorPickerLayout& layout, int selected) {
  if (!dc_ || layout.bar.empty() || layout.swatches.empty()) return;
  const float dpi = f.dpiScale > 0.f ? f.dpiScale : 1.f;
  const Painter p(dc_, brush_, roundCap_.Get());
  const RectI& bar = layout.bar;
  const float bl = static_cast<float>(bar.left), bt = static_cast<float>(bar.top);
  const float br = static_cast<float>(bar.right), bb = static_cast<float>(bar.bottom);
  const float barR = std::min(12.f * dpi, 0.5f * static_cast<float>(bar.height()));
  p.rounded(bl, bt + 4.f * dpi, br, bb + 4.f * dpi, barR, Hex(0x000000, 0.25f));
  p.rounded(bl, bt, br, bb, barR, Hex(0x101626, 0.92f));
  p.roundedStroke(bl + 0.5f, bt + 0.5f, br - 0.5f, bb - 0.5f, barR, Hex(0xFFFFFF, 0.18f), 1.f * dpi);
  for (size_t i = 0; i < layout.swatches.size(); ++i) {
    const RectI& s = layout.swatches[i];
    const float cx = 0.5f * static_cast<float>(s.left + s.right);
    const float cy = 0.5f * static_cast<float>(s.top + s.bottom);
    const float r = 0.5f * static_cast<float>(s.right - s.left) - 2.f * dpi;
    const D2D1_COLOR_F c = Hex(DecorColorRgb(static_cast<int>(i)));
    p.circle(cx, cy, r, c);
    p.ring(cx, cy, r, Hex(0xFFFFFF, 0.35f), 1.f * dpi);
    if (static_cast<int>(i) == selected) {
      p.ring(cx, cy, r + 3.f * dpi, Hex(0x8CD9FF, 0.95f), 2.f * dpi);
      p.circle(cx, cy, 2.5f * dpi, Hex(0x0B1220, 0.85f));
    }
  }
}

}  // namespace snow::gfx
