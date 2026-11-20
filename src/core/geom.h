// Portable geometry helpers. All screen coordinates in this project are
// physical pixels in the virtual-screen space (the process is PerMonitorV2).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace snow {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
  float x = 0, y = 0;
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  float length() const { return std::sqrt(x * x + y * y); }
};

struct Vec2d {
  double x = 0, y = 0;
  Vec2d() = default;
  Vec2d(double x_, double y_) : x(x_), y(y_) {}
  Vec2d operator+(Vec2d o) const { return {x + o.x, y + o.y}; }
  Vec2d operator-(Vec2d o) const { return {x - o.x, y - o.y}; }
  Vec2d operator*(double s) const { return {x * s, y * s}; }
  double length() const { return std::sqrt(x * x + y * y); }
};

struct RectI {
  int left = 0, top = 0, right = 0, bottom = 0;
  RectI() = default;
  RectI(int l, int t, int r, int b) : left(l), top(t), right(r), bottom(b) {}
  int width() const { return right - left; }
  int height() const { return bottom - top; }
  bool empty() const { return right <= left || bottom <= top; }
  bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
  bool containsF(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
  bool intersects(const RectI& o) const {
    return left < o.right && o.left < right && top < o.bottom && o.top < bottom;
  }
  RectI intersect(const RectI& o) const {
    RectI r(std::max(left, o.left), std::max(top, o.top), std::min(right, o.right), std::min(bottom, o.bottom));
    if (r.empty()) return RectI();
    return r;
  }
  RectI offset(int dx, int dy) const { return {left + dx, top + dy, right + dx, bottom + dy}; }
  bool operator==(const RectI& o) const {
    return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
  }
  bool operator!=(const RectI& o) const { return !(*this == o); }
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
inline float easeOutCubic(float t) { t = clampf(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }
inline float easeInOut(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

// Vertical drop of a rounded window corner below the bounding-rect top edge,
// at distance `dx` (>=0) from the nearest side. Zero for dx >= radius.
inline float cornerDrop(float dx, float radius) {
  if (radius <= 0.f || dx >= radius) return 0.f;
  if (dx < 0.f) dx = 0.f;
  float a = radius - dx;
  return radius - std::sqrt(std::max(0.f, radius * radius - a * a));
}

}  // namespace snow
