// Decorations: persistent model + layout and hit-testing helpers (portable).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "geom.h"

namespace snow {

enum class OrnamentKind : int { Bauble = 0, CandyCane = 1 };

// Free-standing decorations: placed anywhere on the desktop (not attached to a
// tree or a window edge). Hat/Scarf pieces can additionally be worn by a
// snowman when dropped onto it.
enum class FreeKind : int { Bauble = 0, CandyCane, Star, Hat, Scarf };

struct FreeDecor {
  uint32_t id = 0;
  FreeKind kind = FreeKind::Bauble;
  std::string monitor;         // monitor device name; empty = primary
  float nx = 0.5f, ny = 0.5f;  // centre, normalized to the monitor work area
  float scale = 1.0f;
  int color = 0;               // index into the decor colour palette
  int variant = 1;             // Hat: 1 top hat, 2 beanie, 3 bucket
};

struct Ornament {
  OrnamentKind kind = OrnamentKind::Bauble;
  float u = 0.5f, v = 0.5f;  // position inside the tree box, 0..1 (v=0 top, v=1 bottom)
  int color = 0;             // index into the theme ornament palette
};

struct TreeDecor {
  uint32_t id = 0;
  std::string monitor;       // monitor device name ("\\\\.\\DISPLAY1"); empty = primary
  float nx = 0.5f, ny = 1.0f;  // trunk base position, normalized to the monitor work area
  float scale = 1.0f;
  float grow = 1.0f;         // 0..1 growth animation progress (runtime, persisted as 1)
  bool star = false;
  bool lights = false;
  std::vector<Ornament> ornaments;
};

// Identifies "the same window" across moves and restarts.
struct WindowAnchor {
  bool taskbar = false;
  uint64_t hwnd = 0;         // last known handle (only trusted together with process+class)
  std::string process;       // exe file name, lower-case
  std::string className;
  std::string title;
};

struct SnowmanDecor {
  uint32_t id = 0;
  WindowAnchor anchor;
  float relX = 0.5f;         // 0..1 along the anchored edge
  float scale = 1.0f;
  int hat = 1;               // 0 none, 1 top hat, 2 beanie, 3 bucket
  int scarf = 0;             // index into the decor colour palette
  int hatColor = 0;          // index into the decor colour palette
  bool free = false;         // free-standing anywhere on the desktop
  std::string monitor;       // free: monitor device name (empty = primary)
  float nx = 0.5f, ny = 1.0f;  // free: base point normalized to the work area
  // A snowman rolled from real snowballs keeps the radii the user built it
  // with (fractions of its height, 0 = the classic proportions), and how many
  // ball tiers it has (2 = body + head, 3 = body + middle + head).
  float rBottom = 0.f;
  float rMiddle = 0.f;
  float rTop = 0.f;
  int layers = 3;
};

// Unit-space placement of the balls (1 = full height, y = 0 at the base).
// Classic snowmen keep the exact original numbers; snowmen built from rolled
// balls use their saved radii and stack without gaps:
//   y_i = y_{i-1} - r_{i-1} - r_i + overlap
struct SnowmanParts {
  int layers = 3;
  float rb = 0.200f, rm = 0.150f, rh = 0.112f;
  float cbY = -0.200f, cmY = -0.515f, chY = -0.747f;
};

SnowmanParts SnowmanProportions(const SnowmanDecor& sm);

struct DecorState {
  std::vector<TreeDecor> trees;
  std::vector<SnowmanDecor> snowmen;
  std::vector<FreeDecor> free;
  uint32_t nextId = 1;
};

// "Remove all decorations": trees (with their ornaments), snowmen and the
// free-standing pieces. Rolled snowballs live in the renderer and are cleared
// separately.
inline void ClearAllDecorations(DecorState& d) {
  d.trees.clear();
  d.snowmen.clear();
  d.free.clear();
}

// "Remove snowmen": only snowmen (placed and rolled ones); trees and the other
// decorations stay exactly where they are.
inline void ClearSnowmenOnly(DecorState& d) { d.snowmen.clear(); }

constexpr int kMaxTrees = 3;
constexpr int kMaxSnowmen = 12;
constexpr int kMaxFreeDecor = 24;
constexpr int kMaxOrnamentsPerTree = 40;

// Shared colour palette for free decorations, scarves and hats.
constexpr int kDecorColorCount = 10;
uint32_t DecorColorRgb(int index);  // wraps around

// Nominal size (height in px at scale 1 / 96 DPI) and width-to-height ratio.
float FreeDecorBaseSize(FreeKind kind);
float FreeDecorAspect(FreeKind kind);

// Tree geometry: base = bottom centre of the trunk; height in px.
struct TreeBox {
  float left, top, right, bottom;
  bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};
float TreeWidth(float height);
TreeBox TreeBounds(Vec2 base, float height);
// True if (u,v) lies on the foliage (where ornaments can hang).
bool TreeFoliageContains(float u, float v);
Vec2 TreeLocalToScreen(Vec2 base, float height, float u, float v);
bool TreeScreenToLocal(Vec2 base, float height, Vec2 p, float& u, float& v);

// Snowman geometry: base = point on the edge it stands on.
struct SnowmanBox { float left, top, right, bottom; };
SnowmanBox SnowmanBounds(Vec2 base, float height);
float SnowmanHeightPx(float scale, float dpiScale);  // default ~92 DIP
float TreeHeightPx(float scale, float dpiScale);     // default ~230 DIP

// Window identity matching. Score 0 = no match; higher is better.
struct WindowIdentity {
  uint64_t hwnd = 0;
  std::string process;
  std::string className;
  std::string title;
  bool taskbar = false;
};
int AnchorMatchScore(const WindowAnchor& a, const WindowIdentity& w);
constexpr int kAnchorMinScore = 40;
// Normalises a title for fuzzy matching: lower-case ASCII, strips a leading "*"
// (unsaved marker) and trailing " - <app>" style suffix noise is kept but
// compared via longest common suffix/prefix ratios.
std::string NormalizeTitle(const std::string& title);

// Decorate-mode palette (drawn by the overlay, hit-tested by the input catcher).
enum class Tool : int { Tree = 0, Bauble, Lights, CandyCane, Star, Snowman, Hat, Scarf, Erase, Done, Count };
struct PaletteButton { Tool tool; RectI rect; };
struct PaletteLayout { RectI bar; std::vector<PaletteButton> buttons; };
// left/top: absolute position of the bar; -1 (or anything off-screen) keeps the
// default centred-at-the-top spot. The bar is always kept fully inside `work`.
PaletteLayout LayoutPalette(const RectI& monitorWork, float dpiScale, int left = -1, int top = -1);
bool PaletteHit(const PaletteLayout& layout, int x, int y, Tool& out);
// The toolbar grabs the mouse as a whole: buttons, the frame around them and a
// few pixels of slack. Everything inside this rect must be swallowed.
bool PaletteUiHit(const PaletteLayout& layout, int x, int y, int margin = 6);
// Drag: where the bar should end up when its top-left is pulled to (left, top).
RectI ClampPaletteRect(RectI bar, const RectI& monitorWork);
// The hint plate under (bar in the upper half of the screen) or above the bar.
RectI HintRect(const RectI& monitorWork, const RectI& bar, float dpiScale);

// Colour picker popup for hats/scarves (anchored at the click point).
struct ColorPickerLayout { RectI bar; std::vector<RectI> swatches; };
ColorPickerLayout LayoutColorPicker(int anchorX, int anchorY, const RectI& monitorWork, float dpiScale);
// `margin` grows every swatch by that many px (the popup is a UI element: a
// near miss inside its frame must not fall through to the canvas).
int ColorPickerHit(const ColorPickerLayout& layout, int x, int y, int margin = 0);  // -1 = miss
bool ColorPickerUiHit(const ColorPickerLayout& layout, int x, int y, int margin = 6);

}  // namespace snow
