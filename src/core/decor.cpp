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

PaletteLayout LayoutPalette(const RectI& work, float dpiScale) {
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
  int left = work.left + static_cast<int>((work.width() - total) / 2);
  int top = work.top + static_cast<int>(16.f * dpiScale);
  l.bar = RectI(left, top, left + static_cast<int>(total), top + static_cast<int>(btnH + 2 * pad));
  for (int i = 0; i < count; ++i) {
    int x = left + static_cast<int>(pad + i * (btnW + gap));
    int y = top + static_cast<int>(pad);
    l.buttons.push_back({static_cast<Tool>(i), RectI(x, y, x + static_cast<int>(btnW), y + static_cast<int>(btnH))});
  }
  return l;
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

}  // namespace snow
