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

bool FullscreenOn(const FsWindow& w, const FsMonitor& m, ShellNotifyState shell) {
  // The shell's own "busy" verdict tolerates apps that stop a pixel or two short
  // of the edges; without it the frame must cover the monitor exactly.
  int tol = w.foreground && shell == ShellNotifyState::Busy ? 2 : 0;
  if (!Covers(w.frame, m.rect, tol)) return false;
  if (w.zoomed && w.captioned && Within(w.frame, m.work, 1)) return false;  // ordinary maximised window
  return true;
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

}  // namespace

std::vector<bool> DetectFullscreen(const std::vector<FsMonitor>& monitors, const std::vector<FsWindow>& windows,
                                   ShellNotifyState shell) {
  std::vector<bool> out(monitors.size(), false);
  if (shell == ShellNotifyState::Presentation) {
    std::fill(out.begin(), out.end(), true);
    return out;
  }
  if (shell == ShellNotifyState::D3DFullscreen) {
    // Exclusive-mode D3D: the app's monitor (all of them if we can't tell which).
    int mi = -1;
    for (const auto& w : windows)
      if (w.foreground) mi = MonitorOf(monitors, w.frame);
    if (mi < 0) std::fill(out.begin(), out.end(), true);
    else out[static_cast<size_t>(mi)] = true;
  }
  for (size_t i = 0; i < monitors.size(); ++i) {
    if (out[i]) continue;
    const FsMonitor& m = monitors[i];
    for (const auto& w : windows) {
      if (!w.frame.intersects(m.rect)) continue;
      if (FullscreenOn(w, m, shell)) {
        out[i] = true;
        break;
      }
      if (w.topmost || w.floating) continue;
      break;
    }
  }
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
