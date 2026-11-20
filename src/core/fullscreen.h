// Fullscreen / maximised detection per monitor and its debounce. Portable: the
// Windows side collects the windows, this decides. Unit-tested.
#pragma once
#include <string>
#include <vector>
#include "geom.h"

namespace snow {

// SHQueryUserNotificationState, platform-neutral.
enum class ShellNotifyState { Unknown, Normal, Busy, D3DFullscreen, Presentation };

struct FsWindow {
  RectI frame;              // visible frame (DWM extended frame bounds), physical px
  bool topmost = false;     // WS_EX_TOPMOST
  bool zoomed = false;      // maximised (IsZoomed / WS_MAXIMIZE)
  bool captioned = false;   // has a title bar (WS_CAPTION)
  bool floating = false;    // tool window or owned popup (player controls, palettes)
  bool foreground = false;
};

struct FsMonitor {
  RectI rect;  // full monitor
  RectI work;  // work area
};

// Frames may miss an edge by this much and still count as covering it: DPI
// scaling and some apps leave a pixel or two (125 %/150 %, auto-hide taskbars).
constexpr int kCoverTolerance = 2;
// A window must overlap a monitor by at least this much in both directions to
// take part in that monitor's decision (maximised borderless windows spill a
// few pixels onto the neighbouring monitor).
constexpr int kMinOverlap = 16;

enum class PauseReason { None, Fullscreen, Maximized };
const char* PauseReasonName(PauseReason r);  // "none" | "fullscreen" | "maximized"

struct PauseOptions {
  bool fullscreen = true;  // pause under fullscreen apps
  bool maximized = true;   // pause under maximised windows / windows filling the work area
};

struct PauseDecision {
  PauseReason reason = PauseReason::None;  // what is showing, even when that pause is switched off
  bool pause = false;                      // reason != None and its option is on
  int window = -1;                         // index into `windows` of the deciding window, -1 = none
  const char* rule = "no window";          // the rule that fired, for diagnostics
};

// `windows`: candidate top-level windows in z-order, topmost first. The caller
// has already dropped hidden, minimised and cloaked windows, the desktop, the
// taskbars, shell popups (Alt+Tab, Start), click-through overlays and our own
// overlay windows. Returns one decision per monitor.
//
// Per monitor, the first window on it decides:
//  - it covers the whole monitor -> Fullscreen (a maximised window with a title
//    bar that stays inside the work area is Maximized instead: it only reaches
//    the monitor edges when the monitor has no taskbar);
//  - it covers the whole work area (maximised, snapped full size, borderless
//    maximised) -> Maximized;
//  - always-on-top and floating windows that cover neither (picture-in-picture,
//    player controls) are looked through;
//  - any other window on top -> None: the user is working there.
// The shell's presentation mode pauses every monitor; exclusive-mode D3D pauses
// the foreground window's monitor (all of them when that is unknown).
std::vector<PauseDecision> DecidePause(const std::vector<FsMonitor>& monitors, const std::vector<FsWindow>& windows,
                                       ShellNotifyState shell, const PauseOptions& options);

// Fullscreen only (maximised windows ignored): one flag per monitor.
std::vector<bool> DetectFullscreen(const std::vector<FsMonitor>& monitors, const std::vector<FsWindow>& windows,
                                   ShellNotifyState shell);

// Applies a new set of paused monitors only after it has been stable for
// `delay` seconds, so Alt+Tab and window transitions don't flicker the snow.
class FullscreenDebouncer {
 public:
  explicit FullscreenDebouncer(double delay = 0.3) : delay_(delay) {}
  // Feeds the latest detection (sorted monitor ids). Returns true when the
  // applied set changed.
  bool update(double now, std::vector<std::string> raw);
  // Applies `state` at once (setting toggled, startup).
  void reset(std::vector<std::string> state);
  const std::vector<std::string>& applied() const { return applied_; }
  // Seconds until the pending change is due; negative when nothing is pending.
  double pendingIn(double now) const;

 private:
  double delay_;
  std::vector<std::string> applied_, pending_;
  double pendingSince_ = 0;
  bool hasPending_ = false;
};

}  // namespace snow
