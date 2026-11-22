#include "decor.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace snow {

float TreeWidth(float height) { return 0.72f * height; }

TreeBox TreeBounds(Vec2 base, float height) {
  float w = TreeWidth(height);
  return {base.x - w / 2, base.y - height, base.x + w / 2, base.y};
}

bool TreeFoliageContains(float u, float v) {
  if (v < 0.10f || v > 0.86f) return false;
  return std::fabs(u - 0.5f) <= 0.49f * (v - 0.08f) / 0.78f;
}

Vec2 TreeLocalToScreen(Vec2 base, float height, float u, float v) {
  TreeBox b = TreeBounds(base, height);
  return {b.left + u * (b.right - b.left), b.top + v * height};
}

bool TreeScreenToLocal(Vec2 base, float height, Vec2 p, float& u, float& v) {
  TreeBox b = TreeBounds(base, height);
  if (height <= 0.f) return false;
  u = (p.x - b.left) / (b.right - b.left);
  v = (p.y - b.top) / height;
  return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
}

SnowmanBox SnowmanBounds(Vec2 base, float height) {
  return {base.x - 0.30f * height, base.y - height, base.x + 0.30f * height, base.y};
}

float SnowmanHeightPx(float scale, float dpiScale) { return 92.f * scale * dpiScale; }
float TreeHeightPx(float scale, float dpiScale) { return 230.f * scale * dpiScale; }

std::string NormalizeTitle(const std::string& title) {
  std::string s;
  s.reserve(title.size());
  for (unsigned char c : title) s += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  // Leading unsaved-changes markers ("*file.txt", "● file.txt").
  while (!s.empty() && (s[0] == '*' || s[0] == ' ')) s.erase(0, 1);
  if (s.rfind("\xE2\x97\x8F ", 0) == 0) s.erase(0, 4);
  while (!s.empty() && (s.back() == ' ' || s.back() == '*')) s.pop_back();
  return s;
}

int AnchorMatchScore(const WindowAnchor& a, const WindowIdentity& w) {
  if (a.taskbar || w.taskbar) return (a.taskbar && w.taskbar) ? 100 : 0;
  if (a.process.empty() && a.className.empty()) return 0;
  if (!a.process.empty() && a.process != w.process) return 0;
  int score = 30;
  if (!a.className.empty()) score += (a.className == w.className) ? 15 : -10;
  if (a.hwnd != 0 && a.hwnd == w.hwnd) score += 40;
  std::string at = NormalizeTitle(a.title), wt = NormalizeTitle(w.title);
  if (!at.empty() && at == wt) {
    score += 30;
  } else if (!at.empty() && !wt.empty()) {
    size_t n = std::min(at.size(), wt.size());
    size_t pre = 0, suf = 0;
    while (pre < n && at[pre] == wt[pre]) ++pre;
    while (suf < n - pre && at[at.size() - 1 - suf] == wt[wt.size() - 1 - suf]) ++suf;
    float sim = static_cast<float>(pre + suf) / static_cast<float>(std::max(at.size(), wt.size()));
    score += static_cast<int>(20.f * std::min(1.f, sim));
  }
  return std::max(0, score);
}

PaletteLayout LayoutPalette(const RectI& work, float dpiScale, int left, int top) {
  PaletteLayout l;
  const int count = static_cast<int>(Tool::Count);
  float btnW = 66.f * dpiScale, btnH = 60.f * dpiScale, gap = 6.f * dpiScale, pad = 10.f * dpiScale;
  float total = count * btnW + (count - 1) * gap + 2 * pad;
  float avail = static_cast<float>(work.width()) - 20.f * dpiScale;
  if (total > avail && avail > 0) {
    float k = avail / total;
    btnW *= k;
    btnH *= k;
    gap *= k;
    pad *= k;
    total = avail;
  }
  int barLeft = work.left + static_cast<int>((work.width() - total) / 2);
  int barTop = work.top + static_cast<int>(16.f * dpiScale);
  if (!(left == -1 && top == -1)) {
    barLeft = left;
    barTop = top;
  }
  RectI raw(barLeft, barTop, barLeft + static_cast<int>(total), barTop + static_cast<int>(btnH + 2 * pad));
  l.bar = ClampPaletteRect(raw, work);
  barLeft = l.bar.left;
  barTop = l.bar.top;
  for (int i = 0; i < count; ++i) {
    int x = barLeft + static_cast<int>(pad + i * (btnW + gap));
    int y = barTop + static_cast<int>(pad);
    l.buttons.push_back({static_cast<Tool>(i), RectI(x, y, x + static_cast<int>(btnW), y + static_cast<int>(btnH))});
  }
  return l;
}

RectI ClampPaletteRect(RectI bar, const RectI& work) {
  const int w = bar.width(), h = bar.height();
  const int maxLeft = std::max(work.left, work.right - w);
  const int maxTop = std::max(work.top, work.bottom - h);
  const int left = std::min(std::max(bar.left, work.left), maxLeft);
  const int top = std::min(std::max(bar.top, work.top), maxTop);
  return RectI(left, top, left + w, top + h);
}

bool PaletteHit(const PaletteLayout& layout, int x, int y, Tool& out) {
  for (const auto& b : layout.buttons) {
    if (b.rect.contains(x, y)) {
      out = b.tool;
      return true;
    }
  }
  return false;
}

bool PaletteUiHit(const PaletteLayout& layout, int x, int y, int margin) {
  return x >= layout.bar.left - margin && x < layout.bar.right + margin && y >= layout.bar.top - margin &&
         y < layout.bar.bottom + margin;
}

RectI HintRect(const RectI& work, const RectI& bar, float dpiScale) {
  const float w = std::min(720.f * dpiScale, static_cast<float>(work.width()) - 40.f * dpiScale);
  const float h = 46.f * dpiScale;
  const float x = static_cast<float>(work.left) + (static_cast<float>(work.width()) - w) / 2.f;
  const bool topHalf = bar.top < work.top + work.height() / 2;
  const float y = topHalf ? static_cast<float>(bar.bottom) + 8.f * dpiScale : static_cast<float>(bar.top) - h - 8.f * dpiScale;
  RectI r(static_cast<int>(x), static_cast<int>(y), static_cast<int>(x + w), static_cast<int>(y + h));
  // Never leave the work area (the bar can be dragged to the very bottom).
  const int top = std::min(std::max(r.top, work.top), std::max(work.top, work.bottom - r.height()));
  const int left = std::min(std::max(r.left, work.left), std::max(work.left, work.right - r.width()));
  return RectI(left, top, left + r.width(), top + r.height());
}

uint32_t DecorColorRgb(int index) {
  static const uint32_t kPalette[kDecorColorCount] = {
      0xE5484D,  // red
      0xF2A93B,  // orange
      0xF7D046,  // yellow
      0x46B17B,  // green
      0x2FB7C9,  // teal
      0x3B82F6,  // blue
      0x8B5CF6,  // violet
      0xE85C9E,  // pink
      0xF4F7FB,  // white
      0x2A2D36,  // charcoal
  };
  int i = index % kDecorColorCount;
  if (i < 0) i += kDecorColorCount;
  return kPalette[i];
}

float FreeDecorBaseSize(FreeKind kind) {
  switch (kind) {
    case FreeKind::Bauble: return 30.f;
    case FreeKind::CandyCane: return 46.f;
    case FreeKind::Star: return 38.f;
    case FreeKind::Hat: return 42.f;
    case FreeKind::Scarf: return 48.f;
  }
  return 34.f;
}

float FreeDecorAspect(FreeKind kind) {
  switch (kind) {
    case FreeKind::Bauble: return 1.f;
    case FreeKind::CandyCane: return 0.62f;
    case FreeKind::Star: return 1.f;
    case FreeKind::Hat: return 1.15f;
    case FreeKind::Scarf: return 0.72f;
  }
  return 1.f;
}

ColorPickerLayout LayoutColorPicker(int anchorX, int anchorY, const RectI& work, float dpiScale) {
  ColorPickerLayout l;
  const float sw = 26.f * dpiScale, gap = 6.f * dpiScale, pad = 8.f * dpiScale;
  const float total = kDecorColorCount * sw + (kDecorColorCount - 1) * gap + 2 * pad;
  const float h = sw + 2 * pad;
  float left = static_cast<float>(anchorX) - total * 0.5f;
  float top = static_cast<float>(anchorY) + 14.f * dpiScale;
  left = clampf(left, static_cast<float>(work.left) + 4.f * dpiScale,
                std::max(static_cast<float>(work.left) + 4.f * dpiScale, static_cast<float>(work.right) - total - 4.f * dpiScale));
  if (top + h > static_cast<float>(work.bottom) - 4.f * dpiScale)
    top = static_cast<float>(anchorY) - h - 14.f * dpiScale;
  top = clampf(top, static_cast<float>(work.top) + 4.f * dpiScale, std::max(static_cast<float>(work.top) + 4.f * dpiScale, static_cast<float>(work.bottom) - h - 4.f * dpiScale));
  l.bar = RectI(static_cast<int>(left), static_cast<int>(top), static_cast<int>(left + total), static_cast<int>(top + h));
  for (int i = 0; i < kDecorColorCount; ++i) {
    const float x = left + pad + static_cast<float>(i) * (sw + gap);
    const float y = top + pad;
    l.swatches.push_back(RectI(static_cast<int>(x), static_cast<int>(y), static_cast<int>(x + sw), static_cast<int>(y + sw)));
  }
  return l;
}

int ColorPickerHit(const ColorPickerLayout& layout, int x, int y, int margin) {
  for (size_t i = 0; i < layout.swatches.size(); ++i) {
    const RectI& r = layout.swatches[i];
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int rad = (r.right - r.left) / 2 + 2 + margin;
    const int dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy <= rad * rad) return static_cast<int>(i);
  }
  return -1;
}

bool ColorPickerUiHit(const ColorPickerLayout& layout, int x, int y, int margin) {
  return x >= layout.bar.left - margin && x < layout.bar.right + margin && y >= layout.bar.top - margin &&
         y < layout.bar.bottom + margin;
}

}  // namespace snow
