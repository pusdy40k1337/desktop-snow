#include "app.h"

#ifndef SNOW_BUILD_ID
#define SNOW_BUILD_ID "dev"
#endif

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "../core/theme.h"
#include "../core/ui_strings.h"

namespace snow::win {

namespace {

enum : UINT {
  ID_ENABLED = 100,
  ID_SETTINGS,
  ID_PAUSE_HOUR,
  ID_QUIT,
};

enum : UINT_PTR { T_FULLSCREEN = 1, T_SAVE, T_MONITORS, T_SLOW, T_TOPMOST, T_QUIT_AFTER, T_FS_DEBOUNCE, T_PAUSE_HOUR, T_TRAIN_NOW };
constexpr UINT HK_TOGGLE = 1, HK_DECORATE = 2;
constexpr UINT WM_APP_SECOND_INSTANCE = WM_APP + 10;
constexpr size_t kDiagHistory = 200;  // decision changes kept for "Copy diagnostics"

App* g_app = nullptr;

// GUID_POWER_SAVING_STATUS / GUID_ACDC_POWER_SOURCE / GUID_CONSOLE_DISPLAY_STATE
const GUID kPowerSaving = {0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}};
const GUID kAcDc = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
const GUID kDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

std::wstring W(const char* s) { return Widen(s); }
std::wstring W(UiStr s, Lang l) { return Widen(UiTr(s, l)); }

std::string Fmt(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

std::string RectText(const RectI& r) { return Fmt("%d,%d,%d,%d", r.left, r.top, r.right, r.bottom); }

std::string LocalTimeText(bool withDate) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  if (withDate)
    return Fmt("%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  return Fmt("%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

}  // namespace

const char* App::T(Str s) const { return Tr(s, lang_); }

// ------------------------------------------------------------------- state

void App::LoadState() {
  std::wstring dir = AppDataDir();
  std::wstring file = dir + L"\\" + (opts_.instance == L"main" ? std::wstring(L"state.json") : opts_.instance + L".json");
  store_ = std::make_unique<StateStore>(std::filesystem::path(file));
  LoadResult r = store_->load(state_, std::filesystem::path(LegacySettingsPath()));
  Log("state: file=%d backup=%d quarantined=%d legacy=%d newer=%d %s", r.fromFile, r.fromBackup,
      r.corruptFileQuarantined, r.migratedLegacy, r.newerVersion, r.message.c_str());
  if (r.corruptFileQuarantined) Balloon(T(Str::TipStateRecovered));
  // The registry is the source of truth for autostart (users can remove it in Task Manager).
  state_.settings.launchAtStartup = AutostartEnabled();
  // The debug layer is opt-in only: a persisted debugOverlay flag is ignored
  // unless this run was started with --debug (TEST.cmd/harness pass it).
  if (opts_.debug) state_.settings.debugOverlay = true;
  else state_.settings.debugOverlay = false;
}

void App::ScheduleSave() {
  if (opts_.noPersist) return;
  SetTimer(hwnd_, T_SAVE, 600, nullptr);
}

void App::SaveNow() {
  KillTimer(hwnd_, T_SAVE);
  if (opts_.noPersist || !store_) return;
  std::string err;
  if (!store_->save(state_, &err)) Log("state: save failed: %s", err.c_str());
}

void App::RefreshSystemState() {
  PowerState p = QueryPower();
  onBattery_ = p.onBattery;
  battery_ = state_.settings.batterySaver && (p.onBattery || p.saver);
  remote_ = GetSystemMetrics(SM_REMOTESESSION) != 0;
  const std::string& rm = state_.settings.reducedMotion;
  reduced_ = rm == "on" || (rm == "auto" && !SystemAnimationsEnabled());
  time_t t = time(nullptr);
  tm lt{};
  localtime_s(&lt, &t);
  night_ = NightFactor(lt.tm_hour, lt.tm_min);
  lang_ = ResolveLanguage(state_.settings.language, UiLanguagePrimaryId());
}

void App::PushConfig() {
  EngineConfig cfg;
  cfg.settings = state_.settings;
  cfg.paused = !state_.settings.enabled || systemPaused_ || displayOff_ || pauseUntil_ > 0 ||
               (state_.settings.pauseOnBattery && onBattery_);
  cfg.batteryMode = battery_;
  cfg.remoteSession = remote_;
  cfg.reducedMotion = reduced_;
  cfg.night = night_;
  cfg.hiddenMonitors = hiddenMonitors_;
  for (const auto& id : state_.settings.disabledMonitors)
    if (std::find(cfg.hiddenMonitors.begin(), cfg.hiddenMonitors.end(), id) == cfg.hiddenMonitors.end())
      cfg.hiddenMonitors.push_back(id);
  cfg.decorateMode = decorate_;
  cfg.activeTool = tool_;
  cfg.hoverId = hoverId_;
  cfg.paletteLeft = state_.settings.paletteLeft;
  cfg.paletteTop = state_.settings.paletteTop;
  cfg.pickerOpen = colorPopup_.open;
  cfg.pickerX = colorPopup_.at.x;
  cfg.pickerY = colorPopup_.at.y;
  cfg.pickerSelected = colorPopup_.selected;
  cfg.lang = lang_;
  cfg.keepAbove = settingsWnd_.Hwnd();
  for (const auto& d : diag_) {
    cfg.diagText += Fmt("%s: %s - %s%s%s\n", d.id.c_str(), d.decision.pause ? "PAUSED" : "snowing",
                        PauseReasonName(d.decision.reason), d.exe.empty() ? "" : ", ", d.exe.c_str());
    cfg.diagText += Fmt("  rule: %s\n", d.decision.rule);
  }
  if (!diagShell_.empty()) cfg.diagText += "shell: " + diagShell_ + "\n";
  if (opts_.harness) {
    cfg.paused = false;
    cfg.hiddenMonitors.clear();
  }
  engine_.SetConfig(cfg);
  // Keep the decorate layer above everything (the snow overlay may be shown or
  // re-stacked by the engine thread at any moment).
  if (decorate_) {
    for (HWND h : catchers_)
      SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
}

void App::PushDecor() { engine_.SetDecor(state_.decor); }

namespace {

ShellNotifyState QueryShellState(std::string& text) {
  QUERY_USER_NOTIFICATION_STATE q{};
  if (FAILED(SHQueryUserNotificationState(&q))) {
    text = "query failed";
    return ShellNotifyState::Unknown;
  }
  static const char* kNames[] = {"?",
                                 "QUNS_NOT_PRESENT",
                                 "QUNS_BUSY",
                                 "QUNS_RUNNING_D3D_FULL_SCREEN",
                                 "QUNS_PRESENTATION_MODE",
                                 "QUNS_ACCEPTS_NOTIFICATIONS",
                                 "QUNS_QUIET_TIME",
                                 "QUNS_APP"};
  int v = static_cast<int>(q);
  text = Fmt("%s(%d)", v >= 1 && v <= 7 ? kNames[v] : "?", v);
  switch (q) {
    case QUNS_BUSY: return ShellNotifyState::Busy;
    case QUNS_RUNNING_D3D_FULL_SCREEN: return ShellNotifyState::D3DFullscreen;
    case QUNS_PRESENTATION_MODE: return ShellNotifyState::Presentation;
    default: return ShellNotifyState::Normal;
  }
}

std::string DescribeWindow(HWND h, const RECT* frame = nullptr) {
  if (!h) return "none";
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  RECT r{};
  if (frame) r = *frame;
  else GetFrameBounds(h, r);
  LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
  return Fmt("0x%08llX class=\"%s\" exe=%s frame=%s zoomed=%d caption=%d topmost=%d floating=%d",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)), WindowClass(h).c_str(),
             ProcessName(pid).c_str(), RectText(RectI(r.left, r.top, r.right, r.bottom)).c_str(), IsZoomed(h) ? 1 : 0,
             (style & WS_CAPTION) == WS_CAPTION ? 1 : 0, (ex & WS_EX_TOPMOST) ? 1 : 0,
             ((ex & WS_EX_TOOLWINDOW) || GetWindow(h, GW_OWNER)) ? 1 : 0);
}

// Top-level windows that could be a fullscreen app or a maximised window, in
// z-order (topmost first); `hwnds` gets the matching handles. Our own windows
// are skipped except `ownAllowed` (the settings window: while it is on top
// the user wants to see the snow they are adjusting).
std::vector<FsWindow> FullscreenCandidates(std::vector<HWND>& hwnds, HWND ownAllowed) {
  struct Ctx {
    std::vector<FsWindow> out;
    std::vector<HWND>* hwnds;
    HWND fgRoot;
    HWND ownAllowed;
    DWORD ownPid;
  } ctx{{}, &hwnds, nullptr, ownAllowed, GetCurrentProcessId()};
  hwnds.clear();
  HWND fg = GetForegroundWindow();
  ctx.fgRoot = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if ((pid == c->ownPid && h != c->ownAllowed) || IsCloaked(h)) return TRUE;
        LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (style & WS_CHILD) return TRUE;
        if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) return TRUE;  // click-through overlays (GPU overlays...)
        if (ex & WS_EX_LAYERED) {
          BYTE alpha = 255;
          DWORD flags = 0;
          if (GetLayeredWindowAttributes(h, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return TRUE;
        }
        std::string cls = WindowClass(h);
        if (cls == "Shell_TrayWnd" || cls == "Shell_SecondaryTrayWnd" || IsShellPopupClass(cls)) return TRUE;
        // Shell thumbnails / tool popups must not influence the pause decision.
        if ((ex & WS_EX_TOOLWINDOW) && (style & WS_CAPTION) != WS_CAPTION) return TRUE;
        RECT r;
        if (!GetFrameBounds(h, r) || r.right - r.left < 2 || r.bottom - r.top < 2) return TRUE;
        FsWindow w;
        w.frame = RectI(r.left, r.top, r.right, r.bottom);
        w.topmost = (ex & WS_EX_TOPMOST) != 0;
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        const bool placedMax = GetWindowPlacement(h, &wp) && PlacementSaysMaximized(wp.showCmd, wp.flags);
        // Pause from the first frame of the restore animation, not after it.
        w.zoomed = IsZoomed(h) != FALSE || placedMax;
        w.captioned = (style & WS_CAPTION) == WS_CAPTION;
        w.floating = (ex & WS_EX_TOOLWINDOW) != 0 || GetWindow(h, GW_OWNER) != nullptr;
        w.foreground = h == c->fgRoot;
        c->out.push_back(w);
        c->hwnds->push_back(h);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return std::move(ctx.out);
}

}  // namespace

void App::CheckFullscreen(bool immediate) {
  if (opts_.harness) return;
  // Always decided (cheap) so diagnostics and the settings window can explain
  // the state even when both pauses are switched off.
  std::vector<MonitorHandle> mons = EnumerateMonitors();
  std::vector<FsMonitor> fm;
  for (const auto& m : mons) fm.push_back(FsMonitor{m.info.rect, m.info.work});
  ShellNotifyState shell = QueryShellState(diagShell_);
  std::vector<HWND> hwnds;
  std::vector<FsWindow> wins = FullscreenCandidates(hwnds, settingsWnd_.Hwnd());
  PauseOptions po;
  po.fullscreen = state_.settings.pauseFullscreen;
  po.maximized = state_.settings.pauseMaximized;
  std::vector<PauseDecision> dec = DecidePause(fm, wins, shell, po);

  std::vector<std::string> raw;
  std::vector<MonitorDiag> diag;
  std::string key;
  for (size_t i = 0; i < mons.size(); ++i) {
    if (dec[i].pause) raw.push_back(mons[i].info.id);
    MonitorDiag d;
    d.id = mons[i].info.id;
    d.rect = mons[i].info.rect;
    d.work = mons[i].info.work;
    d.scale = mons[i].info.scale;
    d.primary = mons[i].info.primary;
    d.decision = dec[i];
    if (dec[i].window >= 0 && static_cast<size_t>(dec[i].window) < hwnds.size()) {
      HWND h = hwnds[static_cast<size_t>(dec[i].window)];
      DWORD pid = 0;
      GetWindowThreadProcessId(h, &pid);
      d.exe = ProcessName(pid);
      d.window = DescribeWindow(h);
    }
    key += d.id + "=" + PauseReasonName(d.decision.reason) + (d.decision.pause ? "+" : "-") + d.exe + "/" +
           d.decision.rule + ";";
    diag.push_back(std::move(d));
  }
  diag_ = std::move(diag);
  HWND fg = GetForegroundWindow();
  diagFg_ = DescribeWindow(fg ? GetAncestor(fg, GA_ROOT) : nullptr);
  bool diagChanged = key != diagLastKey_;
  if (diagChanged) {
    diagLastKey_ = key;
    for (const std::string& line : DiagnosticsLines()) diagHistory_.push_back(LocalTimeText(false) + " " + line);
    while (diagHistory_.size() > kDiagHistory) diagHistory_.pop_front();
  }
  std::sort(raw.begin(), raw.end());

  bool changed;
  // Hiding must feel instant: the moment a monitor becomes paused, apply the
  // new set in this very frame (no debounce, no fade). Only resuming waits for
  // the debounce so alt-tabbing does not make the snow flicker.
  bool newlyPaused = false;
  {
    const std::vector<std::string>& applied = fsDebounce_.applied();
    for (const auto& id : raw)
      if (std::find(applied.begin(), applied.end(), id) == applied.end()) newlyPaused = true;
  }
  if (immediate || newlyPaused) {
    changed = raw != fsDebounce_.applied();
    fsDebounce_.reset(raw);
  } else {
    changed = fsDebounce_.update(Now(), raw);
  }
  double due = fsDebounce_.pendingIn(Now());
  if (due >= 0) SetTimer(hwnd_, T_FS_DEBOUNCE, static_cast<UINT>(due * 1000.0) + 15, nullptr);
  else KillTimer(hwnd_, T_FS_DEBOUNCE);
  if (!changed) {
    if (diagChanged && state_.settings.debugOverlay) PushConfig();  // debug layer shows the decision
    return;
  }
  hiddenMonitors_ = fsDebounce_.applied();
  std::string list;
  for (const auto& d : diag_)
    if (std::find(hiddenMonitors_.begin(), hiddenMonitors_.end(), d.id) != hiddenMonitors_.end())
      list += (list.empty() ? "" : ", ") + d.id + " (" + PauseReasonName(d.decision.reason) +
              (d.exe.empty() ? "" : ", " + d.exe) + ")";
  Log("fullscreen: %zu monitor(s) paused%s%s (shell %s)", hiddenMonitors_.size(), list.empty() ? "" : ": ",
      list.c_str(), diagShell_.c_str());
  PushConfig();
}

std::vector<std::string> App::DiagnosticsLines() const {
  std::vector<std::string> out;
  for (const auto& d : diag_) {
    bool applied = std::find(hiddenMonitors_.begin(), hiddenMonitors_.end(), d.id) != hiddenMonitors_.end();
    out.push_back(Fmt("mon=%s rect=%s work=%s scale=%.2f decision=%s pause=%s applied=%s rule=\"%s\" window=%s | "
                      "fg=%s | shell=%s",
                      d.id.c_str(), RectText(d.rect).c_str(), RectText(d.work).c_str(), static_cast<double>(d.scale),
                      PauseReasonName(d.decision.reason), d.decision.pause ? "yes" : "no",
                      applied ? "hidden" : "shown", d.decision.rule, d.window.empty() ? "none" : d.window.c_str(),
                      diagFg_.c_str(), diagShell_.c_str()));
  }
  return out;
}

std::string App::DiagnosticsReport() const {
  const Settings& s = state_.settings;
  std::string r = "Desktop Snow diagnostics\r\n========================\r\n";
  r += Fmt("Build: %s   Windows build: %lu   Time: %s\r\n", SNOW_BUILD_ID, OsBuild(), LocalTimeText(true).c_str());
  std::string off;
  for (const auto& id : s.disabledMonitors) off += (off.empty() ? "" : ",") + id;
  r += Fmt("Settings: enabled=%d pauseFullscreen=%d pauseMaximized=%d pauseOnBattery=%d batterySaver=%d "
           "snowOnTaskbar=%d debugOverlay=%d disabledMonitors=[%s]\r\n",
           s.enabled, s.pauseFullscreen, s.pauseMaximized, s.pauseOnBattery, s.batterySaver, s.piles.onTaskbar,
           s.debugOverlay, off.c_str());
  r += Fmt("State: systemPaused=%d displayOff=%d pausedForHour=%d onBattery=%d remote=%d decorate=%d\r\n",
           systemPaused_, displayOff_, pauseUntil_ > 0, onBattery_, remote_, decorate_);
  r += "\r\nCurrent pause decision per monitor:\r\n";
  for (const std::string& line : DiagnosticsLines()) r += "  " + line + "\r\n";
  r += "\r\nTop-level windows in z-order (first 20 candidates):\r\n";
  std::vector<HWND> hwnds;
  std::vector<FsWindow> wins = FullscreenCandidates(hwnds, settingsWnd_.Hwnd());
  for (size_t i = 0; i < hwnds.size() && i < 20; ++i) {
    RECT fr{wins[i].frame.left, wins[i].frame.top, wins[i].frame.right, wins[i].frame.bottom};
    r += Fmt("  %2zu %s%s\r\n", i, DescribeWindow(hwnds[i], &fr).c_str(), wins[i].foreground ? " foreground" : "");
  }
  r += "\r\nRecent decision changes (newest last):\r\n";
  for (const std::string& line : diagHistory_) r += "  " + line + "\r\n";
  return r;
}

void App::CopyDiagnostics() {
  std::wstring text = Widen(DiagnosticsReport());
  bool ok = false;
  if (OpenClipboard(hwnd_)) {
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
      if (void* p = GlobalLock(mem)) {
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
      }
      if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
  }
  Log("diagnostics: copied to clipboard (%s)", ok ? "ok" : "FAILED");
  Balloon(ok ? UiTr(UiStr::DiagCopied, lang_) : UiTr(UiStr::DiagSaveFailed, lang_));
}

void App::SaveDiagnostics() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf(name, 64, L"\\diagnostics-%04u%02u%02u-%02u%02u%02u.txt", st.wYear, st.wMonth, st.wDay, st.wHour,
           st.wMinute, st.wSecond);
  std::wstring path = LogDir() + name;
  std::string text = DiagnosticsReport();
  bool ok = WriteTextFile(std::filesystem::path(path), text);
  Log("diagnostics: saved to %s (%s)", Narrow(path).c_str(), ok ? "ok" : "FAILED");
  if (!ok) {
    Balloon(UiTr(UiStr::DiagSaveFailed, lang_));
    return;
  }
  std::wstring args = L"/select,\"" + path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  Balloon(std::string(UiTr(UiStr::DiagSaved, lang_)) + " " + Narrow(path));
}

void App::RegisterHotkeys() {
  UnregisterHotkeys();
  if (opts_.harness) return;
  UINT mods, vk;
  bool failed = false;
  if (ParseHotkey(state_.settings.hotkeyToggle, mods, vk))
    failed |= !RegisterHotKey(hwnd_, HK_TOGGLE, mods | MOD_NOREPEAT, vk);
  if (ParseHotkey(state_.settings.hotkeyDecorate, mods, vk))
    failed |= !RegisterHotKey(hwnd_, HK_DECORATE, mods | MOD_NOREPEAT, vk);
  if (failed) {
    Log("hotkey: registration failed (%lu)", GetLastError());
    if (!hotkeyFailedShown_) Balloon(T(Str::TipHotkeyFailed));
    hotkeyFailedShown_ = true;
  }
}

void App::UnregisterHotkeys() {
  UnregisterHotKey(hwnd_, HK_TOGGLE);
  UnregisterHotKey(hwnd_, HK_DECORATE);
}

// -------------------------------------------------------------------- tray

HICON App::MakeIcon(int size) {
  // Procedural snowflake: six arms with side branches, anti-aliased by distance.
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC sdc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(sdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, sdc);
  if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);
  auto* px = static_cast<uint32_t*>(bits);
  float c = size / 2.f, R = size * 0.46f, w = std::max(1.f, size / 16.f);
  struct Seg { float x0, y0, x1, y1; };
  std::vector<Seg> segs;
  for (int k = 0; k < 6; ++k) {
    float a = k * kPi / 3.f;
    float dx = std::cos(a), dy = std::sin(a);
    segs.push_back({c, c, c + dx * R, c + dy * R});
    for (float t : {0.5f, 0.75f}) {
      float bx = c + dx * R * t, by = c + dy * R * t, bl = R * 0.28f * (1.1f - t * 0.4f);
      for (float s : {-1.f, 1.f}) {
        float ba = a + s * kPi / 4.f;
        segs.push_back({bx, by, bx + std::cos(ba) * bl, by + std::sin(ba) * bl});
      }
    }
  }
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      float px0 = x + 0.5f, py0 = y + 0.5f, dmin = 1e9f;
      for (const auto& s : segs) {
        float vx = s.x1 - s.x0, vy = s.y1 - s.y0;
        float t = clampf(((px0 - s.x0) * vx + (py0 - s.y0) * vy) / (vx * vx + vy * vy), 0.f, 1.f);
        float ex = s.x0 + vx * t - px0, ey = s.y0 + vy * t - py0;
        dmin = std::min(dmin, std::sqrt(ex * ex + ey * ey));
      }
      float a = clampf(w * 0.5f + 0.5f - dmin, 0.f, 1.f);
      // Light blue flake with a darker edge so it reads on light and dark taskbars.
      float core = clampf(w * 0.5f - dmin, 0.f, 1.f);
      float r = 0.35f + 0.65f * core, g = 0.55f + 0.45f * core, b = 0.85f + 0.15f * core;
      uint32_t A = static_cast<uint32_t>(a * 255.f);
      uint32_t Rr = static_cast<uint32_t>(r * a * 255.f), G = static_cast<uint32_t>(g * a * 255.f),
               B = static_cast<uint32_t>(b * a * 255.f);
      px[y * size + x] = (A << 24) | (Rr << 16) | (G << 8) | B;
    }
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void App::AddTrayIcon() {
  if (opts_.harness) return;
  if (!icon_) icon_ = MakeIcon(GetSystemMetrics(SM_CXSMICON));
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = 1;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = icon_;
  std::wstring tip = W(T(Str::AppName)) + L" \x2014 " + W(T(Str::Tagline));
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_DELETE, &nid);
  trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
  Log("tray: icon %s", trayAdded_ ? "added" : "FAILED");
}

void App::RemoveTrayIcon() {
  if (!trayAdded_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  trayAdded_ = false;
}

void App::Balloon(const std::string& text) {
  if (!trayAdded_) {
    Log("note: %s", text.c_str());
    return;
  }
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = 1;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
  wcsncpy_s(nid.szInfoTitle, W(T(Str::AppName)).c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, Widen(text).c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowMenu(POINT pt) {
  const Settings& s = state_.settings;
  auto item = [](HMENU m, UINT id, const std::wstring& text, bool checked, bool isDefault = false) {
    MENUITEMINFOW mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mi.fType = MFT_STRING;
    mi.fState = (checked ? MFS_CHECKED : 0) | (isDefault ? MFS_DEFAULT : 0);
    mi.wID = id;
    mi.dwTypeData = const_cast<wchar_t*>(text.c_str());
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
  };
  HMENU menu = CreatePopupMenu();
  item(menu, ID_ENABLED, W(T(Str::MenuEnabled)) + L"\t" + Widen(s.hotkeyToggle), s.enabled);
  item(menu, ID_SETTINGS, W(UiStr::TraySettings, lang_), false, true);
  bool paused = pauseUntil_ > 0;
  item(menu, ID_PAUSE_HOUR, W(paused ? UiStr::TrayResume : UiStr::TrayPauseHour, lang_), false);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  item(menu, ID_QUIT, W(T(Str::MenuQuit)), false);

  // Required for the menu to close when the user clicks elsewhere (documented quirk).
  SetForegroundWindow(hwnd_);
  UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
  flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  UINT cmd = TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd) OnCommand(cmd);
}

void App::OnCommand(UINT id) {
  switch (id) {
    case ID_ENABLED: {
      Settings s = state_.settings;
      s.enabled = !s.enabled;
      ApplySettings(s);
      break;
    }
    case ID_SETTINGS: settingsWnd_.Show(); break;
    case ID_PAUSE_HOUR: SetPauseHour(pauseUntil_ <= 0); break;
    case ID_QUIT: DestroyWindow(hwnd_); break;
  }
}

void App::SetPauseHour(bool on) {
  pauseUntil_ = on ? Now() + 3600.0 : 0;
  if (on) SetTimer(hwnd_, T_PAUSE_HOUR, 3600 * 1000, nullptr);
  else KillTimer(hwnd_, T_PAUSE_HOUR);
  Log("pause for 1 hour: %s", on ? "on" : "off");
  PushConfig();
  if (settingsWnd_.IsOpen()) settingsWnd_.Refresh();
}

// --------------------------------------------------------- settings window

void App::InitSettingsWindow() {
  SettingsHost host;
  host.get = [this] { return state_.settings; };
  host.set = [this](const Settings& s) { ApplySettings(s); };
  host.action = [this](UiAction a) { SettingsAction(a); };
  host.monitors = [] {
    std::vector<UiMonitor> out;
    for (const auto& m : EnumerateMonitors()) {
      UiMonitor u;
      u.id = m.info.id;
      u.width = m.info.rect.width();
      u.height = m.info.rect.height();
      u.scale = m.info.scale;
      u.primary = m.info.primary;
      out.push_back(u);
    }
    return out;
  };
  host.status = [this] { return Status(); };
  host.setDiagnosticsLogging = [this](bool on) {
    diagLogging_ = on;
    Log("diagnostics logging %s (settings window)", on ? "on" : "off");
  };
  host.hotkeyCapture = [this](bool capturing) {
    if (capturing) UnregisterHotkeys();
    else RegisterHotkeys();
  };
  host.lang = [this] { return lang_; };
  settingsWnd_.Init(GetModuleHandleW(nullptr), std::move(host));
}

void App::ApplySettings(Settings s) {
  const Settings old = state_.settings;
  Sanitize(s);
  if (s.launchAtStartup != old.launchAtStartup && !SetAutostart(s.launchAtStartup, L"")) {
    Log("autostart: could not %s", s.launchAtStartup ? "enable" : "disable");
    s.launchAtStartup = old.launchAtStartup;
  }
  state_.settings = s;
  RefreshSystemState();
  if (s.language != old.language) AddTrayIcon();  // tooltip language
  if (s.hotkeyToggle != old.hotkeyToggle || s.hotkeyDecorate != old.hotkeyDecorate) RegisterHotkeys();
  // The user asked: no debounce.
  if (s.pauseFullscreen != old.pauseFullscreen || s.pauseMaximized != old.pauseMaximized) CheckFullscreen(true);
  PushConfig();
  ScheduleSave();
  if (settingsWnd_.IsOpen()) settingsWnd_.Refresh();
}

void App::SettingsAction(UiAction a) {
  switch (a) {
    case UiAction::Decorate:
      settingsWnd_.Hide();
      EnterDecorate();
      break;
    case UiAction::ClearSnow: engine_.Command(EngineCommand::ClearSnow); break;
    case UiAction::ClearDecorations:
      // Everything: trees with their ornaments, snowmen and free-standing
      // decorations, plus every rolled snowball.
      ClearAllDecorations(state_.decor);
      engine_.Command(EngineCommand::ClearSnowballs);
      PushDecor();
      ScheduleSave();
      break;
    case UiAction::ClearSnowmen:
      // Only snowmen (placed and rolled), trees and other decorations stay.
      ClearSnowmenOnly(state_.decor);
      engine_.Command(EngineCommand::ClearSnowballs);
      PushDecor();
      ScheduleSave();
      break;
    case UiAction::Santa: engine_.Command(EngineCommand::Santa); break;
    case UiAction::Train: engine_.Command(EngineCommand::Train); break;
    case UiAction::Yeti: engine_.Command(EngineCommand::Yeti); break;
    case UiAction::CopyDiagnostics: CopyDiagnostics(); break;
    case UiAction::SaveDiagnostics: SaveDiagnostics(); break;
    case UiAction::OpenSettingsFolder:
      ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case UiAction::OpenLogFolder: ShellExecuteW(nullptr, L"open", LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case UiAction::ResetDefaults: {
      Settings s = state_.settings;
      ResetToDefaults(s);
      Log("settings: reset to defaults");
      ApplySettings(s);
      break;
    }
  }
}

UiStatus App::Status() {
  UiStatus st;
  const Settings& s = state_.settings;
  for (size_t i = 0; i < diag_.size(); ++i) {
    const MonitorDiag& d = diag_[i];
    std::string line = Fmt("%s %zu%s: ", UiTr(UiStr::StatusMonitor, lang_), i + 1, d.primary ? " *" : "");
    bool off = std::find(s.disabledMonitors.begin(), s.disabledMonitors.end(), d.id) != s.disabledMonitors.end();
    bool hidden = std::find(hiddenMonitors_.begin(), hiddenMonitors_.end(), d.id) != hiddenMonitors_.end();
    if (!s.enabled) line += UiTr(UiStr::StatusSnowOff, lang_);
    else if (pauseUntil_ > 0) line += UiTr(UiStr::StatusPausedHour, lang_);
    else if (systemPaused_ || displayOff_) line += UiTr(UiStr::StatusPausedSystem, lang_);
    else if (s.pauseOnBattery && onBattery_) line += UiTr(UiStr::StatusPausedBattery, lang_);
    else if (off) line += UiTr(UiStr::StatusMonitorOff, lang_);
    else if (hidden && d.decision.reason == PauseReason::Maximized) line += UiTr(UiStr::StatusPausedMaximized, lang_);
    else if (hidden) line += UiTr(UiStr::StatusPausedFullscreen, lang_);
    else line += UiTr(UiStr::StatusSnowing, lang_);
    if (!d.exe.empty()) line += " (" + d.exe + ")";
    st.pauseLines.push_back(Widen(line));
  }
  st.trees = static_cast<int>(state_.decor.trees.size());
  st.maxTrees = kMaxTrees;
  st.snowmen = static_cast<int>(state_.decor.snowmen.size());
  st.maxSnowmen = kMaxSnowmen;
  st.diagnosticsLogging = DiagnosticsLogging();
  st.pausedForHour = pauseUntil_ > 0;
  st.fps = engine_.Fps();
  st.build = SNOW_BUILD_ID;
  return st;
}

// ---------------------------------------------------------- decorate mode

void App::EnterDecorate() {
  if (decorate_ || opts_.harness) return;
  decorate_ = true;
  tool_ = Tool::Tree;
  colorPopup_ = ColorPopup();
  RestoreHoverPreview();
  std::vector<HWND> overlays = engine_.OverlayWindows();
  std::vector<MonitorHandle> mons = EnumerateMonitors();
  std::wstring cls = L"DesktopSnowDecorate." + opts_.instance;
  for (size_t i = 0; i < mons.size(); ++i) {
    const RectI& r = mons[i].info.rect;
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.c_str(), L"Desktop Snow - decorate",
                             WS_POPUP, r.left, r.top, r.width(), r.height(), nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h) continue;
    // A faint dim marks the mode and makes the whole screen clickable.
    SetLayeredWindowAttributes(h, 0, 48, LWA_ALPHA);
    HWND above = (i < overlays.size() && IsWindowVisible(overlays[i])) ? overlays[i] : HWND_TOPMOST;
    SetWindowPos(h, above, r.left, r.top, r.width(), r.height(), SWP_SHOWWINDOW);
    catchers_.push_back(h);
  }
  if (!catchers_.empty()) SetForegroundWindow(catchers_.front());
  PushConfig();
}

void App::ExitDecorate() {
  if (!decorate_) return;
  RestoreHoverPreview();
  colorPopup_ = ColorPopup();
  decorate_ = false;
  dragging_ = pressed_ = false;
  dragKind_ = DragKind::None;
  if (GetCapture()) ReleaseCapture();
  for (HWND h : catchers_) DestroyWindow(h);
  catchers_.clear();
  hoverId_ = 0;
  PushConfig();
  SaveNow();
}

int App::HitItem(POINT pt, std::vector<DecorLayoutItem>& layout, bool& isTree) {
  engine_.GetDecorLayout(layout);
  float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
  // Snowmen and free decorations sit in front of the desktop trees: test them first.
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < layout.size(); ++i) {
      const auto& it = layout[i];
      if (!it.visible) continue;
      const bool isTreeItem = it.cls == 0;
      if (isTreeItem != (pass == 1)) continue;
      if (x < it.left || x >= it.right || y < it.top || y >= it.bottom) continue;
      if (isTreeItem) {
        float u, v;
        TreeScreenToLocal(Vec2(it.baseX, it.baseY), it.height, Vec2(x, y), u, v);
        bool trunk = v >= 0.86f && std::fabs(u - 0.5f) < 0.1f;
        bool star = v < 0.12f && std::fabs(u - 0.5f) < 0.12f;
        if (!TreeFoliageContains(u, v) && !trunk && !star) continue;
      }
      isTree = isTreeItem;
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool App::AnchorAt(POINT pt, WindowAnchor& anchor, float& relX, bool* overWindow) {
  if (overWindow) *overWindow = false;
  anchor = WindowAnchor();
  auto snap = tracker_.Snapshot();
  if (snap) {
    for (const auto& w : snap->windows) {
      const RECT& r = w.frame;
      int above = static_cast<int>(60 * w.scale);
      if (pt.x < r.left || pt.x >= r.right || pt.y < r.top - above || pt.y >= r.bottom) continue;
      if (w.isTaskbar && w.holdsSnow) break;
      if (!w.holdsSnow) {
        if (pt.y >= r.top) break;  // inside a window that cannot hold snow (e.g. maximised)
        continue;
      }
      anchor.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(w.hwnd));
      anchor.process = w.process;
      anchor.className = w.className;
      anchor.title = w.title;
      relX = clampf(static_cast<float>(pt.x - r.left) / std::max(1L, r.right - r.left), 0.f, 1.f);
      if (overWindow) *overWindow = true;
      return true;
    }
  }
  // Safe fallback: the taskbar (or the bottom of the screen).
  anchor.taskbar = true;
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  relX = clampf(static_cast<float>(pt.x - mi.rcMonitor.left) / std::max(1L, mi.rcMonitor.right - mi.rcMonitor.left),
                0.f, 1.f);
  return true;
}

void App::OpenColorPicker(int target, uint32_t snowmanId, uint32_t freeId, POINT at, int selected) {
  colorPopup_.open = true;
  colorPopup_.target = target;
  colorPopup_.snowmanId = snowmanId;
  colorPopup_.freeId = freeId;
  colorPopup_.at = at;
  colorPopup_.selected = selected;
  PushConfig();
}

void App::ApplyPickerColor(int index) {
  DecorState& d = state_.decor;
  const int c = ((index % kDecorColorCount) + kDecorColorCount) % kDecorColorCount;
  if (colorPopup_.target == 1) {
    for (auto& s : d.snowmen)
      if (s.id == colorPopup_.snowmanId) {
        s.hatColor = c;
        previewSaved_ = s.hat;
        previewActive_ = false;
      }
  } else if (colorPopup_.target == 2) {
    for (auto& s : d.snowmen)
      if (s.id == colorPopup_.snowmanId) {
        s.scarf = c;
        previewSaved_ = s.scarf;
        previewActive_ = false;
      }
  } else if (colorPopup_.target == 3) {
    for (auto& f : d.free)
      if (f.id == colorPopup_.freeId) f.color = c;
  }
  colorPopup_.selected = c;
  // The chosen colour also sticks for the next hats/scarves the user places.
  state_.settings.decorColor = c;
  PushConfig();
  PushDecor();
  ScheduleSave();
}

void App::RestoreHoverPreview() {
  if (!previewActive_) return;
  for (auto& s : state_.decor.snowmen) {
    if (s.id != previewSnowman_) continue;
    if (previewTool_ == 1) s.hat = previewSaved_;
    else if (previewTool_ == 2) s.scarf = previewSaved_;
  }
  previewActive_ = false;
  previewSnowman_ = 0;
}

void App::UpdateHoverPreview(uint32_t hover) {
  RestoreHoverPreview();
  if (!decorate_ || dragging_ || hover == 0) return;
  if (tool_ != Tool::Hat && tool_ != Tool::Scarf) return;
  for (auto& s : state_.decor.snowmen) {
    if (s.id != hover) continue;
    if (tool_ == Tool::Hat) {
      previewTool_ = 1;
      previewSaved_ = s.hat;
      s.hat = s.hat == 0 ? 1 : (s.hat % 3) + 1;
    } else {
      previewTool_ = 2;
      previewSaved_ = s.scarf;
      s.scarf = (s.scarf + 1) % kDecorColorCount;
    }
    previewActive_ = true;
    previewSnowman_ = s.id;
    PushDecor();
    break;
  }
}

void App::DecorClick(POINT pt) {
  DecorState& d = state_.decor;
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  const RectI work(mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
  const float dpi = MonitorScale(mon);
  const std::string monId = Narrow(mi.szDevice);

  std::vector<DecorLayoutItem> layout;
  bool isTree = false;
  int hit = HitItem(pt, layout, isTree);
  const int cls = hit >= 0 ? layout[static_cast<size_t>(hit)].cls : -1;
  const uint32_t hitId = hit >= 0 ? layout[static_cast<size_t>(hit)].id : 0;
  auto findTree = [&](uint32_t id) -> TreeDecor* {
    for (auto& t : d.trees)
      if (t.id == id) return &t;
    return nullptr;
  };
  auto findSnowman = [&](uint32_t id) -> SnowmanDecor* {
    for (auto& s : d.snowmen)
      if (s.id == id) return &s;
    return nullptr;
  };
  auto findFree = [&](uint32_t id) -> FreeDecor* {
    for (auto& f : d.free)
      if (f.id == id) return &f;
    return nullptr;
  };
  TreeDecor* tree = cls == 0 ? findTree(hitId) : nullptr;
  SnowmanDecor* man = cls == 1 ? findSnowman(hitId) : nullptr;
  FreeDecor* freeItem = cls == 2 ? findFree(hitId) : nullptr;
  float u = 0, v = 0;
  if (tree) {
    const auto& it = layout[static_cast<size_t>(hit)];
    TreeScreenToLocal(Vec2(it.baseX, it.baseY), it.height, Vec2(static_cast<float>(pt.x), static_cast<float>(pt.y)), u, v);
  }
  auto spawnFree = [&](FreeKind kind, int color, int variant) -> FreeDecor* {
    if (static_cast<int>(d.free.size()) >= kMaxFreeDecor) return nullptr;
    FreeDecor f;
    f.id = d.nextId++;
    f.kind = kind;
    f.monitor = monId;
    f.scale = 1.f;
    f.color = color;
    f.variant = variant;
    f.nx = clampf(static_cast<float>(pt.x - work.left) / static_cast<float>(std::max(1, work.width())), 0.02f, 0.98f);
    f.ny = clampf(static_cast<float>(pt.y - work.top) / static_cast<float>(std::max(1, work.height())), 0.02f, 0.98f);
    d.free.push_back(f);
    return &d.free.back();
  };
  bool changed = false;
  switch (tool_) {
    case Tool::Tree:
      if (!tree && !man && !freeItem) {
        if (static_cast<int>(d.trees.size()) >= kMaxTrees) {
          Balloon(T(Str::TipTreeLimit));
          break;
        }
        TreeDecor t;
        t.id = d.nextId++;
        t.monitor = monId;
        const float h = TreeHeightPx(1.f, dpi);
        const float baseX = clampf(static_cast<float>(pt.x), static_cast<float>(work.left) + 20.f * dpi,
                                   static_cast<float>(work.right) - 20.f * dpi);
        const float baseY = clampf(static_cast<float>(pt.y) + h * 0.5f, static_cast<float>(work.top) + h * 0.2f,
                                   static_cast<float>(work.bottom));
        t.nx = clampf((baseX - static_cast<float>(work.left)) / static_cast<float>(std::max(1, work.width())), 0.f, 1.f);
        t.ny = clampf((baseY - static_cast<float>(work.top)) / static_cast<float>(std::max(1, work.height())), 0.f, 1.f);
        d.trees.push_back(t);
        changed = true;
      }
      break;
    case Tool::Bauble:
    case Tool::CandyCane:
      if (tree && TreeFoliageContains(u, v) && static_cast<int>(tree->ornaments.size()) < kMaxOrnamentsPerTree) {
        Ornament o;
        o.kind = tool_ == Tool::Bauble ? OrnamentKind::Bauble : OrnamentKind::CandyCane;
        o.u = u;
        o.v = v;
        o.color = colorCycle_++ % kDecorColorCount;
        tree->ornaments.push_back(o);
        changed = true;
      } else if (!man && !tree) {
        if (spawnFree(tool_ == Tool::Bauble ? FreeKind::Bauble : FreeKind::CandyCane, colorCycle_++ % kDecorColorCount, 1))
          changed = true;
      }
      break;
    case Tool::Lights:
      if (tree) {
        tree->lights = !tree->lights;
        changed = true;
      }
      break;
    case Tool::Star:
      if (tree) {
        tree->star = !tree->star;
        changed = true;
      } else if (!man) {
        if (spawnFree(FreeKind::Star, 2, 1)) changed = true;
      }
      break;
    case Tool::Snowman:
      if (!man) {
        if (static_cast<int>(d.snowmen.size()) >= kMaxSnowmen) break;
        SnowmanDecor s;
        s.id = d.nextId++;
        const float h = SnowmanHeightPx(1.f, dpi);
        s.free = true;
        s.monitor = monId;
        const float baseX = clampf(static_cast<float>(pt.x), static_cast<float>(work.left) + 0.3f * h,
                                   static_cast<float>(work.right) - 0.3f * h);
        const float baseY = clampf(static_cast<float>(pt.y), static_cast<float>(work.top) + h,
                                   static_cast<float>(work.bottom));
        s.nx = clampf((baseX - static_cast<float>(work.left)) / static_cast<float>(std::max(1, work.width())), 0.f, 1.f);
        s.ny = clampf((baseY - static_cast<float>(work.top)) / static_cast<float>(std::max(1, work.height())), 0.f, 1.f);
        s.hat = 1;
        s.hatColor = state_.settings.decorColor >= 0 ? state_.settings.decorColor % kDecorColorCount
                                                     : colorCycle_ % kDecorColorCount;
        s.scarf = state_.settings.decorColor >= 0 ? state_.settings.decorColor % kDecorColorCount
                                                  : colorCycle_++ % kDecorColorCount;
        d.snowmen.push_back(s);
        changed = true;
      }
      break;
    case Tool::Hat:
      if (man) {
        if (previewActive_ && previewSnowman_ == man->id && previewTool_ == 1) {
          previewActive_ = false;  // commit the previewed hat
          previewSaved_ = man->hat;
        } else {
          man->hat = man->hat == 0 ? 1 : (man->hat % 3) + 1;
        }
        changed = true;
        OpenColorPicker(1, man->id, 0, pt, man->hatColor);
      } else if (!tree) {
        const int col = state_.settings.decorColor >= 0 ? state_.settings.decorColor % kDecorColorCount
                                                        : colorCycle_++ % kDecorColorCount;
        FreeDecor* f = spawnFree(FreeKind::Hat, col, (colorCycle_ % 3) + 1);
        if (f) {
          changed = true;
          OpenColorPicker(3, 0, f->id, pt, f->color);
        }
      }
      break;
    case Tool::Scarf:
      if (man) {
        if (previewActive_ && previewSnowman_ == man->id && previewTool_ == 2) {
          previewActive_ = false;
          previewSaved_ = man->scarf;
        }
        OpenColorPicker(2, man->id, 0, pt, man->scarf);
      } else if (!tree) {
        const int col = state_.settings.decorColor >= 0 ? state_.settings.decorColor % kDecorColorCount
                                                        : colorCycle_++ % kDecorColorCount;
        FreeDecor* f = spawnFree(FreeKind::Scarf, col, 1);
        if (f) {
          changed = true;
          OpenColorPicker(3, 0, f->id, pt, f->color);
        }
      }
      break;
    case Tool::Erase:
      if (freeItem) {
        const uint32_t id = freeItem->id;
        d.free.erase(std::remove_if(d.free.begin(), d.free.end(), [&](const FreeDecor& f) { return f.id == id; }),
                     d.free.end());
        changed = true;
      } else if (tree) {
        int best = -1;
        float bestD = 0.06f;
        for (size_t i = 0; i < tree->ornaments.size(); ++i) {
          float du = tree->ornaments[i].u - u, dv = (tree->ornaments[i].v - v) / 0.72f;
          float dd = std::sqrt(du * du + dv * dv);
          if (dd < bestD) {
            bestD = dd;
            best = static_cast<int>(i);
          }
        }
        if (best >= 0) tree->ornaments.erase(tree->ornaments.begin() + best);
        else if (tree->star && v < 0.12f) tree->star = false;
        else {
          uint32_t id = tree->id;
          d.trees.erase(std::remove_if(d.trees.begin(), d.trees.end(), [&](const TreeDecor& t) { return t.id == id; }),
                        d.trees.end());
        }
        changed = true;
      } else if (man) {
        uint32_t id = man->id;
        d.snowmen.erase(
            std::remove_if(d.snowmen.begin(), d.snowmen.end(), [&](const SnowmanDecor& s) { return s.id == id; }),
            d.snowmen.end());
        changed = true;
      }
      break;
    case Tool::Done:
    case Tool::Count:
      break;
  }
  if (changed) {
    PushDecor();
    ScheduleSave();
  }
}

LRESULT App::HandleCatcher(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(h, &rc);
      FillRect(reinterpret_cast<HDC>(w), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      return 1;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, hoverId_ ? IDC_HAND : IDC_CROSS));
      return TRUE;
    case WM_KEYDOWN:
      if (w == VK_ESCAPE) {
        if (colorPopup_.open) {
          colorPopup_.open = false;
          PushConfig();
        } else {
          ExitDecorate();
        }
      }
      return 0;
    case WM_RBUTTONUP:
      if (colorPopup_.open) {
        colorPopup_.open = false;
        PushConfig();
      } else {
        ExitDecorate();
      }
      return 0;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ClientToScreen(h, &pt);
      HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(mon, &mi);
      const RectI work(mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
      const float dpi = MonitorScale(mon);
      auto overUi = [&](const PaletteLayout& pl, const RectI& hint) {
        if (PaletteUiHit(pl, pt.x, pt.y, 8)) return true;
        if (pt.x >= hint.left - 8 && pt.x < hint.right + 8 && pt.y >= hint.top - 8 && pt.y < hint.bottom + 8) return true;
        if (colorPopup_.open) {
          ColorPickerLayout cp = LayoutColorPicker(colorPopup_.at.x, colorPopup_.at.y, work, dpi);
          if (ColorPickerUiHit(cp, pt.x, pt.y, 8)) return true;
        }
        return false;
      };
      if (m == WM_MOUSEMOVE && paletteDrag_) {
        const int left = static_cast<int>(pt.x - paletteGrabX_);
        const int top = static_cast<int>(pt.y - paletteGrabY_);
        RectI moved(left, top, left + paletteBar_.width(), top + paletteBar_.height());
        RectI clamped = ClampPaletteRect(moved, work);
        if (clamped.left != state_.settings.paletteLeft || clamped.top != state_.settings.paletteTop) {
          state_.settings.paletteLeft = clamped.left;
          state_.settings.paletteTop = clamped.top;
          paletteBar_ = clamped;
          PushConfig();
        }
        return 0;
      }
      if (m == WM_LBUTTONDOWN) {
        // Colour picker first: it floats above everything.
        if (colorPopup_.open) {
          ColorPickerLayout cp = LayoutColorPicker(colorPopup_.at.x, colorPopup_.at.y, work, dpi);
          const int sw = ColorPickerHit(cp, pt.x, pt.y, 2);
          if (sw >= 0) {
            // Keep the popup open and still bound to the same hat/scarf: every
            // further swatch click recolours that item instead of falling
            // through to the canvas and spawning a duplicate.
            ApplyPickerColor(sw);
            swallowUp_ = true;
            return 0;
          }
          if (ColorPickerUiHit(cp, pt.x, pt.y, 8)) {
            swallowUp_ = true;  // inside the popup frame: keep it open if not on a swatch
            return 0;
          }
          colorPopup_.open = false;  // click outside closes it; the click continues
          PushConfig();
        }
        // The toolbar is a UI element: a press on it never reaches the canvas.
        PaletteLayout pl = LayoutPalette(work, dpi, state_.settings.paletteLeft, state_.settings.paletteTop);
        const RectI hint = HintRect(work, pl.bar, dpi);
        if (PaletteUiHit(pl, pt.x, pt.y, 6)) {
          Tool t;
          if (PaletteHit(pl, pt.x, pt.y, t)) {
            if (t == Tool::Done) {
              ExitDecorate();
            } else {
              RestoreHoverPreview();
              tool_ = t;
              hoverId_ = 0;
              PushDecor();
              PushConfig();
            }
          } else {
            // Free part of the bar (frame / gaps): drag the whole toolbar.
            paletteDrag_ = true;
            paletteGrabX_ = static_cast<float>(pt.x - pl.bar.left);
            paletteGrabY_ = static_cast<float>(pt.y - pl.bar.top);
            paletteBar_ = pl.bar;
            SetCapture(h);
          }
          swallowUp_ = true;
          return 0;
        }
        if (overUi(pl, hint) && !PaletteUiHit(pl, pt.x, pt.y, 8)) {
          swallowUp_ = true;  // the hint plate swallows clicks too
          return 0;
        }
        // Hat/scarf tool: a click on an existing snowman (its hat sits a little
        // above the body box) or on an already placed free hat/scarf selects it
        // and opens the colour popup again - never spawns a duplicate.
        if (tool_ == Tool::Hat || tool_ == Tool::Scarf) {
          const FreeKind wantKind = tool_ == Tool::Hat ? FreeKind::Hat : FreeKind::Scarf;
          std::vector<DecorLayoutItem> lay;
          engine_.GetDecorLayout(lay);
          for (const auto& it : lay) {
            if (!it.visible || it.cls != 1) continue;
            if (pt.x < it.left - 8 || pt.x > it.right + 8) continue;
            if (pt.y < it.top - 0.34f * it.height || pt.y > it.bottom + 4) continue;
            for (auto& s : state_.decor.snowmen) {
              if (s.id != it.id) continue;
              hoverId_ = s.id;
              OpenColorPicker(tool_ == Tool::Hat ? 1 : 2, s.id, 0, pt,
                              tool_ == Tool::Hat ? s.hatColor : s.scarf);
              PushDecor();
              PushConfig();
              swallowUp_ = true;
              return 0;
            }
          }
          for (const auto& it : lay) {
            if (!it.visible || it.cls != 2) continue;
            if (pt.x < it.left - 8 || pt.x > it.right + 8 || pt.y < it.top - 8 || pt.y > it.bottom + 8) continue;
            for (auto& f : state_.decor.free) {
              if (f.id != it.id || f.kind != wantKind) continue;
              OpenColorPicker(3, 0, f.id, pt, f.color);
              swallowUp_ = true;
              return 0;
            }
          }
        }
        std::vector<DecorLayoutItem> layout;
        bool isTree = false;
        int hit = HitItem(pt, layout, isTree);
        pressed_ = true;
        dragging_ = false;
        pressPt_ = pt;
        dragId_ = hit >= 0 ? layout[static_cast<size_t>(hit)].id : 0;
        if (hit >= 0) {
          const DecorLayoutItem& it = layout[static_cast<size_t>(hit)];
          dragKind_ = it.cls == 0 ? DragKind::Tree : (it.cls == 2 ? DragKind::Free : DragKind::Snowman);
          dragOffX_ = static_cast<float>(pt.x) - it.baseX;
          dragOffY_ = static_cast<float>(pt.y) - it.baseY;
        } else {
          dragKind_ = DragKind::None;
        }
        SetCapture(h);
        return 0;
      }
      if (m == WM_MOUSEMOVE) {
        PaletteLayout pl = LayoutPalette(work, dpi, state_.settings.paletteLeft, state_.settings.paletteTop);
        const RectI hint = HintRect(work, pl.bar, dpi);
        if (overUi(pl, hint)) {
          if (hoverId_ != 0 || previewActive_) {
            RestoreHoverPreview();
            hoverId_ = 0;
            PushDecor();
            PushConfig();
          }
          return 0;  // no hover ghost / no dragging under the UI
        }
        if (pressed_ && dragId_ && !dragging_ && (std::abs(pt.x - pressPt_.x) > 4 || std::abs(pt.y - pressPt_.y) > 4)) {
          dragging_ = true;
          RestoreHoverPreview();
        }
        if (dragging_) {
          DecorState& d = state_.decor;
          MONITORINFOEXW mi2{};
          mi2.cbSize = sizeof(mi2);
          GetMonitorInfoW(mon, &mi2);
          if (dragKind_ == DragKind::Tree) {
            for (auto& t : d.trees) {
              if (t.id != dragId_) continue;
              float hgt = TreeHeightPx(t.scale, dpi);
              float bx = clampf(static_cast<float>(pt.x) - dragOffX_, static_cast<float>(work.left) + 20.f * dpi,
                                static_cast<float>(work.right) - 20.f * dpi);
              float by = clampf(static_cast<float>(pt.y) - dragOffY_, static_cast<float>(work.top) + hgt * 0.15f,
                                static_cast<float>(work.bottom));
              t.monitor = Narrow(mi2.szDevice);
              t.nx = clampf((bx - static_cast<float>(work.left)) / static_cast<float>(std::max(1, work.width())), 0.f, 1.f);
              t.ny = clampf((by - static_cast<float>(work.top)) / static_cast<float>(std::max(1, work.height())), 0.f, 1.f);
            }
          } else if (dragKind_ == DragKind::Snowman) {
            for (auto& s : d.snowmen) {
              if (s.id != dragId_) continue;
              float hh = SnowmanHeightPx(s.scale, dpi);
              float bx = clampf(static_cast<float>(pt.x) - dragOffX_, static_cast<float>(work.left) + 0.3f * hh,
                                static_cast<float>(work.right) - 0.3f * hh);
              float by = clampf(static_cast<float>(pt.y) - dragOffY_, static_cast<float>(work.top) + hh,
                                static_cast<float>(work.bottom));
              s.free = true;
              s.monitor = Narrow(mi2.szDevice);
              s.nx = clampf((bx - static_cast<float>(work.left)) / static_cast<float>(std::max(1, work.width())), 0.f, 1.f);
              s.ny = clampf((by - static_cast<float>(work.top)) / static_cast<float>(std::max(1, work.height())), 0.f, 1.f);
            }
          } else if (dragKind_ == DragKind::Free) {
            for (auto& fr : d.free) {
              if (fr.id != dragId_) continue;
              float bx = clampf(static_cast<float>(pt.x) - dragOffX_, static_cast<float>(work.left) + 10.f * dpi,
                                static_cast<float>(work.right) - 10.f * dpi);
              float by = clampf(static_cast<float>(pt.y) - dragOffY_, static_cast<float>(work.top) + 10.f * dpi,
                                static_cast<float>(work.bottom) - 10.f * dpi);
              fr.monitor = Narrow(mi2.szDevice);
              fr.nx = clampf((bx - static_cast<float>(work.left)) / static_cast<float>(std::max(1, work.width())), 0.01f, 0.99f);
              fr.ny = clampf((by - static_cast<float>(work.top)) / static_cast<float>(std::max(1, work.height())), 0.01f, 0.99f);
            }
          }
          PushDecor();
          return 0;
        }
        std::vector<DecorLayoutItem> layout;
        bool isTree = false;
        int hit = HitItem(pt, layout, isTree);
        uint32_t hov = hit >= 0 ? layout[static_cast<size_t>(hit)].id : 0;
        if (hov != hoverId_) {
          hoverId_ = hov;
          UpdateHoverPreview(hov);
          PushConfig();
        }
        return 0;
      }
      // WM_LBUTTONUP
      ReleaseCapture();
      if (paletteDrag_) {
        paletteDrag_ = false;
        pressed_ = dragging_ = false;
        dragKind_ = DragKind::None;
        ScheduleSave();
        return 0;
      }
      if (swallowUp_) {
        swallowUp_ = false;
        pressed_ = dragging_ = false;
        dragKind_ = DragKind::None;
        return 0;
      }
      bool wasDrag = dragging_;
      pressed_ = dragging_ = false;
      dragKind_ = DragKind::None;
      if (wasDrag) ScheduleSave();
      else DecorClick(pt);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};  // screen coordinates for the wheel
      std::vector<DecorLayoutItem> layout;
      bool isTree = false;
      int hit = HitItem(pt, layout, isTree);
      if (hit >= 0) {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / static_cast<float>(WHEEL_DELTA);
        const float k = std::pow(1.1f, steps);
        const uint32_t id = layout[static_cast<size_t>(hit)].id;
        const int cls = layout[static_cast<size_t>(hit)].cls;
        DecorState& d = state_.decor;
        bool done = false;
        if (cls == 0) {
          for (auto& t : d.trees)
            if (t.id == id) {
              t.scale = clampf(t.scale * k, 0.4f, 2.5f);
              done = true;
            }
        } else if (cls == 1) {
          for (auto& s : d.snowmen)
            if (s.id == id) {
              s.scale = clampf(s.scale * k, 0.4f, 2.5f);
              done = true;
            }
        } else {
          for (auto& f : d.free)
            if (f.id == id) {
              f.scale = clampf(f.scale * k, 0.4f, 2.5f);
              done = true;
            }
        }
        if (done) {
          PushDecor();
          ScheduleSave();
        }
      }
      return 0;
    }
    case WM_CLOSE:
      ExitDecorate();
      return 0;
    default:
      return DefWindowProcW(h, m, w, l);
  }
}

// ------------------------------------------------------------ window procs

LRESULT CALLBACK App::ControlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (g_app) return g_app->Handle(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::CatcherProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (g_app) return g_app->HandleCatcher(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}

LRESULT App::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (taskbarCreatedMsg_ && m == taskbarCreatedMsg_) {
    // Explorer restarted: re-add the tray icon, the taskbar window changed.
    Log("explorer restarted");
    AddTrayIcon();
    tracker_.RequestRescan();
    SetTimer(hwnd_, T_MONITORS, 500, nullptr);
    return 0;
  }
  switch (m) {
    case WM_APP_TRAY: {
      // NOTIFYICON_VERSION_4: NIN_SELECT (left click / Enter) and WM_CONTEXTMENU
      // (right click). A left click or double click opens the settings window;
      // the right-click menu keeps only the quick actions.
      UINT ev = LOWORD(l);
      if (ev == WM_CONTEXTMENU) {
        POINT pt{GET_X_LPARAM(w), GET_Y_LPARAM(w)};
        ShowMenu(pt);
      } else if (ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONDBLCLK) {
        if (!opts_.harness) settingsWnd_.Show();
      }
      return 0;
    }
    case WM_APP_SECOND_INSTANCE:
      // Started again (START.cmd twice, autostart + manual): show the settings.
      if (!opts_.harness) settingsWnd_.Show();
      return 0;
    case WM_APP_FOREGROUND:
      CheckFullscreen();
      SetTimer(hwnd_, T_TOPMOST, 150, nullptr);
      return 0;
    case WM_APP_FS_CHECK:
      CheckFullscreen();
      return 0;
    case WM_APP_DECOR_UPDATE: {
      std::vector<SnowmanUpdate> ups;
      engine_.TakeSnowmanUpdates(ups);
      bool changed = false;
      // Balls stacked three high turn into a real snowman on that edge.
      std::vector<SnowballSnowmanSpawn> ballSpawns;
      engine_.TakeSnowballSpawns(ballSpawns);
      for (const auto& sp : ballSpawns) {
        if (static_cast<int>(state_.decor.snowmen.size()) >= kMaxSnowmen) break;
        SnowmanDecor s;
        s.id = state_.decor.nextId++;
        s.free = false;
        s.anchor.taskbar = sp.taskbar;
        s.anchor.hwnd = sp.surface;
        s.relX = clampf(sp.relX, 0.05f, 0.95f);
        s.scale = 1.f;
        s.hat = 1;
        s.hatColor = state_.settings.decorColor >= 0 ? state_.settings.decorColor % kDecorColorCount : 0;
        s.scarf = state_.settings.decorColor >= 0 ? state_.settings.decorColor % kDecorColorCount : 0;
        state_.decor.snowmen.push_back(s);
        changed = true;
      }
      for (const auto& up : ups) {
        for (auto& s : state_.decor.snowmen) {
          if (s.id != up.id) continue;
          if (std::fabs(s.relX - up.relX) > 1e-4f) {
            s.relX = up.relX;
            changed = true;
          }
          if (up.anchorChanged && !s.anchor.taskbar &&
              (s.anchor.hwnd != up.anchor.hwnd || s.anchor.title != up.anchor.title)) {
            s.anchor.hwnd = up.anchor.hwnd;
            s.anchor.title = up.anchor.title;
            changed = true;
          }
        }
      }
      if (changed) {
        PushDecor();
        ScheduleSave();
      }
      return 0;
    }
    case WM_HOTKEY:
      if (w == HK_TOGGLE) OnCommand(ID_ENABLED);
      else if (w == HK_DECORATE) decorate_ ? ExitDecorate() : EnterDecorate();
      return 0;
    case WM_TIMER:
      switch (w) {
        case T_FULLSCREEN:
          CheckFullscreen();
          if (DiagnosticsLogging())
            for (const std::string& line : DiagnosticsLines()) Log("diag: %s", line.c_str());
          break;
        case T_PAUSE_HOUR: SetPauseHour(false); break;
        case T_FS_DEBOUNCE:
          KillTimer(hwnd_, T_FS_DEBOUNCE);
          CheckFullscreen();
          break;
        case T_SAVE: SaveNow(); break;
        case T_MONITORS:
          KillTimer(hwnd_, T_MONITORS);
          engine_.Command(EngineCommand::MonitorsChanged);
          tracker_.RequestRescan();
          if (decorate_) {
            ExitDecorate();
          }
          break;
        case T_SLOW: {
          bool b = battery_, r = remote_, red = reduced_;
          float n = night_;
          RefreshSystemState();
          if (b != battery_ || r != remote_ || red != reduced_ || std::fabs(n - night_) > 0.01f) PushConfig();
          break;
        }
        case T_TOPMOST:
          KillTimer(hwnd_, T_TOPMOST);
          engine_.Command(EngineCommand::ReassertTopmost);
          break;
        case T_TRAIN_NOW:
          KillTimer(hwnd_, T_TRAIN_NOW);
          engine_.Command(EngineCommand::Train);
          Log("debug: gift train requested (--train-now)");
          break;
        case T_QUIT_AFTER:
          DestroyWindow(hwnd_);
          break;
      }
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      SetTimer(hwnd_, T_MONITORS, 300, nullptr);
      return 0;
    case WM_SETTINGCHANGE:
      if (w == SPI_SETWORKAREA) SetTimer(hwnd_, T_MONITORS, 300, nullptr);
      if (w == SPI_SETCLIENTAREAANIMATION) {
        RefreshSystemState();
        PushConfig();
      }
      return 0;
    case WM_POWERBROADCAST:
      if (w == PBT_APMSUSPEND) {
        systemPaused_ = true;
        SaveNow();
        PushConfig();
      } else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
        systemPaused_ = false;
        RefreshSystemState();
        PushConfig();
        SetTimer(hwnd_, T_MONITORS, 1000, nullptr);  // displays and GPU may have changed
      } else if (w == PBT_APMPOWERSTATUSCHANGE) {
        RefreshSystemState();
        PushConfig();
      } else if (w == PBT_POWERSETTINGCHANGE) {
        auto* ps = reinterpret_cast<POWERBROADCAST_SETTING*>(l);
        if (ps && IsEqualGUID(ps->PowerSetting, kDisplayState) && ps->DataLength >= sizeof(DWORD)) {
          displayOff_ = *reinterpret_cast<const DWORD*>(ps->Data) == 0;
        }
        RefreshSystemState();
        PushConfig();
      }
      return TRUE;
    case WM_WTSSESSION_CHANGE:
      if (w == WTS_SESSION_LOCK) systemPaused_ = true;
      else if (w == WTS_SESSION_UNLOCK) systemPaused_ = false;
      if (w == WTS_REMOTE_CONNECT || w == WTS_CONSOLE_CONNECT || w == WTS_REMOTE_DISCONNECT ||
          w == WTS_CONSOLE_DISCONNECT)
        SetTimer(hwnd_, T_MONITORS, 1000, nullptr);
      RefreshSystemState();
      PushConfig();
      return 0;
    case WM_QUERYENDSESSION:
      SaveNow();
      return TRUE;
    case WM_ENDSESSION:
      if (w) SaveNow();
      return 0;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      // Release everything tied to this window while it still exists.
      if (DiagnosticsLogging()) {
        std::string report = DiagnosticsReport();
        Log("diagnostics report at exit:");
        size_t start = 0;
        while (start < report.size()) {
          size_t end = report.find("\r\n", start);
          if (end == std::string::npos) end = report.size();
          Log("  %s", report.substr(start, end - start).c_str());
          start = end + 2;
        }
      }
      settingsWnd_.Destroy();
      KillTimer(h, T_FULLSCREEN);
      KillTimer(h, T_FS_DEBOUNCE);
      KillTimer(h, T_SLOW);
      KillTimer(h, T_PAUSE_HOUR);
      for (HPOWERNOTIFY& pn : powerNotify_) {
        if (pn) UnregisterPowerSettingNotification(pn);
        pn = nullptr;
      }
      WTSUnRegisterSessionNotification(h);
      UnregisterHotkeys();
      RemoveTrayIcon();
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(h, m, w, l);
  }
}

// -------------------------------------------------------------------- run

int App::Run(const LaunchOptions& opts) {
  opts_ = opts;
  g_app = this;
  LogInit(opts_.instance, opts_.console, opts_.logPath);
  Log("Desktop Snow %s starting (instance %s, Windows build %lu, harness %d)", SNOW_BUILD_ID,
      Narrow(opts_.instance).c_str(), OsBuild(), opts_.harness);

  // Single instance per instance name.
  std::wstring mutexName = L"Local\\DesktopSnow." + opts_.instance;
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  std::wstring ctlClass = L"DesktopSnowControl." + opts_.instance;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND other = FindWindowW(ctlClass.c_str(), nullptr);
    if (other) PostMessageW(other, WM_APP_SECOND_INSTANCE, 0, 0);
    Log("another instance is running; exiting");
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  HINSTANCE hinst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = ControlProc;
  wc.hInstance = hinst;
  wc.lpszClassName = ctlClass.c_str();
  RegisterClassExW(&wc);
  WNDCLASSEXW cc{};
  cc.cbSize = sizeof(cc);
  cc.lpfnWndProc = CatcherProc;
  cc.hInstance = hinst;
  std::wstring catcherClass = L"DesktopSnowDecorate." + opts_.instance;
  cc.lpszClassName = catcherClass.c_str();
  cc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
  RegisterClassExW(&cc);

  // A hidden top-level window (not message-only) so that broadcasts such as
  // WM_DISPLAYCHANGE, WM_SETTINGCHANGE and TaskbarCreated reach us.
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, ctlClass.c_str(), L"Desktop Snow", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                          hinst, nullptr);
  taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
  ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);

  LoadState();
  RefreshSystemState();
  if (!opts_.harness) InitSettingsWindow();
  tracker_.Start(hwnd_);
  engine_.Start(opts_, &tracker_, hwnd_);
  PushConfig();
  PushDecor();
  AddTrayIcon();
  RegisterHotkeys();
  WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);
  powerNotify_[0] = RegisterPowerSettingNotification(hwnd_, &kPowerSaving, DEVICE_NOTIFY_WINDOW_HANDLE);
  powerNotify_[1] = RegisterPowerSettingNotification(hwnd_, &kAcDc, DEVICE_NOTIFY_WINDOW_HANDLE);
  powerNotify_[2] = RegisterPowerSettingNotification(hwnd_, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
  // Fullscreen: WinEvents (foreground, foreground moved/resized, windows shown,
  // hidden, closed) trigger a check; this timer is the fallback.
  CheckFullscreen(true);
  SetTimer(hwnd_, T_FULLSCREEN, 500, nullptr);
  SetTimer(hwnd_, T_SLOW, 30000, nullptr);
  if (opts_.trainNow) SetTimer(hwnd_, T_TRAIN_NOW, 2000, nullptr);
  if (!opts_.harness && !opts_.noPersist && state_.decor.trees.empty() && state_.decor.snowmen.empty() &&
      !store_->file().empty()) {
    // First run: nothing to configure, just a hint where the menu lives.
    std::error_code ec;
    if (!std::filesystem::exists(store_->file(), ec)) Balloon(std::string(T(Str::Tagline)) + " " + T(Str::TipDecorateHint));
  }
  wchar_t qa[16] = {};
  int quitAfter = opts_.quitAfter;
  if (quitAfter <= 0 && GetEnvironmentVariableW(L"DESKTOPSNOW_QUIT_AFTER", qa, 16) > 0) quitAfter = _wtoi(qa);
  if (quitAfter > 0) SetTimer(hwnd_, T_QUIT_AFTER, static_cast<UINT>(quitAfter) * 1000, nullptr);
  if (DiagnosticsLogging()) Log("diagnostics: logging every pause decision every 0.5 s");

  std::wstring quitName = L"Local\\DesktopSnow." + opts_.instance + L".Quit";
  quitEvent_ = CreateEventW(nullptr, TRUE, FALSE, quitName.c_str());

  MSG msg;
  bool running = true;
  while (running) {
    DWORD rc = MsgWaitForMultipleObjectsEx(quitEvent_ ? 1 : 0, &quitEvent_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (quitEvent_ && rc == WAIT_OBJECT_0) {
      Log("quit event signalled");
      DestroyWindow(hwnd_);
      ResetEvent(quitEvent_);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  Log("shutting down");
  ExitDecorate();
  SaveNow();
  engine_.Stop();
  tracker_.Stop();
  if (icon_) DestroyIcon(icon_);
  if (quitEvent_) CloseHandle(quitEvent_);
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  g_app = nullptr;
  Log("bye");
  return 0;
}

}  // namespace snow::win
