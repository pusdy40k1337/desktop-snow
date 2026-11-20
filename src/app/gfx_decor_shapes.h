// Small Direct2D helpers for the procedural decor artwork: colour math,
// matrices, a path builder, a thin fill/stroke painter and a cache of radial
// gradient brushes keyed by colour.
#pragma once
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
#include "gfx_common.h"
#include "../core/geom.h"

namespace snow::gfx {

inline D2D1_POINT_2F Pt(float x, float y) { return D2D1::Point2F(x, y); }

inline D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.f) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 255u) / 255.f, static_cast<float>((rgb >> 8) & 255u) / 255.f,
                      static_cast<float>(rgb & 255u) / 255.f, a);
}
inline D2D1_COLOR_F MixC(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
inline D2D1_COLOR_F Darken(const D2D1_COLOR_F& c, float k) { return D2D1::ColorF(c.r * k, c.g * k, c.b * k, c.a); }
inline D2D1_COLOR_F Lighten(const D2D1_COLOR_F& c, float t) {
  return D2D1::ColorF(c.r + (1.f - c.r) * t, c.g + (1.f - c.g) * t, c.b + (1.f - c.b) * t, c.a);
}
inline D2D1_COLOR_F Fade(const D2D1_COLOR_F& c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// Matrices use D2D's row-vector convention: a point goes through scale, then
// rotation (radians, clockwise on screen), then translation.
inline D2D1_MATRIX_3X2_F MakeM(float m11, float m12, float m21, float m22, float dx, float dy) {
  D2D1_MATRIX_3X2_F m{};
  m._11 = m11;
  m._12 = m12;
  m._21 = m21;
  m._22 = m22;
  m._31 = dx;
  m._32 = dy;
  return m;
}
inline D2D1_MATRIX_3X2_F Xf(float sx, float sy, float rot, float tx, float ty) {
  const float c = std::cos(rot), s = std::sin(rot);
  return MakeM(sx * c, sx * s, -sy * s, sy * c, tx, ty);
}
// Rotation by `rot` around the pivot (px, py) of the current space.
inline D2D1_MATRIX_3X2_F RotAbout(float rot, float px, float py) {
  const float c = std::cos(rot), s = std::sin(rot);
  return MakeM(c, s, -s, c, px - (px * c - py * s), py - (px * s + py * c));
}
// Uniform scale around the pivot (px, py).
inline D2D1_MATRIX_3X2_F ScaleAbout(float k, float px, float py) {
  return MakeM(k, 0, 0, k, px - px * k, py - py * k);
}

// Builds one path geometry; coordinates are whatever space the caller picks.
class PathBuilder {
 public:
  explicit PathBuilder(ID2D1Factory1* factory) {
    if (!factory) return;
    if (FAILED(factory->CreatePathGeometry(&geo_))) {
      geo_.Reset();
      return;
    }
    if (FAILED(geo_->Open(&sink_))) {
      geo_.Reset();
      sink_.Reset();
      return;
    }
    sink_->SetFillMode(D2D1_FILL_MODE_WINDING);
  }
  void begin(float x, float y, bool filled = true) {
    if (!sink_) return;
    if (open_) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
    sink_->BeginFigure(Pt(x, y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    open_ = true;
  }
  void line(float x, float y) {
    if (sink_ && open_) sink_->AddLine(Pt(x, y));
  }
  void quad(float cx, float cy, float x, float y) {
    if (!sink_ || !open_) return;
    D2D1_QUADRATIC_BEZIER_SEGMENT seg{};
    seg.point1 = Pt(cx, cy);
    seg.point2 = Pt(x, y);
    sink_->AddQuadraticBezier(&seg);
  }
  void cubic(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!sink_ || !open_) return;
    D2D1_BEZIER_SEGMENT seg{};
    seg.point1 = Pt(c1x, c1y);
    seg.point2 = Pt(c2x, c2y);
    seg.point3 = Pt(x, y);
    sink_->AddBezier(&seg);
  }
  void end(bool closed = true) {
    if (!sink_ || !open_) return;
    sink_->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    open_ = false;
  }
  ComPtr<ID2D1PathGeometry> finish() {
    if (!sink_) return nullptr;
    if (open_) end();
    const HRESULT hr = sink_->Close();
    sink_.Reset();
    if (FAILED(hr)) geo_.Reset();
    return std::move(geo_);
  }

 private:
  ComPtr<ID2D1PathGeometry> geo_;
  ComPtr<ID2D1GeometrySink> sink_;
  bool open_ = false;
};

// Solid fills and strokes through the shared brush. Strokes use a round
// cap/join style when one is supplied.
class Painter {
 public:
  Painter(ID2D1DeviceContext* dc, SolidBrush& brush, ID2D1StrokeStyle* round) : dc_(dc), b_(brush), round_(round) {}

  void circle(float x, float y, float r, const D2D1_COLOR_F& c) const {
    if (auto* br = b_(c)) dc_->FillEllipse(D2D1::Ellipse(Pt(x, y), r, r), br);
  }
  void ellipse(float x, float y, float rx, float ry, const D2D1_COLOR_F& c) const {
    if (auto* br = b_(c)) dc_->FillEllipse(D2D1::Ellipse(Pt(x, y), rx, ry), br);
  }
  void ring(float x, float y, float r, const D2D1_COLOR_F& c, float w) const {
    if (auto* br = b_(c)) dc_->DrawEllipse(D2D1::Ellipse(Pt(x, y), r, r), br, w, round_);
  }
  void ringE(float x, float y, float rx, float ry, const D2D1_COLOR_F& c, float w) const {
    if (auto* br = b_(c)) dc_->DrawEllipse(D2D1::Ellipse(Pt(x, y), rx, ry), br, w, round_);
  }
  void rect(float l, float t, float r, float bt, const D2D1_COLOR_F& c) const {
    if (auto* br = b_(c)) dc_->FillRectangle(D2D1::RectF(l, t, r, bt), br);
  }
  void rounded(float l, float t, float r, float bt, float rad, const D2D1_COLOR_F& c) const {
    if (auto* br = b_(c)) dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, bt), rad, rad), br);
  }
  void roundedStroke(float l, float t, float r, float bt, float rad, const D2D1_COLOR_F& c, float w) const {
    if (auto* br = b_(c))
      dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, bt), rad, rad), br, w, round_);
  }
  void line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& c, float w) const {
    if (auto* br = b_(c)) dc_->DrawLine(Pt(x0, y0), Pt(x1, y1), br, w, round_);
  }
  void fill(ID2D1Geometry* g, const D2D1_COLOR_F& c) const {
    if (!g) return;
    if (auto* br = b_(c)) dc_->FillGeometry(g, br);
  }
  void stroke(ID2D1Geometry* g, const D2D1_COLOR_F& c, float w) const {
    if (!g) return;
    if (auto* br = b_(c)) dc_->DrawGeometry(g, br, w, round_);
  }
  // Limb: dark outline line with a lighter fill line on top.
  void limb(float x0, float y0, float x1, float y1, float w, float outline, const D2D1_COLOR_F& fillC,
            const D2D1_COLOR_F& lineC) const {
    line(x0, y0, x1, y1, lineC, w + 2.f * outline);
    line(x0, y0, x1, y1, fillC, w);
  }

 private:
  ID2D1DeviceContext* dc_;
  SolidBrush& b_;
  ID2D1StrokeStyle* round_;
};

enum class GradKind : uint32_t {
  Glow = 0,   // colour fading out to transparent (halos, soft shadows, puffs)
  Shade = 1,  // light centre, base colour, darker rim (baubles, foliage, skin)
  Snow = 2,   // white with blue-grey shading (snowballs, fur, beard)
  Soft = 3    // gentle highlight and rim (skin)
};

inline void GradStops(GradKind k, const D2D1_COLOR_F& c, D2D1_GRADIENT_STOP out[3]) {
  switch (k) {
    case GradKind::Glow:
      out[0] = {0.f, Fade(c, 1.f)};
      out[1] = {0.35f, Fade(c, 0.42f)};
      out[2] = {1.f, Fade(c, 0.f)};
      break;
    case GradKind::Shade:
      out[0] = {0.f, Lighten(c, 0.55f)};
      out[1] = {0.5f, c};
      out[2] = {1.f, Darken(c, 0.6f)};
      break;
    case GradKind::Soft:
      out[0] = {0.f, Lighten(c, 0.3f)};
      out[1] = {0.55f, c};
      out[2] = {1.f, Darken(c, 0.86f)};
      break;
    case GradKind::Snow:
    default:
      out[0] = {0.f, c};
      out[1] = {0.58f, MixC(c, Hex(0xBCCBE4, c.a), 0.5f)};
      out[2] = {1.f, MixC(c, Hex(0x8CA2CF, c.a), 0.85f)};
      break;
  }
}

// Radial gradient brushes created lazily per (kind, colour); the positions and
// radii are set at every use.
class RadialCache {
 public:
  void reset() { entries_.clear(); }
  ID2D1RadialGradientBrush* get(ID2D1RenderTarget* rt, GradKind k, const D2D1_COLOR_F& c) {
    const uint64_t key = (static_cast<uint64_t>(k) << 32) | pack(c);
    for (const Entry& e : entries_)
      if (e.key == key) return e.brush.Get();
    if (!rt) return nullptr;
    D2D1_GRADIENT_STOP stops[3];
    GradStops(k, c, stops);
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(rt->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll))) return nullptr;
    ComPtr<ID2D1RadialGradientBrush> br;
    const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props = D2D1::RadialGradientBrushProperties(Pt(0, 0), Pt(0, 0), 1.f, 1.f);
    if (FAILED(rt->CreateRadialGradientBrush(props, coll.Get(), &br))) return nullptr;
    if (entries_.size() >= 160) entries_.clear();
    entries_.push_back(Entry{key, br});
    return br.Get();
  }

 private:
  struct Entry {
    uint64_t key;
    ComPtr<ID2D1RadialGradientBrush> brush;
  };
  static uint32_t q(float v) {
    const float x = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    return static_cast<uint32_t>(x * 255.f + 0.5f);
  }
  static uint32_t pack(const D2D1_COLOR_F& c) { return (q(c.r) << 24) | (q(c.g) << 16) | (q(c.b) << 8) | q(c.a); }
  std::vector<Entry> entries_;
};

}  // namespace snow::gfx
