// Decorations: persistent model + layout and hit-testing helpers (portable).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "geom.h"

namespace snow {

enum class OrnamentKind : int { Bauble = 0, CandyCane = 1 };

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
  int scarf = 0;             // index into theme scarf palette
};

struct DecorState {
  std::vector<TreeDecor> trees;
  std::vector<SnowmanDecor> snowmen;
  uint32_t nextId = 1;
};

constexpr int kMaxTrees = 3;
constexpr int kMaxSnowmen = 12;
constexpr int kMaxOrnamentsPerTree = 40;

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
PaletteLayout LayoutPalette(const RectI& monitorWork, float dpiScale);
bool PaletteHit(const PaletteLayout& layout, int x, int y, Tool& out);

}  // namespace snow
