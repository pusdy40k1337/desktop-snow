// Event-driven top-level window tracker (SetWinEventHook on a dedicated thread).
#pragma once
#include "platform.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace snow::win {

struct TrackedWindow {
  HWND hwnd = nullptr;
  RECT frame{};          // DWMWA_EXTENDED_FRAME_BOUNDS (visible edges), physical px
  int z = 0;             // 0 = topmost
  bool holdsSnow = false;
  bool isTaskbar = false;
  float cornerRadius = 0;  // px
  float scale = 1;         // DPI scale of the monitor under the top edge
  DWORD pid = 0;
  std::string process, className, title;
};

struct TrackerSnapshot {
  uint64_t seq = 0;
  double time = 0;
  std::vector<TrackedWindow> windows;  // z-order, topmost first
};

struct HotWindow {
  HWND hwnd = nullptr;
  double lastEvent = 0;     // QPC seconds of the latest LOCATIONCHANGE
  double interval = 0;      // smoothed interval between LOCATIONCHANGE events
  bool moveSize = false;    // inside a user move/resize loop
};

class WindowTracker {
 public:
  WindowTracker() = default;
  ~WindowTracker() { Stop(); }
  bool Start(HWND notify);
  void Stop();
  std::shared_ptr<const TrackerSnapshot> Snapshot() const;
  // Windows that moved recently (or are being dragged): re-read every frame.
  void HotWindows(double now, std::vector<HotWindow>& out) const;
  void TakeDestroyed(std::vector<HWND>& out);
  void RequestRescan();
  // Evaluates eligibility and geometry of one window (also used per frame for hot windows).
  static bool Describe(HWND hwnd, TrackedWindow& out, bool withText);

 private:
  static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
  void ThreadMain();
  void Handle(DWORD event, HWND hwnd, LONG idObject, LONG idChild);
  void FullRebuild();
  void UpdateRects();
  void Publish(std::shared_ptr<TrackerSnapshot> snap);
  // Asks the app to re-check fullscreen (rate limited; a timer catches the rest).
  void PostFullscreenCheck();

  HWND notify_ = nullptr;
  std::thread thread_;
  HANDLE stopEvent_ = nullptr;
  std::atomic<bool> running_{false};

  mutable std::mutex mu_;
  std::shared_ptr<const TrackerSnapshot> snap_;
  std::unordered_map<HWND, HotWindow> hot_;
  std::vector<HWND> destroyed_;
  uint64_t seq_ = 0;

  // tracker-thread only
  bool dirtyStructure_ = true;
  std::vector<HWND> dirtyRects_;
  double lastRebuild_ = 0, lastFull_ = 0;
  double lastFsPost_ = 0;
  std::atomic<bool> rescan_{false};
};

}  // namespace snow::win
