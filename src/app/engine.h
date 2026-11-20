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
  Lang lang = Lang::En;
  std::string diagText;   // pause decisions for the debug layer, one line per monitor
  HWND keepAbove = nullptr;  // our settings window: overlays stay below it while it is topmost
};

enum class EngineCommand { ClearSnow, Santa, Train, Yeti, MonitorsChanged, ReassertTopmost };

struct DecorLayoutItem {
  uint32_t id = 0;
  bool tree = false;
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
  std::vector<HWND> OverlayWindows();
  double Fps() const;  // overlay frame rate over the last second (0 before the first second)

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace snow::win
