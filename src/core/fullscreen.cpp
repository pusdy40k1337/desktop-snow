#include "fullscreen.h"

#include <algorithm>

namespace snow {

namespace {

bool Covers(const RectI& w, const RectI& m, int tol) {
  return w.left <= m.left + tol && w.top <= m.top + tol && w.right >= m.right - tol && w.bottom >= m.bottom - tol;
}

bool Within(const RectI& w, const RectI& outer, int tol) {
  return w.left >= outer.left - tol && w.top >= outer.top - tol && w.right <= outer.right + tol &&
         w.bottom <= outer.bottom + tol;
}

int MonitorOf(const std::vector<FsMonitor>& monitors, const RectI& r) {
  int best = -1;
  long long bestArea = 0;
  for (size_t i = 0; i < monitors.size(); ++i) {
    RectI x = r.intersect(monitors[i].rect);
    long long area = static_cast<long long>(x.width()) * x.height();
    if (area > bestArea) {
      bestArea = area;
      best = static_cast<int>(i);
    }
  }
  return best;
}

PauseDecision Decide(PauseReason reason, const char* rule, int window, const PauseOptions& o) {
  PauseDecision d;
  d.reason = reason;
  d.rule = rule;
  d.window = window;
  d.pause = (reason == PauseReason::Fullscreen && o.fullscreen) || (reason == PauseReason::Maximized && o.maximized);
  return d;
}

// A window that fills the monitor (F11 browser, video player, game) but keeps a
// normal title bar inside the work area is just a maximised window: only it
// reaching the monitor edges (no taskbar, borderless) makes it "fullscreen".
bool OrdinaryMaximised(const FsWindow& w, const FsMonitor& m, int tol) {
  return w.zoomed && w.captioned && Within(w.frame, m.work, tol);
}

// Share of the monitor's work area covered by the union of all candidate
// windows, sampled on a 16x9 grid. Two windows snapped side by side cover the
// desktop completely without either being "maximised".
float WorkCoverage(const FsMonitor& m, const std::vector<FsWindow>& windows) {
  constexpr int kGX = 16, kGY = 9;
  if (m.work.width() <= 0 || m.work.height() <= 0) return 0.f;
  int covered = 0;
  for (int gy = 0; gy < kGY; ++gy) {
    const long cy = m.work.top + (static_cast<long>(gy) * 2 + 1) * m.work.height() / (2 * kGY);
    for (int gx = 0; gx < kGX; ++gx) {
      const long cx = m.work.left + (static_cast<long>(gx) * 2 + 1) * m.work.width() / (2 * kGX);
      for (const FsWindow& w : windows) {
        if (cx >= w.frame.left && cx < w.frame.right && cy >= w.frame.top && cy < w.frame.bottom) {
          ++covered;
          break;
        }
      }
    }
  }
  return static_cast<float>(covered) / static_cast<float>(kGX * kGY);
}

constexpr float kCollectiveCoverage = 0.90f;

}  // namespace

const char* PauseReasonName(PauseReason r) {
  switch (r) {
    case PauseReason::Fullscreen: return "fullscreen";
    case PauseReason::Maximized: return "maximized";
    default: return "none";
  }
}

std::vector<PauseDecision> DecidePause(const std::vector<FsMonitor>& monitors, const std::vector<FsWindow>& windows,
                                       ShellNotifyState shell, const PauseOptions& options) {
  std::vector<PauseDecision> out(monitors.size());
  if (shell == ShellNotifyState::Presentation) {
    for (auto& d : out) d = Decide(PauseReason::Fullscreen, "shell: presentation mode", -1, options);
    return out;
  }
  std::vector<bool> decided(monitors.size(), false);
  if (shell == ShellNotifyState::D3DFullscreen) {
    // Exclusive-mode D3D: the app's monitor (all of them if we can't tell which).
    int mi = -1, wi = -1;
    for (size_t i = 0; i < windows.size(); ++i)
      if (windows[i].foreground) {
        mi = MonitorOf(monitors, windows[i].frame);
        wi = static_cast<int>(i);
      }
    for (size_t i = 0; i < monitors.size(); ++i) {
      if (mi >= 0 && static_cast<int>(i) != mi) continue;
      out[i] = Decide(PauseReason::Fullscreen, "shell: exclusive fullscreen (D3D)", mi >= 0 ? wi : -1, options);
      decided[i] = true;
    }
  }
  for (size_t i = 0; i < monitors.size(); ++i) {
    if (decided[i]) continue;
    const FsMonitor& m = monitors[i];
    // Any window on this monitor that covers the monitor is a fullscreen app,
    // no matter where it sits in the z-order: the user who opens a small window
    // (WinRAR, a file dialog, Spotify) over a fullscreen Chrome or video is
    // still inside that fullscreen app and the snow must stay paused.
    int coverIdx = -1, workIdx = -1, topIdx = -1;
    for (size_t k = 0; k < windows.size(); ++k) {
      const FsWindow& w = windows[k];
      RectI x = w.frame.intersect(m.rect);
      if (x.width() < kMinOverlap || x.height() < kMinOverlap) continue;
      if (topIdx < 0) topIdx = static_cast<int>(k);
      // The shell's own "busy" verdict tolerates apps that stop a little further short.
      int tol = kCoverTolerance + (w.foreground && shell == ShellNotifyState::Busy ? 2 : 0);
      if (coverIdx < 0 && Covers(w.frame, m.rect, tol) && !OrdinaryMaximised(w, m, tol))
        coverIdx = static_cast<int>(k);
      if (workIdx < 0 && Covers(w.frame, m.work, tol)) workIdx = static_cast<int>(k);
    }
    if (coverIdx >= 0) {
      out[i] = Decide(PauseReason::Fullscreen, "covers the monitor (any z-order)", coverIdx, options);
      continue;
    }
    // Windows tiled or snapped side by side can hide the desktop completely
    // without any single one being maximised: count their union and pause once
    // it covers at least 90 % of the work area.
    if (workIdx < 0 && WorkCoverage(m, windows) >= kCollectiveCoverage) {
      out[i] = Decide(PauseReason::Maximized, "windows cover the work area (>=90%)", -1, options);
      continue;
    }
    // Same for the work area: a maximised browser stays paused when a small
    // foreground window (Spotify, a chat) is opened on top of it.
    if (workIdx >= 0) {
      const FsWindow& w = windows[static_cast<size_t>(workIdx)];
      out[i] = Decide(PauseReason::Maximized, w.zoomed ? "maximized (zoomed)" : "covers the work area", workIdx, options);
      continue;
    }
    if (topIdx >= 0) {
      // Nothing on this monitor fills the screen or the work area: the user is
      // working in a normal window, so the snow may fall. There is no "top
      // window does not cover" rule any more - a small window can never cancel
      // the pause of a fullscreen/maximised window underneath it.
      out[i] = Decide(PauseReason::None, "no covering window", topIdx, options);
      continue;
    }
    out[i] = Decide(PauseReason::None, "no window", -1, options);
  }
  return out;
}

std::vector<bool> DetectFullscreen(const std::vector<FsMonitor>& monitors, const std::vector<FsWindow>& windows,
                                   ShellNotifyState shell) {
  PauseOptions o;
  o.maximized = false;
  std::vector<PauseDecision> d = DecidePause(monitors, windows, shell, o);
  std::vector<bool> out(d.size());
  for (size_t i = 0; i < d.size(); ++i) out[i] = d[i].reason == PauseReason::Fullscreen;
  return out;
}

bool FullscreenDebouncer::update(double now, std::vector<std::string> raw) {
  std::sort(raw.begin(), raw.end());
  if (raw == applied_) {
    hasPending_ = false;
    return false;
  }
  if (!hasPending_ || raw != pending_) {
    pending_ = std::move(raw);
    pendingSince_ = now;
    hasPending_ = true;
  }
  if (now - pendingSince_ + 1e-9 < delay_) return false;
  applied_ = pending_;
  hasPending_ = false;
  return true;
}

void FullscreenDebouncer::reset(std::vector<std::string> state) {
  std::sort(state.begin(), state.end());
  applied_ = std::move(state);
  hasPending_ = false;
}

double FullscreenDebouncer::pendingIn(double now) const {
  if (!hasPending_) return -1;
  return std::max(0.0, delay_ - (now - pendingSince_));
}

}  // namespace snow
