// Windows platform helpers: clocks, monitors, DPI, paths, power, autostart, logging.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../core/world.h"

namespace snow::win {

// Custom messages posted to the control window.
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_FOREGROUND = WM_APP + 2;
constexpr UINT WM_APP_DECOR_UPDATE = WM_APP + 3;
constexpr UINT WM_APP_ENGINE_STARTED = WM_APP + 4;
constexpr UINT WM_APP_FS_CHECK = WM_APP + 5;  // re-evaluate fullscreen (foreground moved/resized)

double Now();                      // QPC seconds
double QpcToSeconds(int64_t qpc);  // absolute QPC ticks -> seconds (same clock as Now)
int64_t SecondsToQpc(double s);

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& w);
std::string ToLowerAscii(std::string s);

struct MonitorHandle {
  HMONITOR handle;
  MonitorInfo info;
};
std::vector<MonitorHandle> EnumerateMonitors();
float MonitorScale(HMONITOR mon);
float ScaleAtPoint(int x, int y);

DWORD OsBuild();
bool IsWindows11();
uint16_t UiLanguagePrimaryId();

std::wstring ExePath();
std::wstring ExeDir();
std::wstring AppDataDir();    // %APPDATA%\DesktopSnow (created)
std::wstring LogDir();        // %LOCALAPPDATA%\DesktopSnow\logs (created)
std::wstring LegacySettingsPath();  // the Electron app's settings.json

// Visible frame bounds (no invisible resize borders). Falls back to GetWindowRect.
bool GetFrameBounds(HWND hwnd, RECT& out);
bool IsCloaked(HWND hwnd);
std::string ProcessName(DWORD pid);  // lower-case exe file name, cached
std::string WindowTitle(HWND hwnd);
std::string WindowClass(HWND hwnd);
// Desktop, shell popups and input/IME windows that never hold snow and never
// count as an app (Alt+Tab, Start, menus, tooltips...). Taskbars are not included.
bool IsShellPopupClass(const std::string& cls);

struct PowerState {
  bool onBattery = false;
  bool saver = false;
};
PowerState QueryPower();
bool SystemAnimationsEnabled();

bool AutostartEnabled();
bool SetAutostart(bool enable, const std::wstring& args);

void LogInit(const std::wstring& instance, bool console, const std::wstring& file = {});
void Log(const char* fmt, ...);

// Parses "Ctrl+Alt+S" style strings for RegisterHotKey.
bool ParseHotkey(const std::string& text, UINT& mods, UINT& vk);

}  // namespace snow::win
