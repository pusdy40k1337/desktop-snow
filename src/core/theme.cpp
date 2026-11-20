#include "theme.h"

#include <cmath>

#include "geom.h"

namespace snow {

Color Mix(Color a, Color b, float t) {
  return Color{lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t), lerpf(a.a, b.a, t)};
}

Color WithAlpha(Color c, float a) {
  c.a = a;
  return c;
}

bool ParseHexColor(const std::string& hex, Color& out) {
  std::string s = hex;
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
  if (s.size() != 6) return false;
  int v = 0;
  for (char c : s) {
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return false;
  }
  out = Rgb(v);
  return true;
}

static std::vector<Theme> MakeThemes() {
  std::vector<Theme> t;
  Theme classic;
  classic.id = "classic";
  classic.snow = Rgb(0xFFFFFF);
  classic.pileTop = Rgb(0xFFFFFF);
  classic.pileBottom = Rgb(0xDCE8F5);
  classic.pileEdge = Rgb(0xFFFFFF, 0.6f);
  classic.sparkle = Rgb(0xFFFFFF);
  classic.star = Rgb(0xFFD54A);
  classic.ornaments = {Rgb(0xD62828), Rgb(0xF4C430), Rgb(0x2A9D8F), Rgb(0x3A86FF), Rgb(0xC0C0C8), Rgb(0x9B5DE5)};
  classic.lights = {Rgb(0xFF4D4D), Rgb(0xFFD84D), Rgb(0x4DFF88), Rgb(0x4DB8FF), Rgb(0xFF8AF0)};
  classic.scarves = {Rgb(0xD62828), Rgb(0x2A9D8F), Rgb(0x3A86FF), Rgb(0xF4A261)};
  classic.nightTint = Rgb(0xC9D8FF);
  t.push_back(classic);

  Theme frost = classic;
  frost.id = "frost";
  frost.snow = Rgb(0xEEF6FF);
  frost.pileTop = Rgb(0xF4FAFF);
  frost.pileBottom = Rgb(0xBFD7F2);
  frost.star = Rgb(0xBFE6FF);
  frost.ornaments = {Rgb(0x8FD3FF), Rgb(0xE0F2FF), Rgb(0x5B8DEF), Rgb(0xB8C6DB), Rgb(0x7FDBDA), Rgb(0xFFFFFF)};
  frost.lights = {Rgb(0xBFE6FF), Rgb(0xFFFFFF), Rgb(0x8FD3FF), Rgb(0xD6E4FF), Rgb(0xA0F0FF)};
  frost.scarves = {Rgb(0x5B8DEF), Rgb(0x7FDBDA), Rgb(0xB8C6DB), Rgb(0x2B4C7E)};
  t.push_back(frost);

  Theme candy = classic;
  candy.id = "candy";
  candy.pileBottom = Rgb(0xF6DDE8);
  candy.star = Rgb(0xFF7AB6);
  candy.ornaments = {Rgb(0xFF4F79), Rgb(0xFFFFFF), Rgb(0xFF9EC4), Rgb(0xE63946), Rgb(0xFFC8DD), Rgb(0xBDE0FE)};
  candy.lights = {Rgb(0xFF4F79), Rgb(0xFFFFFF), Rgb(0xFF9EC4), Rgb(0xFFC8DD), Rgb(0xBDE0FE)};
  candy.scarves = {Rgb(0xFF4F79), Rgb(0xFF9EC4), Rgb(0xE63946), Rgb(0xBDE0FE)};
  t.push_back(candy);

  Theme golden = classic;
  golden.id = "golden";
  golden.snow = Rgb(0xFFF8EC);
  golden.pileBottom = Rgb(0xEADFCB);
  golden.star = Rgb(0xFFC94D);
  golden.ornaments = {Rgb(0xD4AF37), Rgb(0x8B1E3F), Rgb(0xF5E6C8), Rgb(0xB8860B), Rgb(0x2F4F4F), Rgb(0xC9B037)};
  golden.lights = {Rgb(0xFFD27F), Rgb(0xFFE7B0), Rgb(0xFFC04D), Rgb(0xFFF1D0), Rgb(0xFFB347)};
  golden.scarves = {Rgb(0x8B1E3F), Rgb(0xD4AF37), Rgb(0x2F4F4F), Rgb(0xB8860B)};
  golden.nightTint = Rgb(0xFFE9C9);
  t.push_back(golden);
  return t;
}

static const std::vector<Theme>& Themes() {
  static const std::vector<Theme> themes = MakeThemes();
  return themes;
}

const std::vector<std::string>& ThemeIds() {
  static const std::vector<std::string> ids = [] {
    std::vector<std::string> v;
    for (const auto& t : Themes()) v.push_back(t.id);
    return v;
  }();
  return ids;
}

const Theme& GetTheme(const std::string& id) {
  for (const auto& t : Themes())
    if (t.id == id) return t;
  return Themes().front();
}

float NightFactor(int hour, int minute) {
  float m = static_cast<float>(((hour % 24) + 24) % 24 * 60 + minute);
  if (m >= 360.f && m < 480.f) return 1.f - smoothstep(360.f, 480.f, m);  // dawn 06:00-08:00
  if (m >= 480.f && m < 990.f) return 0.f;                                // day
  if (m >= 990.f && m < 1110.f) return smoothstep(990.f, 1110.f, m);      // dusk 16:30-18:30
  return 1.f;
}

float Twinkle(double t, float phase, float rateHz, bool reducedMotion, float minLevel) {
  if (reducedMotion) return 1.f;
  float rate = clampf(rateHz, 0.f, kMaxTwinkleHz);
  double s = 0.5 + 0.5 * std::sin(2.0 * 3.14159265358979 * (rate * t + phase));
  // Ease the curve so lights linger near full brightness (gentle, not strobing).
  float e = static_cast<float>(s * s * (3.0 - 2.0 * s));
  return minLevel + (1.f - minLevel) * e;
}

int CountFlashes(const std::vector<float>& samples, float threshold) {
  // A flash = a rise of >= threshold followed by a fall of >= threshold.
  if (samples.empty()) return 0;
  int flashes = 0;
  float lo = samples[0], hi = samples[0];
  bool rising = false;  // saw a qualifying rise, waiting for the fall
  for (float v : samples) {
    if (!rising) {
      lo = std::min(lo, v);
      if (v - lo >= threshold) {
        rising = true;
        hi = v;
      }
    } else {
      hi = std::max(hi, v);
      if (hi - v >= threshold) {
        ++flashes;
        rising = false;
        lo = v;
      }
    }
  }
  return flashes;
}

}  // namespace snow
