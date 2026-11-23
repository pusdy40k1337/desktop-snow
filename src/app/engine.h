// Render thread: per-monitor click-through DirectComposition overlays, frame
// pacing synchronised with DWM composition, simulation and drawing.
#pragma once
#include "platform.h"
#include "tracker.h"

#include <memory>
#include <string>
#include <vector>

#include "../core/decor.h"
#include "../core/settings.h"
#include "../core/strings.h"

namespace snow::win {

struct LaunchOptions {
  std::wstring instance = L"main";
  bool harness = false;       // measurement mode for SnowHarness.exe
  bool console = false;
  bool debug = false;         // force the debug overlay on
  bool noPersist = false;
  std::wstring telemetryPath; // per-frame CSV (window vs pile)
  std::wstring logPath;       // --log: append the log here instead of %LOCALAPPDATA%\DesktopSnow\logs
  bool diagnostics = false;   // --diagnostics: log every pause decision every 0.5 s
  bool trainNow = false;      // --train-now: send the gift train once, ~2 s after start
  int quitAfter = 0;          // --quit-after <s>: exit after this many seconds (0 = never)
  std::string syncOverride;   // "latch" | "vsync" | ""
  int predictOverride = -1;   // -1 = settings, 0 off, 1 on
  std::string predictorOverride;  // "linear" | "stepped" | ""
  float latchMarginOverride = -1;  // ms, -1 = settings
};

struct EngineConfig {
  Settings settings;
  bool paused = false;          // snow off, screen locked, sleeping...
  bool batteryMode = false;     // halve intensity, cap fps at 30
  bool remoteSession = false;   // RDP: cap fps, no busy-wait pacing
  bool reducedMotion = false;
  float night = 0;
  std::vector<std::string> hiddenMonitors;  // monitors showing a fullscreen app / maximised window, or switched off
  bool decorateMode = false;
  Tool activeTool = Tool::Tree;
  uint32_t hoverId = 0;
  // Toolbar position (absolute px, -1 = the default top-centre spot).
  int paletteLeft = -1;
  int paletteTop = -1;
  // Colour picker popup for hats/scarves (drawing only; hit-testing is in App).
  bool pickerOpen = false;
  int pickerX = 0, pickerY = 0;
  int pickerSelected = 0;
  Lang lang = Lang::En;
  std::string diagText;   // pause decisions for the debug layer, one line per monitor
  HWND keepAbove = nullptr;  // our settings window: overlays stay below it while it is topmost
};

enum class EngineCommand { ClearSnow, ClearSnowballs, Santa, Train, Yeti, MonitorsChanged, ReassertTopmost };

struct DecorLayoutItem {
  uint32_t id = 0;
  bool tree = false;
  int cls = 0;         // 0 = tree, 1 = snowman, 2 = free decoration
  float left = 0, top = 0, right = 0, bottom = 0;  // hit box, absolute px
  float baseX = 0, baseY = 0, height = 0;
  uint64_t hwnd = 0;   // snowman: window it stands on (0 = taskbar / ground)
  bool visible = true;
};

struct SnowmanUpdate {
  uint32_t id = 0;
  float relX = 0.5f;
  bool anchorChanged = false;
  WindowAnchor anchor;
};

// A tower of snowballs rolled into a snowman: the app turns it into a real
// SnowmanDecor standing on that surface edge.
struct SnowballSnowmanSpawn {
  uint64_t surface = 0;   // HWND (0 = taskbar/ground)
  float relX = 0.5f;      // 0..1 along the edge
  float scale = 1.f;      // DPI scale of the surface
  bool taskbar = false;   // the tower stands on the taskbar
  // Radii of the balls the user actually rolled, as fractions of the height,
  // plus how many tiers the tower has (2 = body + head, 3 = with the middle).
  float fBottom = 0.20f;
  float fMiddle = 0.15f;
  float fTop = 0.112f;
  int layers = 3;
};

class Engine {
 public:
  Engine();
  ~Engine();
  bool Start(const LaunchOptions& opts, WindowTracker* tracker, HWND notify);
  void Stop();
  void SetConfig(const EngineConfig& cfg);
  void SetDecor(const DecorState& decor);
  void Command(EngineCommand c);
  void GetDecorLayout(std::vector<DecorLayoutItem>& out);
  void TakeSnowmanUpdates(std::vector<SnowmanUpdate>& out);
  void TakeSnowballSpawns(std::vector<SnowballSnowmanSpawn>& out);
  std::vector<HWND> OverlayWindows();
  double Fps() const;  // overlay frame rate over the last second (0 before the first second)

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace snow::win
