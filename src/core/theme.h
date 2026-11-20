// Colour themes, day/night tint and photosensitivity-safe twinkling.
#pragma once
#include <array>
#include <string>
#include <vector>

namespace snow {

struct Color {
  float r = 1, g = 1, b = 1, a = 1;
};
inline Color Rgb(int hex, float a = 1.f) {
  return Color{((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a};
}
Color Mix(Color a, Color b, float t);
Color WithAlpha(Color c, float a);
bool ParseHexColor(const std::string& hex, Color& out);

struct Theme {
  std::string id;
  Color snow;          // falling flakes
  Color pileTop;       // pile highlight
  Color pileBottom;    // pile shade near the base
  Color pileEdge;      // soft rim
  Color sparkle;
  Color star;
  std::array<Color, 6> ornaments;
  std::array<Color, 5> lights;
  std::array<Color, 4> scarves;
  Color nightTint;     // multiplied into snow at night
};

const std::vector<std::string>& ThemeIds();
const Theme& GetTheme(const std::string& id);  // unknown id -> classic

// 0 at day, 1 at night, smooth over dusk (16:30-18:30) and dawn (06:00-08:00).
float NightFactor(int hour, int minute);

// Smooth brightness in [minLevel, 1] for a twinkling light. The frequency is
// clamped to kMaxTwinkleHz so that no light (and no synchronized group of
// lights) ever exceeds 3 flashes per second (WCAG 2.3.1). Reduced motion -> 1.
constexpr float kMaxTwinkleHz = 0.8f;
float Twinkle(double t, float phase, float rateHz, bool reducedMotion, float minLevel = 0.45f);
// Counts "flashes" (a rise and fall of >= `threshold` of full brightness) of a
// brightness signal sampled at `hz` - used by tests to prove the bound.
int CountFlashes(const std::vector<float>& samples, float threshold);

}  // namespace snow
