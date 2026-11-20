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
    for (size_t k = 0; k < windows.size(); ++k) {
      const FsWindow& w = windows[k];
      RectI x = w.frame.intersect(m.rect);
      if (x.width() < kMinOverlap || x.height() < kMinOverlap) continue;
      // The shell's own "busy" verdict tolerates apps that stop a little further short.
      int tol = kCoverTolerance + (w.foreground && shell == ShellNotifyState::Busy ? 2 : 0);
      int idx = static_cast<int>(k);
      bool ordinaryMaximised = w.zoomed && w.captioned && Within(w.frame, m.work, tol);
      if (Covers(w.frame, m.rect, tol) && !ordinaryMaximised) {
        out[i] = Decide(PauseReason::Fullscreen, "covers the monitor", idx, options);
        break;
      }
      if (Covers(w.frame, m.work, tol)) {
        out[i] = Decide(PauseReason::Maximized, w.zoomed ? "maximized (zoomed)" : "covers the work area", idx, options);
        break;
      }
      if (w.topmost || w.floating) continue;
      out[i] = Decide(PauseReason::None, "top window does not cover", idx, options);
      break;
    }
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
