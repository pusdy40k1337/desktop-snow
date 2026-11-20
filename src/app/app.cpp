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
#include <ctime>

#include "../core/theme.h"

namespace snow::win {

namespace {

enum : UINT {
  ID_ENABLED = 100,
  ID_INT_LIGHT,
  ID_INT_NORMAL,
  ID_INT_HEAVY,
  ID_INT_BLIZZARD,
  ID_EFF_SNOW = 120,
  ID_EFF_CONFETTI,
  ID_EFF_NONE,
  ID_THEME_BASE = 140,
  ID_Q_PILES = 160,
  ID_Q_TASKBAR,
  ID_Q_WIND,
  ID_Q_CURSOR,
  ID_Q_SPARKLE,
  ID_Q_MELT,
  ID_Q_DAYNIGHT,
  ID_Q_REDUCED,
  ID_Q_BATTERY,
  ID_Q_VISITORS,
  ID_DECORATE = 180,
  ID_SANTA,
  ID_TRAIN,
  ID_YETI,
  ID_CLEAR_SNOW,
  ID_CLEAR_DECOR,
  ID_LANG_AUTO = 200,
  ID_LANG_EN,
  ID_LANG_RU,
  ID_LANG_UK,
  ID_STARTUP = 210,
  ID_DEBUG,
  ID_OPEN_FOLDER,
  ID_QUIT,
  ID_PAUSE_FULLSCREEN,
};

enum : UINT_PTR { T_FULLSCREEN = 1, T_SAVE, T_MONITORS, T_SLOW, T_TOPMOST, T_QUIT_AFTER, T_FS_DEBOUNCE };
constexpr UINT HK_TOGGLE = 1, HK_DECORATE = 2;
constexpr UINT WM_APP_SECOND_INSTANCE = WM_APP + 10;
const int kIntensities[4] = {50, 100, 150, 200};

App* g_app = nullptr;

// GUID_POWER_SAVING_STATUS / GUID_ACDC_POWER_SOURCE / GUID_CONSOLE_DISPLAY_STATE
const GUID kPowerSaving = {0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}};
const GUID kAcDc = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
const GUID kDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

std::wstring W(const char* s) { return Widen(s); }

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
  if (opts_.debug) state_.settings.debugOverlay = true;
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
  cfg.paused = !state_.settings.enabled || systemPaused_ || displayOff_;
  cfg.batteryMode = battery_;
  cfg.remoteSession = remote_;
  cfg.reducedMotion = reduced_;
  cfg.night = night_;
  cfg.hiddenMonitors = hiddenMonitors_;
  cfg.decorateMode = decorate_;
  cfg.activeTool = tool_;
  cfg.hoverId = hoverId_;
  cfg.lang = lang_;
  if (opts_.harness) {
    cfg.paused = false;
    cfg.hiddenMonitors.clear();
  }
  engine_.SetConfig(cfg);
}

void App::PushDecor() { engine_.SetDecor(state_.decor); }

namespace {

ShellNotifyState QueryShellState() {
  QUERY_USER_NOTIFICATION_STATE q{};
  if (FAILED(SHQueryUserNotificationState(&q))) return ShellNotifyState::Unknown;
  switch (q) {
    case QUNS_BUSY: return ShellNotifyState::Busy;
    case QUNS_RUNNING_D3D_FULL_SCREEN: return ShellNotifyState::D3DFullscreen;
    case QUNS_PRESENTATION_MODE: return ShellNotifyState::Presentation;
    default: return ShellNotifyState::Normal;
  }
}

// Top-level windows that could be a fullscreen app, in z-order (topmost first).
std::vector<FsWindow> FullscreenCandidates() {
  struct Ctx {
    std::vector<FsWindow> out;
    HWND fgRoot;
    DWORD ownPid;
  } ctx{{}, nullptr, GetCurrentProcessId()};
  HWND fg = GetForegroundWindow();
  ctx.fgRoot = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c->ownPid || IsCloaked(h)) return TRUE;
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
        RECT r;
        if (!GetFrameBounds(h, r) || r.right - r.left < 2 || r.bottom - r.top < 2) return TRUE;
        FsWindow w;
        w.frame = RectI(r.left, r.top, r.right, r.bottom);
        w.topmost = (ex & WS_EX_TOPMOST) != 0;
        w.zoomed = IsZoomed(h) != FALSE;
        w.captioned = (style & WS_CAPTION) == WS_CAPTION;
        w.floating = (ex & WS_EX_TOOLWINDOW) != 0 || GetWindow(h, GW_OWNER) != nullptr;
        w.foreground = h == c->fgRoot;
        c->out.push_back(w);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return std::move(ctx.out);
}

}  // namespace

void App::CheckFullscreen(bool immediate) {
  if (opts_.harness) return;
  std::vector<std::string> raw;
  ShellNotifyState shell = ShellNotifyState::Unknown;
  if (state_.settings.pauseFullscreen) {
    std::vector<MonitorHandle> mons = EnumerateMonitors();
    std::vector<FsMonitor> fm;
    for (const auto& m : mons) fm.push_back(FsMonitor{m.info.rect, m.info.work});
    shell = QueryShellState();
    std::vector<bool> fs = DetectFullscreen(fm, FullscreenCandidates(), shell);
    for (size_t i = 0; i < mons.size(); ++i)
      if (fs[i]) raw.push_back(mons[i].info.id);
    std::sort(raw.begin(), raw.end());
  }
  bool changed;
  if (immediate) {
    changed = raw != fsDebounce_.applied();
    fsDebounce_.reset(raw);
  } else {
    changed = fsDebounce_.update(Now(), raw);
  }
  double due = fsDebounce_.pendingIn(Now());
  if (due >= 0) SetTimer(hwnd_, T_FS_DEBOUNCE, static_cast<UINT>(due * 1000.0) + 15, nullptr);
  else KillTimer(hwnd_, T_FS_DEBOUNCE);
  if (!changed) return;
  hiddenMonitors_ = fsDebounce_.applied();
  std::string list;
  for (const auto& id : hiddenMonitors_) list += (list.empty() ? "" : ", ") + id;
  Log("fullscreen: %zu monitor(s) paused%s%s (shell state %d)", hiddenMonitors_.size(), list.empty() ? "" : ": ",
      list.c_str(), static_cast<int>(shell));
  PushConfig();
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
  auto item = [](HMENU m, UINT id, const std::wstring& text, bool checked, bool radio = false, bool enabled = true) {
    MENUITEMINFOW mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mi.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
    mi.fState = (checked ? MFS_CHECKED : 0) | (enabled ? 0 : MFS_DISABLED);
    mi.wID = id;
    mi.dwTypeData = const_cast<wchar_t*>(text.c_str());
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
  };
  auto sub = [](HMENU m, HMENU child, const std::wstring& text) {
    AppendMenuW(m, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(child), text.c_str());
  };
  HMENU menu = CreatePopupMenu();
  item(menu, 0, W(T(Str::AppName)) + L" \x2014 " + W(T(Str::Tagline)), false, false, false);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  item(menu, ID_ENABLED, W(T(Str::MenuEnabled)) + L"\t" + Widen(s.hotkeyToggle), s.enabled);
  item(menu, ID_PAUSE_FULLSCREEN, W(T(Str::MenuPauseFullscreen)), s.pauseFullscreen);
  item(menu, ID_Q_TASKBAR, W(T(Str::QuickTaskbarPile)), s.piles.onTaskbar);

  HMENU inten = CreatePopupMenu();
  const Str names[4] = {Str::MenuIntensityLight, Str::MenuIntensityNormal, Str::MenuIntensityHeavy,
                        Str::MenuIntensityBlizzard};
  for (int i = 0; i < 4; ++i) item(inten, ID_INT_LIGHT + i, W(T(names[i])), s.intensity == kIntensities[i], true);
  sub(menu, inten, W(T(Str::MenuIntensity)));

  HMENU eff = CreatePopupMenu();
  item(eff, ID_EFF_SNOW, W(T(Str::EffectSnow)), s.effect == "snow", true);
  item(eff, ID_EFF_CONFETTI, W(T(Str::EffectConfetti)), s.effect == "confetti", true);
  item(eff, ID_EFF_NONE, W(T(Str::EffectNone)), s.effect == "none", true);
  sub(menu, eff, W(T(Str::MenuEffect)));

  HMENU theme = CreatePopupMenu();
  const Str themeNames[4] = {Str::ThemeClassic, Str::ThemeFrost, Str::ThemeCandy, Str::ThemeGolden};
  const auto& ids = ThemeIds();
  for (size_t i = 0; i < ids.size() && i < 4; ++i)
    item(theme, ID_THEME_BASE + static_cast<UINT>(i), W(T(themeNames[i])), s.theme == ids[i], true);
  sub(menu, theme, W(T(Str::MenuTheme)));

  HMENU quick = CreatePopupMenu();
  item(quick, ID_Q_PILES, W(T(Str::QuickPiles)), s.piles.enabled);
  item(quick, ID_Q_WIND, W(T(Str::QuickWind)), s.extras.windGusts);
  item(quick, ID_Q_CURSOR, W(T(Str::QuickCursorSweep)), s.extras.cursorSweep);
  item(quick, ID_Q_SPARKLE, W(T(Str::QuickSparkle)), s.extras.sparkle);
  item(quick, ID_Q_MELT, W(T(Str::QuickMelt)), s.extras.melt);
  item(quick, ID_Q_DAYNIGHT, W(T(Str::QuickDayNight)), s.extras.dayNightTint);
  item(quick, ID_Q_REDUCED, W(T(Str::QuickReducedMotion)), reduced_);
  item(quick, ID_Q_BATTERY, W(T(Str::QuickBatterySaver)), s.batterySaver);
  item(quick, ID_Q_VISITORS, W(T(Str::QuickVisitors)), s.extras.santa || s.extras.train || s.extras.yeti);
  sub(menu, quick, W(T(Str::MenuQuick)));

  item(menu, ID_DECORATE, W(T(Str::MenuDecorate)) + L"\t" + Widen(s.hotkeyDecorate), decorate_);
  HMENU surprise = CreatePopupMenu();
  item(surprise, ID_SANTA, W(T(Str::VisitorSanta)), false);
  item(surprise, ID_TRAIN, W(T(Str::VisitorTrain)), false);
  item(surprise, ID_YETI, W(T(Str::VisitorYeti)), false, false, !state_.decor.snowmen.empty());
  sub(menu, surprise, W(T(Str::MenuSurprise)));
  item(menu, ID_CLEAR_SNOW, W(T(Str::MenuClearSnow)), false);
  item(menu, ID_CLEAR_DECOR, W(T(Str::MenuClearDecorations)), false, false,
       !state_.decor.trees.empty() || !state_.decor.snowmen.empty());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  HMENU langM = CreatePopupMenu();
  item(langM, ID_LANG_AUTO, W(T(Str::LangAuto)), s.language == "auto", true);
  item(langM, ID_LANG_EN, L"English", s.language == "en", true);
  item(langM, ID_LANG_RU, L"\x0420\x0443\x0441\x0441\x043A\x0438\x0439", s.language == "ru", true);
  item(langM, ID_LANG_UK, L"\x0423\x043A\x0440\x0430\x0457\x043D\x0441\x044C\x043A\x0430", s.language == "uk", true);
  sub(menu, langM, W(T(Str::MenuLanguage)));
  item(menu, ID_STARTUP, W(T(Str::MenuStartup)), s.launchAtStartup);
  item(menu, ID_DEBUG, W(T(Str::MenuDebug)), s.debugOverlay);
  item(menu, ID_OPEN_FOLDER, W(T(Str::MenuOpenFolder)), false);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  item(menu, ID_QUIT, W(T(Str::MenuQuit)), false);

  // Required for the menu to close when the user clicks elsewhere (documented quirk).
  SetForegroundWindow(hwnd_);
  UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
  flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  UINT cmd = TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);  // destroys submenus too
  if (cmd) OnCommand(cmd);
}

void App::OnCommand(UINT id) {
  Settings& s = state_.settings;
  bool save = true;
  switch (id) {
    case ID_ENABLED: s.enabled = !s.enabled; break;
    case ID_PAUSE_FULLSCREEN: s.pauseFullscreen = !s.pauseFullscreen; break;
    case ID_INT_LIGHT:
    case ID_INT_NORMAL:
    case ID_INT_HEAVY:
    case ID_INT_BLIZZARD: s.intensity = kIntensities[id - ID_INT_LIGHT]; break;
    case ID_EFF_SNOW: s.effect = "snow"; break;
    case ID_EFF_CONFETTI: s.effect = "confetti"; break;
    case ID_EFF_NONE: s.effect = "none"; break;
    case ID_Q_PILES: s.piles.enabled = !s.piles.enabled; break;
    case ID_Q_TASKBAR: s.piles.onTaskbar = !s.piles.onTaskbar; break;
    case ID_Q_WIND: s.extras.windGusts = !s.extras.windGusts; break;
    case ID_Q_CURSOR: s.extras.cursorSweep = !s.extras.cursorSweep; break;
    case ID_Q_SPARKLE: s.extras.sparkle = !s.extras.sparkle; break;
    case ID_Q_MELT: s.extras.melt = !s.extras.melt; break;
    case ID_Q_DAYNIGHT: s.extras.dayNightTint = !s.extras.dayNightTint; break;
    case ID_Q_REDUCED: s.reducedMotion = reduced_ ? "off" : "on"; break;
    case ID_Q_BATTERY: s.batterySaver = !s.batterySaver; break;
    case ID_Q_VISITORS: {
      bool on = !(s.extras.santa || s.extras.train || s.extras.yeti);
      s.extras.santa = s.extras.train = s.extras.yeti = on;
      break;
    }
    case ID_DECORATE:
      save = false;
      decorate_ ? ExitDecorate() : EnterDecorate();
      break;
    case ID_SANTA: engine_.Command(EngineCommand::Santa); save = false; break;
    case ID_TRAIN: engine_.Command(EngineCommand::Train); save = false; break;
    case ID_YETI: engine_.Command(EngineCommand::Yeti); save = false; break;
    case ID_CLEAR_SNOW: engine_.Command(EngineCommand::ClearSnow); save = false; break;
    case ID_CLEAR_DECOR:
      state_.decor.trees.clear();
      state_.decor.snowmen.clear();
      PushDecor();
      break;
    case ID_LANG_AUTO: s.language = "auto"; break;
    case ID_LANG_EN: s.language = "en"; break;
    case ID_LANG_RU: s.language = "ru"; break;
    case ID_LANG_UK: s.language = "uk"; break;
    case ID_STARTUP:
      if (SetAutostart(!s.launchAtStartup, L"")) s.launchAtStartup = !s.launchAtStartup;
      break;
    case ID_DEBUG: s.debugOverlay = !s.debugOverlay; break;
    case ID_OPEN_FOLDER:
      ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      save = false;
      break;
    case ID_QUIT:
      DestroyWindow(hwnd_);
      return;
    default:
      if (id >= ID_THEME_BASE && id < ID_THEME_BASE + ThemeIds().size()) s.theme = ThemeIds()[id - ID_THEME_BASE];
      else save = false;
  }
  Sanitize(s);
  RefreshSystemState();
  if (id >= ID_LANG_AUTO && id <= ID_LANG_UK) AddTrayIcon();  // tooltip language
  if (id == ID_PAUSE_FULLSCREEN) CheckFullscreen(true);  // the user asked: no debounce
  PushConfig();
  if (save) ScheduleSave();
}

// ---------------------------------------------------------- decorate mode

void App::EnterDecorate() {
  if (decorate_ || opts_.harness) return;
  decorate_ = true;
  tool_ = Tool::Tree;
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
    HWND above = i < overlays.size() ? overlays[i] : HWND_TOPMOST;
    SetWindowPos(h, above, r.left, r.top, r.width(), r.height(), SWP_SHOWWINDOW);
    catchers_.push_back(h);
  }
  if (!catchers_.empty()) SetForegroundWindow(catchers_.front());
  PushConfig();
}

void App::ExitDecorate() {
  if (!decorate_) return;
  decorate_ = false;
  dragging_ = pressed_ = false;
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
  // Snowmen stand on windows in front of the desktop trees: test them first.
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < layout.size(); ++i) {
      const auto& it = layout[i];
      if (!it.visible || it.tree != (pass == 1)) continue;
      if (x < it.left || x >= it.right || y < it.top || y >= it.bottom) continue;
      if (it.tree) {
        float u, v;
        TreeScreenToLocal(Vec2(it.baseX, it.baseY), it.height, Vec2(x, y), u, v);
        bool trunk = v >= 0.86f && std::fabs(u - 0.5f) < 0.1f;
        bool star = v < 0.12f && std::fabs(u - 0.5f) < 0.12f;
        if (!TreeFoliageContains(u, v) && !trunk && !star) continue;
      }
      isTree = it.tree;
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool App::AnchorAt(POINT pt, WindowAnchor& anchor, float& relX) {
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

void App::DecorClick(POINT pt) {
  DecorState& d = state_.decor;
  std::vector<DecorLayoutItem> layout;
  bool isTree = false;
  int hit = HitItem(pt, layout, isTree);
  const Theme& th = GetTheme(state_.settings.theme);
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
  TreeDecor* tree = hit >= 0 && isTree ? findTree(layout[static_cast<size_t>(hit)].id) : nullptr;
  SnowmanDecor* man = hit >= 0 && !isTree ? findSnowman(layout[static_cast<size_t>(hit)].id) : nullptr;
  float u = 0, v = 0;
  if (tree) {
    const auto& it = layout[static_cast<size_t>(hit)];
    TreeScreenToLocal(Vec2(it.baseX, it.baseY), it.height, Vec2(static_cast<float>(pt.x), static_cast<float>(pt.y)), u, v);
  }
  bool changed = false;
  switch (tool_) {
    case Tool::Tree:
      if (!tree && !man) {
        if (static_cast<int>(d.trees.size()) >= kMaxTrees) {
          Balloon(T(Str::TipTreeLimit));
          break;
        }
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        TreeDecor t;
        t.id = d.nextId++;
        t.monitor = Narrow(mi.szDevice);
        float h = TreeHeightPx(1.f, MonitorScale(mon));
        // The click marks the middle of the tree; the base sits half a tree lower.
        float baseY = std::min(static_cast<float>(pt.y) + h * 0.5f, static_cast<float>(mi.rcWork.bottom));
        t.nx = clampf(static_cast<float>(pt.x - mi.rcWork.left) / std::max(1L, mi.rcWork.right - mi.rcWork.left), 0, 1);
        t.ny = clampf((baseY - mi.rcWork.top) / std::max(1L, mi.rcWork.bottom - mi.rcWork.top), 0, 1);
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
        o.color = colorCycle_++ % static_cast<int>(th.ornaments.size());
        tree->ornaments.push_back(o);
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
      }
      break;
    case Tool::Snowman:
      if (!man && static_cast<int>(d.snowmen.size()) < kMaxSnowmen) {
        SnowmanDecor s;
        s.id = d.nextId++;
        AnchorAt(pt, s.anchor, s.relX);
        s.scarf = colorCycle_++ % 4;
        d.snowmen.push_back(s);
        changed = true;
      }
      break;
    case Tool::Hat:
      if (man) {
        man->hat = (man->hat + 1) % 4;
        changed = true;
      }
      break;
    case Tool::Scarf:
      if (man) {
        man->scarf = (man->scarf + 1) % 4;
        changed = true;
      }
      break;
    case Tool::Erase:
      if (tree) {
        // Nearest ornament first, then the star, then the whole tree.
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
      if (w == VK_ESCAPE) ExitDecorate();
      return 0;
    case WM_RBUTTONUP:
      ExitDecorate();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ClientToScreen(h, &pt);
      if (m == WM_LBUTTONDOWN) {
        // Palette first.
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        PaletteLayout pl = LayoutPalette(RectI(mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom),
                                         MonitorScale(mon));
        Tool t;
        if (PaletteHit(pl, pt.x, pt.y, t)) {
          if (t == Tool::Done) ExitDecorate();
          else {
            tool_ = t;
            PushConfig();
          }
          return 0;
        }
        std::vector<DecorLayoutItem> layout;
        bool isTree = false;
        int hit = HitItem(pt, layout, isTree);
        pressed_ = true;
        dragging_ = false;
        pressPt_ = pt;
        dragId_ = hit >= 0 ? layout[static_cast<size_t>(hit)].id : 0;
        dragTree_ = isTree;
        SetCapture(h);
        return 0;
      }
      if (m == WM_MOUSEMOVE) {
        if (pressed_ && dragId_ && !dragging_ && (std::abs(pt.x - pressPt_.x) > 4 || std::abs(pt.y - pressPt_.y) > 4))
          dragging_ = true;
        if (dragging_) {
          DecorState& d = state_.decor;
          if (dragTree_) {
            for (auto& t : d.trees) {
              if (t.id != dragId_) continue;
              HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
              MONITORINFOEXW mi{};
              mi.cbSize = sizeof(mi);
              GetMonitorInfoW(mon, &mi);
              float hgt = TreeHeightPx(t.scale, MonitorScale(mon));
              float baseY = std::min(static_cast<float>(pt.y) + hgt * 0.5f, static_cast<float>(mi.rcWork.bottom));
              t.monitor = Narrow(mi.szDevice);
              t.nx = clampf(static_cast<float>(pt.x - mi.rcWork.left) / std::max(1L, mi.rcWork.right - mi.rcWork.left),
                            0, 1);
              t.ny = clampf((baseY - mi.rcWork.top) / std::max(1L, mi.rcWork.bottom - mi.rcWork.top), 0, 1);
            }
          } else {
            for (auto& s : d.snowmen)
              if (s.id == dragId_) AnchorAt(pt, s.anchor, s.relX);
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
          PushConfig();
        }
        return 0;
      }
      // WM_LBUTTONUP
      ReleaseCapture();
      bool wasDrag = dragging_;
      pressed_ = dragging_ = false;
      if (wasDrag) ScheduleSave();
      else DecorClick(pt);
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
      // (right click). WM_LBUTTONUP also arrives for a left click; ignoring it
      // avoids opening the menu twice.
      UINT ev = LOWORD(l);
      if (ev == WM_CONTEXTMENU || ev == NIN_SELECT || ev == NIN_KEYSELECT) {
        POINT pt{GET_X_LPARAM(w), GET_Y_LPARAM(w)};
        ShowMenu(pt);
      }
      return 0;
    }
    case WM_APP_SECOND_INSTANCE: {
      POINT pt;
      GetCursorPos(&pt);
      ShowMenu(pt);
      return 0;
    }
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
        case T_FULLSCREEN: CheckFullscreen(); break;
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
      KillTimer(h, T_FULLSCREEN);
      KillTimer(h, T_FS_DEBOUNCE);
      KillTimer(h, T_SLOW);
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
  if (!opts_.harness && !opts_.noPersist && state_.decor.trees.empty() && state_.decor.snowmen.empty() &&
      !store_->file().empty()) {
    // First run: nothing to configure, just a hint where the menu lives.
    std::error_code ec;
    if (!std::filesystem::exists(store_->file(), ec)) Balloon(std::string(T(Str::Tagline)) + " " + T(Str::TipDecorateHint));
  }
  wchar_t qa[16] = {};
  if (GetEnvironmentVariableW(L"DESKTOPSNOW_QUIT_AFTER", qa, 16) > 0) SetTimer(hwnd_, T_QUIT_AFTER, _wtoi(qa) * 1000, nullptr);

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
