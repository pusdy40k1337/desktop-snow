#include "platform.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace snow::win {

static int64_t QpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
  }();
  return f;
}

double Now() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return static_cast<double>(li.QuadPart) / static_cast<double>(QpcFreq());
}

double QpcToSeconds(int64_t qpc) { return static_cast<double>(qpc) / static_cast<double>(QpcFreq()); }
int64_t SecondsToQpc(double s) { return static_cast<int64_t>(s * static_cast<double>(QpcFreq())); }

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string ToLowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

float MonitorScale(HMONITOR mon) {
  UINT dx = 96, dy = 96;
  if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) return static_cast<float>(dx) / 96.f;
  return 1.f;
}

float ScaleAtPoint(int x, int y) { return MonitorScale(MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST)); }

static BOOL CALLBACK EnumMonProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
  auto* out = reinterpret_cast<std::vector<MonitorHandle>*>(lp);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) return TRUE;
  MonitorHandle h;
  h.handle = mon;
  h.info.id = Narrow(mi.szDevice);
  h.info.rect = RectI(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
  h.info.work = RectI(mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
  h.info.scale = MonitorScale(mon);
  h.info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  out->push_back(h);
  return TRUE;
}

std::vector<MonitorHandle> EnumerateMonitors() {
  std::vector<MonitorHandle> out;
  EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&out));
  // Primary first, then left-to-right: stable indices across calls.
  std::stable_sort(out.begin(), out.end(), [](const MonitorHandle& a, const MonitorHandle& b) {
    if (a.info.primary != b.info.primary) return a.info.primary;
    if (a.info.rect.left != b.info.rect.left) return a.info.rect.left < b.info.rect.left;
    return a.info.rect.top < b.info.rect.top;
  });
  return out;
}

DWORD OsBuild() {
  static const DWORD build = [] {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    if (fn && fn(&vi) == 0) return vi.dwBuildNumber;
    return DWORD(0);
  }();
  return build;
}

bool IsWindows11() { return OsBuild() >= 22000; }

uint16_t UiLanguagePrimaryId() { return static_cast<uint16_t>(PRIMARYLANGID(GetUserDefaultUILanguage())); }

std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return std::wstring(buf, n);
}

std::wstring ExeDir() {
  std::wstring p = ExePath();
  size_t s = p.find_last_of(L"\\/");
  return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)) && path) out = path;
  if (path) CoTaskMemFree(path);
  return out;
}

std::wstring AppDataDir() {
  std::wstring d = KnownFolder(FOLDERID_RoamingAppData);
  if (d.empty()) d = ExeDir();
  d += L"\\DesktopSnow";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

std::wstring LogDir() {
  std::wstring d = KnownFolder(FOLDERID_LocalAppData);
  if (d.empty()) d = ExeDir();
  d += L"\\DesktopSnow";
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\logs";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

std::wstring LegacySettingsPath() {
  std::wstring d = KnownFolder(FOLDERID_RoamingAppData);
  // Electron userData = %APPDATA%\<productName>; productName was "Снег на десктопе".
  return d + L"\\\x0421\x043D\x0435\x0433 \x043D\x0430 \x0434\x0435\x0441\x043A\x0442\x043E\x043F\x0435\\settings.json";
}

bool GetFrameBounds(HWND hwnd, RECT& out) {
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(out)))) return true;
  return GetWindowRect(hwnd, &out) != FALSE;
}

bool IsCloaked(HWND hwnd) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::string ProcessName(DWORD pid) {
  static std::mutex mu;
  static std::unordered_map<DWORD, std::pair<std::string, double>> cache;
  std::lock_guard<std::mutex> lock(mu);
  double now = Now();
  auto it = cache.find(pid);
  // PIDs are recycled: refresh entries after a minute.
  if (it != cache.end() && now - it->second.second < 60.0) return it->second.first;
  std::string name;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
      std::wstring full(buf, n);
      size_t s = full.find_last_of(L"\\/");
      name = ToLowerAscii(Narrow(s == std::wstring::npos ? full : full.substr(s + 1)));
    }
    CloseHandle(h);
  }
  if (cache.size() > 2048) cache.clear();
  cache[pid] = {name, now};
  return name;
}

std::string WindowTitle(HWND hwnd) {
  wchar_t buf[256];
  int n = GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
  return Narrow(std::wstring(buf, static_cast<size_t>(std::max(0, n))));
}

std::string WindowClass(HWND hwnd) {
  wchar_t buf[128];
  int n = GetClassNameW(hwnd, buf, static_cast<int>(std::size(buf)));
  return Narrow(std::wstring(buf, static_cast<size_t>(std::max(0, n))));
}

bool IsShellPopupClass(const std::string& cls) {
  static const char* kSkip[] = {"Progman",        "WorkerW",          "#32768",
                                "tooltips_class32", "SysShadow",      "Xaml_WindowedPopupClass",
                                "TaskListThumbnailWnd", "MultitaskingViewFrame", "ForegroundStaging",
                                "NotifyIconOverflowWindow", "TopLevelWindowForOverflowXamlIsland",
                                "Windows.UI.Core.CoreWindow", "Shell_InputSwitchTopLevelWindow",
                                "XamlExplorerHostIslandWindow", "TaskSwitcherWnd", "TaskSwitcherOverlayWnd",
                                "Shell_LightDismissOverlay", "IME", "MSCTFIME UI"};
  for (const char* s : kSkip)
    if (cls == s) return true;
  return false;
}

PowerState QueryPower() {
  PowerState p;
  SYSTEM_POWER_STATUS s{};
  if (GetSystemPowerStatus(&s)) {
    p.onBattery = s.ACLineStatus == 0;
    p.saver = (s.SystemStatusFlag & 1) != 0;  // battery saver on
  }
  return p;
}

bool SystemAnimationsEnabled() {
  BOOL on = TRUE;
  if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
  return on != FALSE;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"DesktopSnow";

bool AutostartEnabled() {
  wchar_t buf[1024];
  DWORD size = sizeof(buf);
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

bool SetAutostart(bool enable, const std::wstring& args) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return false;
  LONG rc;
  if (enable) {
    std::wstring cmd = L"\"" + ExePath() + L"\"";
    if (!args.empty()) cmd += L" " + args;
    rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    rc = RegDeleteValueW(key, kRunValue);
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

static std::mutex g_logMu;
static FILE* g_log = nullptr;
static bool g_console = false;

void LogInit(const std::wstring& instance, bool console, const std::wstring& file) {
  std::lock_guard<std::mutex> lock(g_logMu);
  g_console = console;
  if (!file.empty()) {
    // Explicit file (run.cmd, SnowHarness): append, so earlier sessions survive.
    if (_wfopen_s(&g_log, file.c_str(), L"ab") == 0) return;
    g_log = nullptr;
  }
  std::wstring path = LogDir() + L"\\" + instance + L".log";
  // Keep one previous log for diagnostics.
  std::wstring old = path + L".1";
  MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (_wfopen_s(&g_log, path.c_str(), L"wb") != 0) g_log = nullptr;
}

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[1200];
  snprintf(line, sizeof line, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
  OutputDebugStringA(line);
  std::lock_guard<std::mutex> lock(g_logMu);
  if (g_log) {
    fputs(line, g_log);
    fflush(g_log);
  }
  if (g_console) fputs(line, stdout);
}

bool ParseHotkey(const std::string& text, UINT& mods, UINT& vk) {
  mods = 0;
  vk = 0;
  std::string t = ToLowerAscii(text);
  size_t start = 0;
  while (start <= t.size()) {
    size_t plus = t.find('+', start);
    std::string part = t.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    while (!part.empty() && part.front() == ' ') part.erase(0, 1);
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (part == "ctrl" || part == "control") mods |= MOD_CONTROL;
    else if (part == "alt") mods |= MOD_ALT;
    else if (part == "shift") mods |= MOD_SHIFT;
    else if (part == "win") mods |= MOD_WIN;
    else if (part.size() == 1 && ((part[0] >= 'a' && part[0] <= 'z') || (part[0] >= '0' && part[0] <= '9')))
      vk = static_cast<UINT>(part[0] >= 'a' ? part[0] - 'a' + 'A' : part[0]);
    else if (part.size() >= 2 && part[0] == 'f') {
      int n = atoi(part.c_str() + 1);
      if (n >= 1 && n <= 24) vk = static_cast<UINT>(VK_F1 + n - 1);
    } else if (!part.empty()) {
      return false;
    }
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  return vk != 0 && mods != 0;
}

}  // namespace snow::win
