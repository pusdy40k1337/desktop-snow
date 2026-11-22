// Procedural Direct2D artwork for decorations and visitors.
// All positions are in the context's current coordinate space (the renderer
// sets a transform mapping absolute virtual-screen px to the overlay target);
// every function composes its own local transform with the current one and
// restores it before returning.
#pragma once
#include "gfx_common.h"
#include "gfx_decor_shapes.h"
#include "../core/actors.h"
#include "../core/decor.h"
#include "../core/theme.h"

namespace snow::gfx {

struct DecorFrame {
  double time = 0;            // seconds, monotonic (animation clock)
  bool reducedMotion = false; // no twinkle / wobble / waving
  const Theme* theme = nullptr;
  float night = 0;            // 0 day .. 1 night (lights glow stronger)
  float dpiScale = 1;         // monitor DPI / 96
  bool treeBodyFromSprite = false;  // a PNG tree was drawn: only ornaments, lights, star
};

class DecorRenderer {
 public:
  // Must be called whenever the device context is (re)created; drops caches.
  void Reset(ID2D1DeviceContext* dc, ID2D1Factory1* factory);

  // base = bottom centre of the trunk, height = full tree height in px
  // (star included). Tree width = TreeWidth(height) (core/decor.h);
  // ornaments use TreeLocalToScreen(u, v) for placement.
  void DrawTree(const DecorFrame& f, const TreeDecor& tree, D2D1_POINT_2F base, float height, bool highlight);

  // base = point between the snowman's "feet" (on the window's top edge).
  // build: 0..1 build-up animation (bottom ball, middle, head, then accessories).
  // wobble: lean angle in radians (window jolts, yeti scares).
  void DrawSnowman(const DecorFrame& f, const SnowmanDecor& sm, D2D1_POINT_2F base, float height,
                   float build, float wobble, bool highlight);

  void DrawSanta(const DecorFrame& f, const SantaState& s);
  void DrawTrain(const DecorFrame& f, const TrainState& t);
  void DrawYeti(const DecorFrame& f, const YetiState& y);

  // Free-standing decoration placed anywhere on the desktop.
  void DrawFreeDecor(const DecorFrame& f, const FreeDecor& d, D2D1_POINT_2F centre, float size, bool highlight);
  // The snowball being rolled: a shaded ball with snow crust details spinning
  // with the ball (radius in px, spin in radians).
  void DrawSnowball(const DecorFrame& f, D2D1_POINT_2F centre, float radius, float spin);

  // Colour picker popup for hats/scarves.
  void DrawColorPicker(const DecorFrame& f, const ColorPickerLayout& layout, int selected);

  // Decorate-mode toolbar. labels: Tool::Count UTF-16 strings.
  void DrawPalette(const DecorFrame& f, const PaletteLayout& layout, Tool active,
                   const wchar_t* const* labels, IDWriteTextFormat* labelFormat);

 private:
  void ensureGeometry();
  // Radial-gradient fills (brushes cached per kind + colour).
  void shadedEllipse(GradKind k, const D2D1_COLOR_F& c, float cx, float cy, float rx, float ry, float ox, float oy);
  void shadedGeometry(ID2D1Geometry* g, GradKind k, const D2D1_COLOR_F& c, float cx, float cy, float rx, float ry,
                      float ox, float oy);
  void glow(const D2D1_COLOR_F& c, float cx, float cy, float rx, float ry, float opacity);
  void drawIcon(Tool tool, float cx, float cy, float size, const DecorFrame& f);

  ID2D1DeviceContext* dc_ = nullptr;
  ID2D1Factory1* factory_ = nullptr;
  SolidBrush brush_;
  RadialCache radial_;
  bool built_ = false;
  ComPtr<ID2D1StrokeStyle> roundCap_;   // round caps + joins for outlines and limbs
  ComPtr<ID2D1StrokeStyle> candyDash_;  // flat dashes for candy-cane stripes
  // Unit-space geometries (tree: 1 unit = tree height, x centred, y up = negative).
  ComPtr<ID2D1PathGeometry> treeTier_[3];
  ComPtr<ID2D1PathGeometry> treeFrost_[3];
  ComPtr<ID2D1PathGeometry> garland_[3];
  ComPtr<ID2D1PathGeometry> star_;
  ComPtr<ID2D1PathGeometry> candy_;
  ComPtr<ID2D1PathGeometry> carrot_;
  ComPtr<ID2D1PathGeometry> bucket_;
  ComPtr<ID2D1PathGeometry> beanie_;
  ComPtr<ID2D1PathGeometry> beard_;
  ComPtr<ID2D1PathGeometry> santaHat_;
  ComPtr<ID2D1PathGeometry> elfEar_;
  ComPtr<ID2D1PathGeometry> elfHat_;
  ComPtr<ID2D1PathGeometry> yetiBody_;
  ComPtr<ID2D1PathGeometry> yetiHead_;
  ComPtr<ID2D1PathGeometry> yetiHorn_;
};

}  // namespace snow::gfx
