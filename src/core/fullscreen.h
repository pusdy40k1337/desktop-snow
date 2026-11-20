// Fullscreen detection per monitor and its debounce. Portable: the Windows side
// collects the windows, this decides. Unit-tested.
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
  bool zoomed = false;      // maximised
  bool captioned = false;   // has a title bar (WS_CAPTION)
  bool floating = false;    // tool window or owned popup (player controls, palettes)
  bool foreground = false;
};

struct FsMonitor {
  RectI rect;  // full monitor
  RectI work;  // work area
};

// `windows`: candidate top-level windows in z-order, topmost first. The caller
// has already dropped hidden, minimised and cloaked windows, the desktop, the
// taskbars, shell popups (Alt+Tab, Start), click-through overlays and our own
// windows. Returns one flag per monitor: a fullscreen app is showing there.
//
// Per monitor, the first window on it decides. If it covers the whole monitor it
// is fullscreen (a maximised window with a title bar is not: it only reaches the
// monitor edges when the monitor has no taskbar). Always-on-top and floating
// windows that don't cover it (picture-in-picture, player controls) are looked
// through; any other window on top means no fullscreen app is showing.
std::vector<bool> DetectFullscreen(const std::vector<FsMonitor>& monitors, const std::vector<FsWindow>& windows,
                                   ShellNotifyState shell);

// Applies a new set of fullscreen monitors only after it has been stable for
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
