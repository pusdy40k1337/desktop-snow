// Shared Direct2D helpers for the Windows renderer.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "../core/theme.h"

namespace snow::gfx {

using Microsoft::WRL::ComPtr;

inline D2D1_COLOR_F ToD2D(const Color& c, float alphaMul = 1.f) {
  return D2D1::ColorF(c.r, c.g, c.b, c.a * alphaMul);
}
inline D2D1_COLOR_F Rgba(float r, float g, float b, float a = 1.f) { return D2D1::ColorF(r, g, b, a); }

// One reusable solid brush: every call re-colours it. Each D2D draw call uses
// the brush immediately, so sharing is safe and avoids per-frame allocations.
class SolidBrush {
 public:
  void reset(ID2D1DeviceContext* dc) {
    brush_.Reset();
    if (dc) dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
  }
  ID2D1SolidColorBrush* operator()(const D2D1_COLOR_F& c) {
    if (brush_) brush_->SetColor(c);
    return brush_.Get();
  }
  ID2D1SolidColorBrush* operator()(const Color& c, float alphaMul = 1.f) { return (*this)(ToD2D(c, alphaMul)); }

 private:
  ComPtr<ID2D1SolidColorBrush> brush_;
};

// Saves the context transform and restores it on scope exit.
class TransformScope {
 public:
  explicit TransformScope(ID2D1DeviceContext* dc) : dc_(dc) { dc_->GetTransform(&saved_); }
  ~TransformScope() { dc_->SetTransform(saved_); }
  const D2D1_MATRIX_3X2_F& saved() const { return saved_; }
  // Applies `local` in front of the saved transform.
  void set(const D2D1_MATRIX_3X2_F& local) {
    const D2D1_MATRIX_3X2_F& s = saved_;
    D2D1_MATRIX_3X2_F m{};
    m._11 = local._11 * s._11 + local._12 * s._21;
    m._12 = local._11 * s._12 + local._12 * s._22;
    m._21 = local._21 * s._11 + local._22 * s._21;
    m._22 = local._21 * s._12 + local._22 * s._22;
    m._31 = local._31 * s._11 + local._32 * s._21 + s._31;
    m._32 = local._31 * s._12 + local._32 * s._22 + s._32;
    dc_->SetTransform(m);
  }

 private:
  ID2D1DeviceContext* dc_;
  D2D1_MATRIX_3X2_F saved_{};
};

}  // namespace snow::gfx
