#include "tracker.h"

#include <dwmapi.h>

#include <algorithm>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace snow::win {

namespace {
WindowTracker* g_tracker = nullptr;  // WinEvent callbacks carry no user data
DWORD g_ownPid = 0;

float CornerRadiusFor(HWND hwnd, LONG style, float scale) {
  if (!IsWindows11() || IsZoomed(hwnd)) return 0.f;
  if (!(style & (WS_CAPTION | WS_THICKFRAME))) return 0.f;
  DWORD pref = 0;  // DWMWCP_DEFAULT
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)))) pref = 0;
  if (pref == 1) return 0.f;           // DWMWCP_DONOTROUND
  if (pref == 3) return 4.f * scale;   // DWMWCP_ROUNDSMALL
  return 8.f * scale;                  // default / ROUND
}
}  // namespace

bool WindowTracker::Describe(HWND hwnd, TrackedWindow& out, bool withText) {
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == g_ownPid) return false;
  if (IsCloaked(hwnd)) return false;  // other virtual desktops, UWP ghost frames, suspended apps
  LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
  if (style & WS_CHILD) return false;
  std::string cls = WindowClass(hwnd);
  bool taskbar = cls == "Shell_TrayWnd" || cls == "Shell_SecondaryTrayWnd";
  if (!taskbar && IsShellPopupClass(cls)) return false;
  // Floating tool windows without a title (shell previews, helper popups) must
  // never host snow: hovering taskbar icons used to tear the taskbar drift.
  if (!taskbar && (ex & WS_EX_TOOLWINDOW) && (style & WS_CAPTION) != WS_CAPTION) return false;
  if (!taskbar && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) return false;  // click-through overlays
  if (ex & WS_EX_LAYERED) {
    BYTE alpha = 255;
    DWORD flags = 0;
    if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return false;
  }
  RECT r;
  if (!GetFrameBounds(hwnd, r)) return false;
  int w = r.right - r.left, h = r.bottom - r.top;
  if (w < 2 || h < 2) return false;

  out.hwnd = hwnd;
  out.frame = r;
  out.pid = pid;
  out.className = cls;
  out.isTaskbar = taskbar;
  POINT topMid{r.left + w / 2, r.top};
  HMONITOR mon = MonitorFromPoint(topMid, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  out.scale = MonitorScale(mon);
  out.cornerRadius = taskbar ? 0.f : CornerRadiusFor(hwnd, style, out.scale);

  if (taskbar) {
    // Snow only on a bottom taskbar that is actually showing (not auto-hidden).
    out.holdsSnow = w > h && r.bottom >= mi.rcMonitor.bottom - 2 && (mi.rcMonitor.bottom - r.top) > 20;
  } else {
    bool tool = (ex & WS_EX_TOOLWINDOW) != 0;
    HWND owner = GetWindow(hwnd, GW_OWNER);
    bool captioned = (style & WS_CAPTION) == WS_CAPTION;
    out.holdsSnow = !tool && (owner == nullptr || captioned) && w >= static_cast<int>(120 * out.scale) &&
                    h >= static_cast<int>(48 * out.scale) && !IsZoomed(hwnd) &&
                    r.top > mi.rcWork.top + 1;  // maximised / snapped to the top: no room for snow
  }
  if (withText) {
    out.process = ProcessName(pid);
    out.title = WindowTitle(hwnd);
  }
  return true;
}

bool WindowTracker::Start(HWND notify) {
  if (running_) return true;
  notify_ = notify;
  g_tracker = this;
  g_ownPid = GetCurrentProcessId();
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  running_ = true;
  thread_ = std::thread([this] { ThreadMain(); });
  return true;
}

void WindowTracker::Stop() {
  if (!running_) return;
  running_ = false;
  SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
  g_tracker = nullptr;
}

std::shared_ptr<const TrackerSnapshot> WindowTracker::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return snap_;
}

void WindowTracker::HotWindows(double now, std::vector<HotWindow>& out) const {
  out.clear();
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [h, w] : hot_)
    if (w.moveSize || now - w.lastEvent < 0.35) out.push_back(w);
}

void WindowTracker::TakeDestroyed(std::vector<HWND>& out) {
  std::lock_guard<std::mutex> lock(mu_);
  out.swap(destroyed_);
  destroyed_.clear();
}

void WindowTracker::RequestRescan() { rescan_ = true; }

void CALLBACK WindowTracker::OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                     DWORD) {
  if (g_tracker) g_tracker->Handle(event, hwnd, idObject, idChild);
}

void WindowTracker::PostFullscreenCheck() {
  double now = Now();
  if (!notify_ || now - lastFsPost_ < 0.05) return;
  lastFsPost_ = now;
  PostMessageW(notify_, WM_APP_FS_CHECK, 0, 0);
}

void WindowTracker::Handle(DWORD event, HWND hwnd, LONG idObject, LONG idChild) {
  switch (event) {
    case EVENT_OBJECT_LOCATIONCHANGE: {
      if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
      if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
      double now = Now();
      {
        std::lock_guard<std::mutex> lock(mu_);
        HotWindow& h = hot_[hwnd];
        if (h.hwnd && now - h.lastEvent < 0.1) {
          double iv = now - h.lastEvent;
          h.interval = h.interval > 0 ? h.interval * 0.8 + iv * 0.2 : iv;
        }
        h.hwnd = hwnd;
        h.lastEvent = now;
      }
      if (std::find(dirtyRects_.begin(), dirtyRects_.end(), hwnd) == dirtyRects_.end()) dirtyRects_.push_back(hwnd);
      if (hwnd == GetForegroundWindow()) PostFullscreenCheck();  // F11, video player going fullscreen
      return;
    }
    case EVENT_SYSTEM_MOVESIZESTART:
    case EVENT_SYSTEM_MOVESIZEEND: {
      if (!hwnd) return;
      {
        std::lock_guard<std::mutex> lock(mu_);
        HotWindow& h = hot_[hwnd];
        h.hwnd = hwnd;
        h.moveSize = event == EVENT_SYSTEM_MOVESIZESTART;
        h.lastEvent = Now();
        dirtyStructure_ = true;
      }
      if (event == EVENT_SYSTEM_MOVESIZEEND) PostFullscreenCheck();
      return;
    }
    case EVENT_OBJECT_DESTROY: {
      if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
      bool tracked = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        hot_.erase(hwnd);
        if (snap_) {
          for (const auto& w : snap_->windows) {
            if (w.hwnd == hwnd) {
              destroyed_.push_back(hwnd);
              dirtyStructure_ = true;
              tracked = true;
              break;
            }
          }
        }
      }
      if (tracked) PostFullscreenCheck();
      return;
    }
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED: {
      if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
      bool root = GetAncestor(hwnd, GA_ROOT) == hwnd;
      if (!root && event != EVENT_OBJECT_HIDE) return;
      dirtyStructure_ = true;
      if (root && event != EVENT_OBJECT_CREATE) PostFullscreenCheck();
      return;
    }
    case EVENT_SYSTEM_FOREGROUND:
      dirtyStructure_ = true;
      if (notify_) PostMessageW(notify_, WM_APP_FOREGROUND, 0, 0);
      return;
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
      dirtyStructure_ = true;
      PostFullscreenCheck();
      return;
    case EVENT_SYSTEM_DESKTOPSWITCH:
      dirtyStructure_ = true;
      return;
    default:
      return;
  }
}

void WindowTracker::Publish(std::shared_ptr<TrackerSnapshot> snap) {
  std::lock_guard<std::mutex> lock(mu_);
  snap->seq = ++seq_;
  snap->time = Now();
  snap_ = std::move(snap);
  // Forget hot entries that have gone quiet.
  double now = snap_->time;
  for (auto it = hot_.begin(); it != hot_.end();) {
    if (!it->second.moveSize && now - it->second.lastEvent > 5.0) it = hot_.erase(it);
    else ++it;
  }
}

void WindowTracker::FullRebuild() {
  auto snap = std::make_shared<TrackerSnapshot>();
  struct Ctx {
    TrackerSnapshot* snap;
    int z;
  } ctx{snap.get(), 0};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        TrackedWindow w;
        if (Describe(hwnd, w, true)) {
          w.z = c->z++;
          c->snap->windows.push_back(std::move(w));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  Publish(std::move(snap));
  dirtyStructure_ = false;
  dirtyRects_.clear();
  lastFull_ = lastRebuild_ = Now();
}

void WindowTracker::UpdateRects() {
  std::shared_ptr<const TrackerSnapshot> cur = Snapshot();
  if (!cur) {
    FullRebuild();
    return;
  }
  auto snap = std::make_shared<TrackerSnapshot>(*cur);
  for (HWND h : dirtyRects_) {
    for (auto& w : snap->windows) {
      if (w.hwnd != h) continue;
      TrackedWindow fresh;
      if (!Describe(h, fresh, false)) {
        dirtyStructure_ = true;  // became invisible / cloaked: needs a full pass
        break;
      }
      fresh.z = w.z;
      fresh.process = std::move(w.process);
      fresh.title = std::move(w.title);
      w = std::move(fresh);
      break;
    }
  }
  dirtyRects_.clear();
  Publish(std::move(snap));
  lastRebuild_ = Now();
}

void WindowTracker::ThreadMain() {
  // Hook only the event ranges we need; out-of-context, skipping our own process.
  const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
  const DWORD ranges[][2] = {
      {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
      {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
      {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
      {EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH},
      {EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},  // CREATE, DESTROY, SHOW, HIDE
      {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
      {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
  };
  std::vector<HWINEVENTHOOK> hooks;
  for (const auto& r : ranges) {
    HWINEVENTHOOK h = SetWinEventHook(r[0], r[1], nullptr, &WindowTracker::OnEvent, 0, 0, flags);
    if (h) hooks.push_back(h);
    else Log("tracker: SetWinEventHook(0x%lx..0x%lx) failed: %lu", r[0], r[1], GetLastError());
  }
  Log("tracker: %zu hooks installed", hooks.size());
  FullRebuild();

  while (running_) {
    double now = Now();
    DWORD timeout = 1000;
    if (dirtyStructure_ || !dirtyRects_.empty() || rescan_) {
      double wait = lastRebuild_ + 0.004 - now;  // coalesce bursts, at most ~250 Hz
      timeout = wait <= 0 ? 0 : static_cast<DWORD>(wait * 1000.0) + 1;
    } else {
      double wait = lastFull_ + 1.0 - now;  // safety net: z-order changes have no event
      timeout = wait <= 0 ? 0 : static_cast<DWORD>(wait * 1000.0) + 1;
    }
    DWORD rc = MsgWaitForMultipleObjectsEx(1, &stopEvent_, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (rc == WAIT_OBJECT_0) break;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {  // WinEvent callbacks run in here
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    now = Now();
    if (rescan_.exchange(false)) dirtyStructure_ = true;
    if (now - lastRebuild_ < 0.004 && (dirtyStructure_ || !dirtyRects_.empty())) continue;
    if (dirtyStructure_ || now - lastFull_ >= 1.0) FullRebuild();
    else if (!dirtyRects_.empty()) UpdateRects();
  }
  for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
  Log("tracker: hooks removed");
}

}  // namespace snow::win
