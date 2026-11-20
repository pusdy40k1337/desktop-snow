// SnowHarness: measures, from real screen captures, how far DesktopSnow's
// overlay is drawn from the real edge of a window that is being moved along
// scripted sweeps. All coordinates are physical pixels.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../core/shared_stats.h"
#include "capture.h"
#include "detect.h"
#include "sweeps.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace fs = std::filesystem;

#ifndef SNOW_BUILD_ID
#define SNOW_BUILD_ID "dev"
#endif
using namespace snowharness;

namespace {

constexpr int kTargetW = 480;
constexpr int kTargetH = 320;
constexpr int kRoiMargin = 96;
constexpr int kTopInset = 96;
constexpr int kSideInset = 16;
constexpr int kFramedW = 600;
constexpr int kFramedH = 400;
constexpr DWORD kReadyTimeoutMs = 15000;
constexpr DWORD kQuitTimeoutMs = 5000;
constexpr uint32_t kAcquireTimeoutMs = 100;
constexpr DWORD kDwmaBorderColor = 34;   // DWMWA_BORDER_COLOR (Windows 11 SDK)
constexpr DWORD kDwmaCaptionColor = 35;  // DWMWA_CAPTION_COLOR
constexpr wchar_t kReadyEventName[] = L"Local\\DesktopSnow.harness.Ready";
constexpr wchar_t kQuitEventName[] = L"Local\\DesktopSnow.harness.Quit";
constexpr wchar_t kTargetClass[] = L"SnowHarnessTarget";
constexpr wchar_t kBackdropClass[] = L"SnowHarnessBackdrop";
constexpr wchar_t kFramedClass[] = L"SnowHarnessFramed";
constexpr wchar_t kCanvasClass[] = L"SnowHarnessCanvas";
constexpr wchar_t kFullscreenClass[] = L"SnowHarnessFullscreen";
constexpr wchar_t kStatsMapName[] = L"Local\\DesktopSnow.harness.Stats";

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitAcceptance = 2;
constexpr int kExitNotReady = 3;
constexpr int kExitNoCapture = 4;

int64_t g_qpcFreq = 1;
std::atomic<long> g_targetClicks{0};

// ---- small utilities --------------------------------------------------------

int64_t Qpc() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double QpcToMs(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_qpcFreq); }

std::string g_transcript;  // everything printed, for the results file

void Print(const std::string& s) {
  std::fputs(s.c_str(), stdout);
  std::fflush(stdout);
  g_transcript += s;
}

template <class... Args>
void Printf(std::format_string<Args...> fmt, Args&&... args) {
  Print(std::format(fmt, std::forward<Args>(args)...));
}

std::string Utf8(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string PathUtf8(const fs::path& p) { return Utf8(p.native()); }

std::vector<std::string> SplitList(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t comma = s.find(',', start);
    const size_t end = comma == std::string::npos ? s.size() : comma;
    std::string item = s.substr(start, end - start);
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

bool ParseInt(const std::string& s, int lo, int hi, int* out) {
  if (s.empty() || s.size() > 9) return false;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

std::string RectText(const RECT& r) { return std::format("({},{})-({},{})", r.left, r.top, r.right, r.bottom); }

std::string JsonString(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          o += std::format("\\u{:04x}", static_cast<int>(c));
        } else {
          o += c;
        }
    }
  }
  return o + "\"";
}

std::string JsonNumber(double v, int decimals = 3) {
  if (!std::isfinite(v)) return "null";
  return std::format("{:.{}f}", v, decimals);
}

std::string JsonRect(const RECT& r) { return std::format("[{}, {}, {}, {}]", r.left, r.top, r.right, r.bottom); }

std::string JsonStringArray(const std::vector<std::string>& v) {
  std::string o = "[";
  for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + JsonString(v[i]);
  return o + "]";
}

template <class F>
F LoadProc(const wchar_t* module, const char* name) {
  HMODULE m = GetModuleHandleW(module);
  if (!m) return nullptr;
  // Via void(*)() to keep GCC's -Wcast-function-type quiet.
  return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(m, name)));
}

fs::path ExeDirectory() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return fs::current_path();
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);
  }
  return fs::path(buf).parent_path();
}

// Quotes one argument following the CommandLineToArgvW rules.
std::wstring QuoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring r = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') {
      ++slashes;
      ++i;
    }
    if (i == a.size()) {
      r.append(slashes * 2, L'\\');
      break;
    }
    if (a[i] == L'"') {
      r.append(slashes * 2 + 1, L'\\');
    } else {
      r.append(slashes, L'\\');
    }
    r.push_back(a[i]);
  }
  r.push_back(L'"');
  return r;
}

void SleepMs(double ms) {
  if (ms > 0) Sleep(static_cast<DWORD>(std::lround(ms)));
}

// ---- options ----------------------------------------------------------------

struct Mode {
  std::string name;
  bool vsync = false;
  bool predict = false;
  bool stepped = false;       // step-aware predictor
  double marginMs = -1;       // latch margin override, -1 = overlay default
};

struct Options {
  fs::path exe;
  fs::path out;
  std::vector<Mode> modes;
  std::vector<int> rates;
  std::vector<SweepKind> sweeps;
  CaptureMode capture = CaptureMode::Auto;
  bool quick = false;
  bool strict = false;
  bool help = false;
  int dumpInvalid = 10;
  int snowfallSec = -1;  // -1 = default (180 s, quick 60 s)
  bool fullscreenTest = true;
  bool fullscreenOnly = false;
};

// <latch|vsync>-<predict|nopredict|step>[@<latch margin ms>], e.g. latch-step@0.5
bool ParseMode(const std::string& name, Mode* m) {
  std::string base = name;
  double margin = -1;
  const size_t at = name.find('@');
  if (at != std::string::npos) {
    base = name.substr(0, at);
    try {
      margin = std::stod(name.substr(at + 1));
    } catch (...) {
      return false;
    }
    if (margin < 0 || margin > 10) return false;
  }
  const size_t dash = base.find('-');
  if (dash == std::string::npos) return false;
  const std::string sync = base.substr(0, dash);
  const std::string pred = base.substr(dash + 1);
  if (sync != "latch" && sync != "vsync") return false;
  if (pred != "predict" && pred != "nopredict" && pred != "step") return false;
  m->name = name;
  m->vsync = sync == "vsync";
  m->predict = pred != "nopredict";
  m->stepped = pred == "step";
  m->marginMs = margin;
  return true;
}

void PrintUsage() {
  Print(
      "SnowHarness - measures DesktopSnow overlay alignment against a moving window.\n"
      "\n"
      "Usage: SnowHarness.exe [options]\n"
      "  --exe <path>        overlay executable (default: DesktopSnow.exe next to SnowHarness.exe)\n"
      "  --out <dir>         output directory (default: harness-results\\<yyyyMMdd-HHmmss>)\n"
      "  --modes <list>      overlay modes, comma separated, each <latch|vsync>-<predict|nopredict|step>[@margin_ms]\n"
      "                      (default: latch-step,vsync-nopredict = the app's default and the naive baseline)\n"
      "  --rates <list>      mover step rates in Hz, e.g. 60,125,144,1000 (default: 60,144,1000)\n"
      "  --sweeps <list>     subset of still,slow,fast,sine,stop,vsweep (default: all)\n"
      "  --capture <mode>    auto | dd | gdi (default: auto = Desktop Duplication, GDI fallback)\n"
      "  --snowfall <sec>    afterwards, plain snowfall with nothing moving, logging fps and\n"
      "                      frame-wait timeouts once a minute (default 180, --quick 60, 0 = skip)\n"
      "  --fullscreen-test <0|1>  fake fullscreen app: the snow must stop, the load drop and\n"
      "                      the snow come back after it closes (default 1, about 25 s)\n"
      "  --fullscreen-only   run only the fullscreen test\n"
      "  --quick             halve every duration; default rates 60,144\n"
      "  --strict            exit with code 2 when the acceptance criteria fail\n"
      "  --dump-invalid <n>  save up to n invalid capture ROIs as BMP under <out>\\dumps (default 10)\n"
      "  --help              show this text\n"
      "\n"
      "Exit codes: 0 ok, 1 bad arguments or setup failure, 2 acceptance failed (--strict),\n"
      "            3 overlay never became ready, 4 no capture backend or < 10 valid frames.\n");
}

bool ParseOptions(int argc, wchar_t** argv, Options* o, std::string* err) {
  bool modesSet = false, ratesSet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = Utf8(argv[i]);
    auto value = [&](std::string* v) {
      if (i + 1 >= argc) {
        *err = "missing value for " + a;
        return false;
      }
      *v = Utf8(argv[++i]);
      return true;
    };
    std::string v;
    if (a == "--help" || a == "-h" || a == "/?") {
      o->help = true;
    } else if (a == "--quick") {
      o->quick = true;
    } else if (a == "--strict") {
      o->strict = true;
    } else if (a == "--exe") {
      if (!value(&v)) return false;
      o->exe = fs::path(Wide(v));
    } else if (a == "--out") {
      if (!value(&v)) return false;
      o->out = fs::path(Wide(v));
    } else if (a == "--modes") {
      if (!value(&v)) return false;
      o->modes.clear();
      for (const std::string& item : SplitList(v)) {
        Mode m;
        if (!ParseMode(item, &m)) {
          *err = "invalid mode '" + item + "' (expected <latch|vsync>-<predict|nopredict|step>[@margin_ms])";
          return false;
        }
        o->modes.push_back(m);
      }
      modesSet = !o->modes.empty();
    } else if (a == "--rates") {
      if (!value(&v)) return false;
      o->rates.clear();
      for (const std::string& item : SplitList(v)) {
        int r = 0;
        if (!ParseInt(item, 1, 4000, &r)) {
          *err = "invalid rate '" + item + "' (1..4000 Hz)";
          return false;
        }
        o->rates.push_back(r);
      }
      ratesSet = !o->rates.empty();
    } else if (a == "--sweeps") {
      if (!value(&v)) return false;
      o->sweeps.clear();
      for (const std::string& item : SplitList(v)) {
        SweepKind k = SweepKind::Still;
        if (!ParseSweepName(item, &k)) {
          *err = "invalid sweep '" + item + "'";
          return false;
        }
        o->sweeps.push_back(k);
      }
    } else if (a == "--capture") {
      if (!value(&v)) return false;
      if (v == "auto") {
        o->capture = CaptureMode::Auto;
      } else if (v == "dd") {
        o->capture = CaptureMode::DesktopDuplication;
      } else if (v == "gdi") {
        o->capture = CaptureMode::Gdi;
      } else {
        *err = "invalid --capture '" + v + "' (auto, dd or gdi)";
        return false;
      }
    } else if (a == "--dump-invalid") {
      if (!value(&v)) return false;
      if (!ParseInt(v, 0, 100000, &o->dumpInvalid)) {
        *err = "invalid --dump-invalid '" + v + "'";
        return false;
      }
    } else if (a == "--snowfall") {
      if (!value(&v)) return false;
      if (!ParseInt(v, 0, 3600, &o->snowfallSec)) {
        *err = "invalid --snowfall '" + v + "' (0..3600 s)";
        return false;
      }
    } else if (a == "--fullscreen-test") {
      int on = 1;
      if (!value(&v)) return false;
      if (!ParseInt(v, 0, 1, &on)) {
        *err = "invalid --fullscreen-test '" + v + "' (0 or 1)";
        return false;
      }
      o->fullscreenTest = on != 0;
    } else if (a == "--fullscreen-only") {
      o->fullscreenOnly = true;
      o->fullscreenTest = true;
    } else {
      *err = "unknown option '" + a + "'";
      return false;
    }
  }
  if (!modesSet) {
    o->modes.clear();
    for (const std::string& item : SplitList("latch-step,vsync-nopredict")) {
      Mode m;
      ParseMode(item, &m);
      o->modes.push_back(m);
    }
  }
  if (!ratesSet) o->rates = o->quick ? std::vector<int>{60, 144} : std::vector<int>{60, 144, 1000};
  if (o->sweeps.empty()) o->sweeps.assign(std::begin(kAllSweeps), std::end(kAllSweeps));
  if (o->snowfallSec < 0) o->snowfallSec = o->quick || o->fullscreenOnly ? (o->fullscreenOnly ? 0 : 60) : 180;
  if (o->fullscreenOnly) o->modes.clear();
  return true;
}

// ---- display ----------------------------------------------------------------

struct DisplayInfo {
  HMONITOR monitor = nullptr;
  RECT rcMonitor{};
  RECT rcWork{};
  std::string device;
  int width = 0;
  int height = 0;
  DWORD refreshHz = 0;
  double dwmRefreshHz = 0;
  UINT dpi = 96;
  std::string os;
  bool perMonitorV2 = false;
  std::string dpiAwareness;
};

DisplayInfo QueryDisplay() {
  DisplayInfo d;
  d.monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  if (GetMonitorInfoW(d.monitor, &mi)) {
    d.rcMonitor = mi.rcMonitor;
    d.rcWork = mi.rcWork;
    d.device = Utf8(mi.szDevice);
  }
  d.width = d.rcMonitor.right - d.rcMonitor.left;
  d.height = d.rcMonitor.bottom - d.rcMonitor.top;
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) d.refreshHz = dm.dmDisplayFrequency;
  DWM_TIMING_INFO ti{};
  ti.cbSize = sizeof ti;
  if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator != 0)
    d.dwmRefreshHz = static_cast<double>(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator;
  UINT dx = 0, dy = 0;
  if (SUCCEEDED(GetDpiForMonitor(d.monitor, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx) d.dpi = dx;
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  if (auto rtlGetVersion = LoadProc<RtlGetVersionFn>(L"ntdll.dll", "RtlGetVersion")) {
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (rtlGetVersion(&v) == 0)
      d.os = std::format("{}.{}.{}", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
  }
  const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
  d.perMonitorV2 = AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
  if (d.perMonitorV2) {
    d.dpiAwareness = "per-monitor v2";
  } else {
    switch (GetAwarenessFromDpiAwarenessContext(ctx)) {
      case DPI_AWARENESS_PER_MONITOR_AWARE: d.dpiAwareness = "per-monitor v1"; break;
      case DPI_AWARENESS_SYSTEM_AWARE: d.dpiAwareness = "system aware"; break;
      case DPI_AWARENESS_UNAWARE: d.dpiAwareness = "unaware"; break;
      default: d.dpiAwareness = "unknown"; break;
    }
  }
  return d;
}

double CaptureRefreshHz(const DisplayInfo& d) {
  if (d.refreshHz > 1) return static_cast<double>(d.refreshHz);
  if (d.dwmRefreshHz > 1) return d.dwmRefreshHz;
  return 60.0;
}

// ---- windows ----------------------------------------------------------------

HBRUSH BlueBrush() {
  static HBRUSH b = CreateSolidBrush(RGB(0, 0, 255));
  return b;
}
HBRUSH BlackBrush() { return static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); }
HBRUSH WhiteBrush() { return static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)); }
HBRUSH TickerBrush() {
  static HBRUSH b = CreateSolidBrush(RGB(24, 24, 24));
  return b;
}

LRESULT PaintSolid(HWND hwnd, UINT msg, WPARAM wp, HBRUSH brush) {
  if (msg == WM_ERASEBKGND) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
    return 1;
  }
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  FillRect(dc, &ps.rcPaint, brush);
  EndPaint(hwnd, &ps);
  return 0;
}

LRESULT CALLBACK TargetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
    case WM_PAINT: return PaintSolid(hwnd, msg, wp, BlueBrush());
    case WM_LBUTTONDOWN: g_targetClicks.fetch_add(1); return 0;
    case WM_CLOSE: return 0;  // only the harness destroys its windows
    default: return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
    case WM_PAINT: return PaintSolid(hwnd, msg, wp, BlackBrush());
    case WM_CLOSE: return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

LRESULT CALLBACK FramedProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
    case WM_PAINT: return PaintSolid(hwnd, msg, wp, WhiteBrush());
    default: return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

bool RegisterClasses() {
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  struct Def {
    const wchar_t* name;
    WNDPROC proc;
  };
  const Def defs[] = {{kTargetClass, TargetProc},
                      {kBackdropClass, BackdropProc},
                      {kFramedClass, FramedProc},
                      {kCanvasClass, BackdropProc},
                      {kFullscreenClass, BackdropProc}};
  for (const Def& d : defs) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = d.proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = d.name;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  }
  return true;
}

std::string DescribeWindow(HWND h) {
  if (!h) return "none";
  wchar_t cls[256] = {};
  GetClassNameW(h, cls, 256);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  return std::format("{} (pid {})", Utf8(cls), pid);
}

// ---- mover thread -------------------------------------------------------------

struct MoveSample {
  int64_t qpc = 0;
  int x = 0;
  int y = 0;
};

uint64_t PackPoint(SweepPoint p) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 32) | static_cast<uint32_t>(p.y);
}
SweepPoint UnpackPoint(uint64_t v) {
  SweepPoint p;
  p.x = static_cast<int32_t>(static_cast<uint32_t>(v >> 32));
  p.y = static_cast<int32_t>(static_cast<uint32_t>(v & 0xffffffffu));
  return p;
}

// Owns the backdrop, the target and the framed test window, pumps their
// messages and moves the target from its own thread, paced by a waitable timer.
class Mover {
 public:
  ~Mover() { Stop(); }

  bool Start(const RECT& monitor, const SweepArea& area, std::string* err) {
    monitor_ = monitor;
    area_ = area;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sweepDone_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    started_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    highResTimer_ = timer_ != nullptr;
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!wake_ || !sweepDone_ || !started_ || !timer_) {
      *err = "failed to create mover synchronisation objects";
      return false;
    }
    thread_ = std::thread([this] { ThreadMain(); });
    WaitForSingleObject(started_, INFINITE);
    if (!startOk_) {
      *err = startError_;
      Stop();
      return false;
    }
    return true;
  }

  void Stop() {
    if (thread_.joinable()) {
      // A thread that failed to start has already returned: never Invoke into it.
      if (startOk_) {
        Invoke([this] {
          if (target_) DestroyWindow(target_);
          if (backdrop_) DestroyWindow(backdrop_);
          target_ = backdrop_ = nullptr;
          quit_ = true;
        });
      }
      thread_.join();
    }
    for (HANDLE* h : {&wake_, &sweepDone_, &started_, &timer_}) {
      if (*h) CloseHandle(*h);
      *h = nullptr;
    }
  }

  bool started() const { return startOk_; }
  bool highResTimer() const { return highResTimer_; }
  HWND target() const { return target_; }
  HANDLE sweepDone() const { return sweepDone_; }
  SweepPoint Latest() const { return UnpackPoint(latest_.load()); }

  // Runs fn on the mover thread and waits for it.
  void Invoke(const std::function<void()>& fn) {
    if (GetCurrentThreadId() == threadId_) {
      fn();
      return;
    }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      queue_.push_back([&fn, done] {
        fn();
        SetEvent(done);
      });
    }
    SetEvent(wake_);
    WaitForSingleObject(done, INFINITE);
    CloseHandle(done);
  }

  void MoveTo(SweepPoint p) {
    Invoke([this, p] {
      sweepActive_ = false;
      Apply(p, Qpc(), false);
    });
  }

  // Puts the backdrop and the target back on top of other windows (z-order
  // changes need no foreground rights).
  void Raise() {
    Invoke([this] {
      SetWindowPos(backdrop_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(target_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    });
  }

  // Starts a sweep on the mover thread; returns its start time (QPC).
  int64_t BeginSweep(SweepKind kind, int rateHz, double scale) {
    int64_t start = 0;
    Invoke([&] {
      kind_ = kind;
      scale_ = scale;
      durationSec_ = SweepDurationSec(kind, scale);
      period_ = std::max<int64_t>(1, g_qpcFreq / rateHz);
      {
        std::lock_guard<std::mutex> lock(movesMutex_);
        moves_.clear();
      }
      ResetEvent(sweepDone_);
      start_ = Qpc();
      sweepActive_ = true;
      Step();
      start = start_;
    });
    return start;
  }

  std::vector<MoveSample> TakeMovements() {
    std::lock_guard<std::mutex> lock(movesMutex_);
    return std::move(moves_);
  }

  HWND CreateFramed(int x, int y, int w, int h, DWORD* error) {
    HWND hwnd = nullptr;
    Invoke([&] {
      // Shown without activation: an inactive caption is light on Windows 10,
      // an active one takes the (possibly dark) accent colour.
      hwnd = CreateWindowExW(0, kFramedClass, L"SnowHarness invisible-border check", WS_OVERLAPPEDWINDOW, x, y, w, h,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      *error = hwnd ? 0 : GetLastError();  // per-thread: read it on the mover thread
      if (!hwnd) return;
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      // Windows 11 (22000+): a dark accent caption/border could fall below the
      // brightness threshold; force them white. Geometry is unaffected; ignored on Windows 10.
      const COLORREF white = RGB(255, 255, 255);
      DwmSetWindowAttribute(hwnd, kDwmaBorderColor, &white, sizeof white);
      DwmSetWindowAttribute(hwnd, kDwmaCaptionColor, &white, sizeof white);
      UpdateWindow(hwnd);
    });
    return hwnd;
  }

  void DestroyWindowSync(HWND hwnd) {
    Invoke([hwnd] {
      if (IsWindow(hwnd)) DestroyWindow(hwnd);
    });
  }

 private:
  void ThreadMain() {
    threadId_ = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the message queue
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!RegisterClasses()) {
      startError_ = std::format("RegisterClassExW failed (error {})", GetLastError());
      SetEvent(started_);
      return;
    }
    const int mw = monitor_.right - monitor_.left;
    const int mh = monitor_.bottom - monitor_.top;
    backdrop_ = CreateWindowExW(WS_EX_TOOLWINDOW, kBackdropClass, L"SnowHarness backdrop", WS_POPUP | WS_VISIBLE,
                                monitor_.left, monitor_.top, mw, mh, nullptr, nullptr, inst, nullptr);
    const SweepPoint c{area_.CenterX(), area_.CenterY()};
    target_ = CreateWindowExW(0, kTargetClass, L"SnowHarness target", WS_POPUP | WS_VISIBLE, c.x, c.y, kTargetW,
                              kTargetH, nullptr, nullptr, inst, nullptr);
    if (!backdrop_ || !target_) {
      startError_ = std::format("CreateWindowExW failed (error {})", GetLastError());
      if (target_) DestroyWindow(target_);
      if (backdrop_) DestroyWindow(backdrop_);
      target_ = backdrop_ = nullptr;
      SetEvent(started_);
      return;
    }
    // Z-order changes are not subject to the foreground lock, activation is:
    // raise both windows first, then try (best effort) to activate the target.
    SetWindowPos(backdrop_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(target_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(target_);
    UpdateWindow(backdrop_);
    UpdateWindow(target_);
    last_ = c;
    latest_.store(PackPoint(c));
    startOk_ = true;
    SetEvent(started_);

    while (!quit_) {
      HANDLE handles[2] = {wake_, timer_};
      const DWORD r = MsgWaitForMultipleObjectsEx(2, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      if (r == WAIT_OBJECT_0) {
        RunQueue();
      } else if (r == WAIT_OBJECT_0 + 1) {
        Step();
      }
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }

  void RunQueue() {
    for (;;) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.empty()) return;
        fn = std::move(queue_.front());
        queue_.pop_front();
      }
      fn();
    }
  }

  void Step() {
    if (!sweepActive_) return;
    const int64_t now = Qpc();
    const double t = static_cast<double>(now - start_) / static_cast<double>(g_qpcFreq);
    if (t >= durationSec_) {
      sweepActive_ = false;
      SetEvent(sweepDone_);
      return;
    }
    Apply(SweepPosition(kind_, area_, t, scale_), now, true);
    FlipTicker();
    // Next deadline on the fixed grid; late steps are dropped, never bunched.
    const int64_t next = start_ + ((now - start_) / period_ + 1) * period_;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, (next - Qpc()) * 10000000 / g_qpcFreq);
    SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
  }

  void Apply(SweepPoint p, int64_t now, bool record) {
    if (p.x != last_.x || p.y != last_.y) {
      SetWindowPos(target_, nullptr, p.x, p.y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      last_ = p;
    }
    latest_.store(PackPoint(p));
    if (record) {
      std::lock_guard<std::mutex> lock(movesMutex_);
      moves_.push_back({now, p.x, p.y});
    }
  }

  // Repaints a 4x4 patch in the monitor's top-right corner every step so DWM
  // composes (and Desktop Duplication delivers) a frame even when the target
  // and the overlay are still. Far from every ROI scan line.
  void FlipTicker() {
    HDC dc = GetDC(backdrop_);
    if (!dc) return;
    const int w = monitor_.right - monitor_.left;
    RECT r{w - 4, 0, w, 4};
    FillRect(dc, &r, tickerOn_ ? TickerBrush() : BlackBrush());
    ReleaseDC(backdrop_, dc);
    tickerOn_ = !tickerOn_;
  }

  std::thread thread_;
  std::atomic<DWORD> threadId_{0};
  HANDLE wake_ = nullptr;
  HANDLE sweepDone_ = nullptr;
  HANDLE started_ = nullptr;
  HANDLE timer_ = nullptr;
  bool highResTimer_ = false;
  bool startOk_ = false;
  std::string startError_;
  std::mutex queueMutex_;
  std::deque<std::function<void()>> queue_;
  std::atomic<uint64_t> latest_{0};
  std::mutex movesMutex_;
  std::vector<MoveSample> moves_;

  // Mover-thread state.
  RECT monitor_{};
  SweepArea area_{};
  HWND backdrop_ = nullptr;
  HWND target_ = nullptr;
  bool quit_ = false;
  bool sweepActive_ = false;
  bool tickerOn_ = false;
  SweepKind kind_ = SweepKind::Still;
  double scale_ = 1.0;
  double durationSec_ = 0;
  int64_t start_ = 0;
  int64_t period_ = 1;
  SweepPoint last_{};
};

// ---- overlay process ----------------------------------------------------------

class Overlay {
 public:
  ~Overlay() { Kill(); }

  bool Launch(const fs::path& exe, const Mode& m, const fs::path& telemetry, const fs::path& log, HANDLE job,
              std::string* err) {
    std::wstring args = L" --harness --instance harness --sync ";
    args += m.vsync ? L"vsync" : L"latch";
    args += L" --predict ";
    args += m.predict ? L"on" : L"off";
    args += L" --predictor ";
    args += m.stepped ? L"stepped" : L"linear";
    if (m.marginMs >= 0) args += L" --latch-margin " + std::to_wstring(m.marginMs);
    args += L" --telemetry " + QuoteArg(telemetry.native());
    args += L" --log " + QuoteArg(log.native());
    return Start(exe, args, job, err);
  }

  // Ordinary snowfall with default settings: no --harness, so normal pacing and
  // fullscreen handling apply. Same instance name, so the same quit event works.
  bool LaunchPlain(const fs::path& exe, const fs::path& log, HANDLE job, std::string* err) {
    return Start(exe, L" --instance harness --no-persist --log " + QuoteArg(log.native()), job, err);
  }

 private:
  bool Start(const fs::path& exe, const std::wstring& args, HANDLE job, std::string* err) {
    std::wstring cmd = QuoteArg(exe.native()) + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    const std::wstring dir = exe.parent_path().native();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi_)) {
      *err = std::format("CreateProcessW failed (error {})", GetLastError());
      pi_ = PROCESS_INFORMATION{};
      return false;
    }
    if (job) AssignProcessToJobObject(job, pi_.hProcess);  // dies with the harness
    ResumeThread(pi_.hThread);
    commandLine_ = Utf8(cmd);
    return true;
  }

 public:
  enum class Ready { Ok, Timeout, Exited };
  Ready WaitReady(HANDLE readyEvent, DWORD timeoutMs) {
    HANDLE hs[2] = {readyEvent, pi_.hProcess};
    const DWORD r = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
    if (r == WAIT_OBJECT_0) return Ready::Ok;
    if (r == WAIT_OBJECT_0 + 1) {
      GetExitCodeProcess(pi_.hProcess, &exitCode_);
      return Ready::Exited;
    }
    return Ready::Timeout;
  }

  // Signals the quit event, waits, then terminates as a last resort.
  void Stop(HANDLE quitEvent, DWORD timeoutMs) {
    if (!pi_.hProcess) return;
    SetEvent(quitEvent);
    if (WaitForSingleObject(pi_.hProcess, timeoutMs) != WAIT_OBJECT_0) {
      terminated_ = true;
      TerminateProcess(pi_.hProcess, 0xDEAD);
      WaitForSingleObject(pi_.hProcess, 2000);
    }
    GetExitCodeProcess(pi_.hProcess, &exitCode_);
    Close();
  }

  void Kill() {
    if (!pi_.hProcess) return;
    if (WaitForSingleObject(pi_.hProcess, 0) != WAIT_OBJECT_0) {
      terminated_ = true;
      TerminateProcess(pi_.hProcess, 0xDEAD);
      WaitForSingleObject(pi_.hProcess, 2000);
    }
    GetExitCodeProcess(pi_.hProcess, &exitCode_);
    Close();
  }

  bool Exited() const { return pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_OBJECT_0; }
  HANDLE process() const { return pi_.hProcess; }
  DWORD pid() const { return pi_.dwProcessId; }
  DWORD exitCode() const { return exitCode_; }
  bool terminated() const { return terminated_; }
  const std::string& commandLine() const { return commandLine_; }

 private:
  void Close() {
    if (pi_.hThread) CloseHandle(pi_.hThread);
    if (pi_.hProcess) CloseHandle(pi_.hProcess);
    pi_.hThread = pi_.hProcess = nullptr;
  }

  PROCESS_INFORMATION pi_{};
  DWORD exitCode_ = 0;
  bool terminated_ = false;
  std::string commandLine_;
};

// ---- results ------------------------------------------------------------------

struct ForegroundMonitor {
  DWORD overlayPid = 0;
  int samples = 0;
  int overlaySamples = 0;
  void Sample() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    ++samples;
    if (overlayPid && pid == overlayPid) ++overlaySamples;
  }
};

struct SweepResult {
  int rate = 0;
  SweepKind sweep = SweepKind::Still;
  double durationMs = 0;
  int moverSteps = 0;
  double moverRateHz = 0;
  SweepSummary s;
  bool pass = false;
  std::vector<std::string> failures;
};

struct ModeResult {
  Mode mode;
  bool launched = false;
  bool ready = false;
  double readyMs = 0;
  std::string error;
  DWORD exitCode = 0;
  bool terminated = false;
  ForegroundMonitor fg;
  std::vector<SweepResult> sweeps;
  bool pass = false;
  std::vector<std::string> failures;
};

struct BorderCheck {
  bool ran = false;
  bool valid = false;
  bool pass = false;
  RECT visible{};
  RECT windowRect{};
  RECT frameBounds{};
  bool frameBoundsOk = false;
  std::string note;
  int WrDelta(int side) const { return Side(windowRect, side) - Side(visible, side); }
  int EfbDelta(int side) const { return Side(frameBounds, side) - Side(visible, side); }
  static int Side(const RECT& r, int side) {
    switch (side) {
      case 0: return r.left;
      case 1: return r.top;
      case 2: return r.right;
      default: return r.bottom;
    }
  }
};

struct ClickCheck {
  bool ran = false;
  bool skipped = false;
  std::string skipReason;
  POINT point{};
  POINT cursorAfter{};
  UINT inputsSent = 0;
  bool clicked = false;
  double latencyMs = 0;
  bool windowFromPointIsTarget = false;
  std::string windowFromPoint;
  bool Pass() const { return ran && !skipped && clicked && windowFromPointIsTarget; }
};

struct FsPhase {
  double seconds = 0;
  int64_t frames = 0, presents = 0;
  double cpuPct = 0;  // overlay process, % of one core
  int captures = 0;
  int maxBright = 0;  // most snow pixels seen over the black test window in one capture
  int samples = 0, shownSamples = 0;  // the test monitor's overlay visible
  std::string topWindow;  // window under the middle of the measured area at the start
  CapturedFrame brightest;
};

struct FullscreenResult {
  bool ran = false;
  bool pass = false;
  std::string note;  // why it did not run
  FsPhase before, during, after;
  double hideMs = -1, showMs = -1;
  bool allHidden = false;  // every overlay hidden while the fake app was up
  std::vector<std::string> cases;  // extra pause cases: "name: PASS/FAIL - detail"
  std::vector<std::string> failures;
};

struct Context {
  Options opt;
  DisplayInfo disp;
  SweepArea area{};
  fs::path outDir;
  fs::path exe;
  std::string started;
  Mover mover;
  ScreenCapture cap;
  std::ofstream framesCsv;
  std::ofstream movesCsv;
  int dumpsLeft = 0;
  std::vector<ModeResult> modes;
  BorderCheck border;
  ClickCheck click;
  long long validTotal = 0;
  double scale = 1.0;
  POINT parkCursor{};
  std::vector<fs::path> overlayLogs;          // copied into the results file
  std::vector<std::string> snowfallStats;     // per-minute lines from the plain-snowfall phase
  std::string snowfallNote;                   // why that phase is missing or incomplete
  FullscreenResult fullscreen;
};

SweepPoint CenterPoint(const SweepArea& a) { return SweepPoint{a.CenterX(), a.CenterY()}; }

RECT RoiFor(SweepPoint a, SweepPoint b) {
  RECT r;
  r.left = std::min(a.x, b.x) - kRoiMargin;
  r.top = std::min(a.y, b.y) - kRoiMargin;
  r.right = std::max(a.x, b.x) + kTargetW + kRoiMargin;
  r.bottom = std::max(a.y, b.y) + kTargetH + kRoiMargin;
  return r;
}

bool WriteBmp(const fs::path& path, const CapturedFrame& f) {
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof ih;
  ih.biWidth = f.width;
  ih.biHeight = -f.height;
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = static_cast<DWORD>(f.pixels.size());
  fh.bfType = 0x4D42;  // "BM"
  fh.bfOffBits = sizeof fh + sizeof ih;
  fh.bfSize = fh.bfOffBits + ih.biSizeImage;
  std::ofstream o(path, std::ios::binary);
  if (!o) return false;
  o.write(reinterpret_cast<const char*>(&fh), sizeof fh);
  o.write(reinterpret_cast<const char*>(&ih), sizeof ih);
  o.write(reinterpret_cast<const char*>(f.pixels.data()), static_cast<std::streamsize>(f.pixels.size()));
  return static_cast<bool>(o);
}

std::string ExtraText(SweepKind k, const SweepSummary& s) {
  switch (k) {
    case SweepKind::Still: return std::format("distinct={} max|d|={}", s.distinctPositions, s.maxAbsD);
    case SweepKind::Stop: return std::format("off-after-stop={} settle={:.0f}ms", s.framesOffAfterStop, s.settleMs);
    case SweepKind::VSweep:
      return std::format("drift={} ({}->{})", JsonNumber(s.drift, 2), JsonNumber(s.firstWindowMeanDy, 2),
                         JsonNumber(s.lastWindowMeanDy, 2));
    default: return "";
  }
}

// ---- one sweep ------------------------------------------------------------------

struct FrameRecord {
  double tMs = 0;
  SweepPoint hint{};
  Detection det;
};

SweepResult RunSweep(Context& ctx, ModeResult& mr, int rate, SweepKind kind) {
  SweepResult res;
  res.rate = rate;
  res.sweep = kind;
  const double durSec = SweepDurationSec(kind, ctx.scale);
  res.durationMs = durSec * 1000.0;

  const SweepPoint p0 = SweepPosition(kind, ctx.area, 0, ctx.scale);
  ctx.mover.MoveTo(p0);
  SleepMs(sweep::kInterSweepHoldSec * ctx.scale * 1000.0);
  // Another window on top of the target (a notification, a window the user
  // clicked) hides the edges being measured: say so and raise the target again.
  const POINT centre{p0.x + kTargetW / 2, p0.y + kTargetH / 2};
  if (HWND at = WindowFromPoint(centre); at != ctx.mover.target()) {
    Printf("  WARNING: before {} Hz {} the test window was covered by {} - raised it again\n", rate, SweepName(kind),
           DescribeWindow(at));
    ctx.mover.Raise();
    SleepMs(200);
  }
  ctx.cap.ResetChangeDetection();

  const int64_t start = ctx.mover.BeginSweep(kind, rate, ctx.scale);
  std::vector<FrameRecord> records;
  SweepPoint prev = p0;
  CapturedFrame frame;
  while (WaitForSingleObject(ctx.mover.sweepDone(), 0) == WAIT_TIMEOUT) {
    const SweepPoint cur = ctx.mover.Latest();
    const RECT roi = RoiFor(prev, cur);
    prev = cur;
    const CaptureResult cr = ctx.cap.Next(roi, kAcquireTimeoutMs, frame);
    mr.fg.Sample();
    if (cr == CaptureResult::Error) Sleep(1);  // never spin on a persistent error
    if (cr != CaptureResult::Frame) continue;
    const double t = QpcToMs(frame.qpc - start);
    if (t < 0 || t > res.durationMs) continue;
    FrameRecord rec;
    rec.tMs = t;
    rec.hint = cur;
    rec.det = DetectOffsets(frame.pixels.data(), frame.width, frame.height, frame.stride, frame.originX, frame.originY,
                            cur.x, cur.y, kTargetW, kTargetH);
    if ((!rec.det.validX || !rec.det.validY) && ctx.dumpsLeft > 0) {
      --ctx.dumpsLeft;
      std::error_code ec;
      (void)fs::create_directories(ctx.outDir / "dumps", ec);
      WriteBmp(ctx.outDir / "dumps" /
                   Wide(std::format("{}-{}hz-{}-{:.0f}ms.bmp", mr.mode.name, rate, SweepName(kind), t)),
               frame);
    }
    records.push_back(rec);
  }
  const std::vector<MoveSample> moves = ctx.mover.TakeMovements();
  res.moverSteps = static_cast<int>(moves.size());
  res.moverRateHz = durSec > 0 ? res.moverSteps / durSec : 0;

  // Commanded position in effect at each frame's timestamp.
  std::vector<FrameSample> samples;
  samples.reserve(records.size());
  size_t mi = 0;
  SweepPoint cmd = p0;
  for (const FrameRecord& r : records) {  // records are in capture order (monotonic)
    while (mi < moves.size() && QpcToMs(moves[mi].qpc - start) <= r.tMs) {
      cmd = SweepPoint{moves[mi].x, moves[mi].y};
      ++mi;
    }
    const Detection& d = r.det;
    ctx.framesCsv << std::format("{},{},{},{:.3f},{},{},{},{},{},{},{},{},{},{}\n", mr.mode.name, rate, SweepName(kind),
                                 r.tMs, cmd.x, cmd.y, d.windowLeft, d.windowTop, d.outlineLeft, d.outlineTop, d.dx,
                                 d.dy, d.validX ? 1 : 0, d.validY ? 1 : 0);
    FrameSample s;
    s.tMs = r.tMs;
    s.validX = d.validX;
    s.validY = d.validY;
    s.dx = d.dx;
    s.dy = d.dy;
    s.outlineLeft = d.outlineLeft;
    s.outlineTop = d.outlineTop;
    samples.push_back(s);
  }
  for (const MoveSample& m : moves)
    ctx.movesCsv << std::format("{},{},{},{:.3f},{},{}\n", mr.mode.name, rate, SweepName(kind), QpcToMs(m.qpc - start),
                                m.x, m.y);
  ctx.framesCsv.flush();
  ctx.movesCsv.flush();

  const double stopMs = kind == SweepKind::Stop ? StopInstantSec(ctx.scale) * 1000.0 : -1.0;
  const double driftMs = kind == SweepKind::VSweep ? std::min(1000.0, res.durationMs / 2) : 0.0;
  res.s = Summarize(samples, res.durationMs, stopMs, driftMs);
  res.pass = EvaluateAcceptance(res.s, kind == SweepKind::Still, kind == SweepKind::VSweep, &res.failures);
  ctx.validTotal += res.s.validBoth;
  return res;
}

// ---- extra checks ---------------------------------------------------------------

BorderCheck RunBorderCheck(Context& ctx) {
  BorderCheck bc;
  bc.ran = true;
  const SweepPoint c = CenterPoint(ctx.area);
  ctx.mover.MoveTo(c);
  const int cx = c.x + kTargetW / 2;
  const int cy = c.y + kTargetH / 2;
  DWORD error = 0;
  HWND framed = ctx.mover.CreateFramed(cx - kFramedW / 2, cy - kFramedH / 2, kFramedW, kFramedH, &error);
  if (!framed) {
    bc.note = std::format("CreateWindowExW failed (error {})", error);
    return bc;
  }
  SleepMs(700);  // let it paint and DWM finish any show animation
  GetWindowRect(framed, &bc.windowRect);
  bc.frameBoundsOk =
      SUCCEEDED(DwmGetWindowAttribute(framed, DWMWA_EXTENDED_FRAME_BOUNDS, &bc.frameBounds, sizeof bc.frameBounds));
  RECT roi = bc.windowRect;
  InflateRect(&roi, 24, 24);
  CapturedFrame f;
  if (ctx.cap.GrabGdi(roi, f)) {
    const RECT& ref = bc.frameBoundsOk ? bc.frameBounds : bc.windowRect;
    // Threshold 12, not 40: the backdrop is pure black (shadows stay black), and
    // a dark title bar or the 1-px Windows 10 border must still count as visible.
    const BrightEdges e = DetectBrightEdges(f.pixels.data(), f.width, f.height, f.stride, f.originX, f.originY,
                                            (ref.left + ref.right) / 2, (ref.top + ref.bottom) / 2, 12);
    WriteBmp(ctx.outDir / "border-check.bmp", f);
    if (e.valid) {
      bc.valid = true;
      bc.visible = RECT{e.left, e.top, e.right, e.bottom};
      bc.pass = bc.frameBoundsOk;
      for (int side = 0; side < 4; ++side)
        if (std::abs(bc.EfbDelta(side)) > 1) bc.pass = false;
    } else {
      bc.note = "visible edges not found in the capture";
    }
  } else {
    bc.note = "GDI capture failed";
  }
  ctx.mover.DestroyWindowSync(framed);
  SleepMs(300);
  return bc;
}

ClickCheck RunClickCheck(Context& ctx, DWORD overlayPid) {
  ClickCheck cc;
  cc.ran = true;
  RECT wr{};
  GetWindowRect(ctx.mover.target(), &wr);
  cc.point = POINT{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
  HWND at = WindowFromPoint(cc.point);
  cc.windowFromPointIsTarget = at == ctx.mover.target();
  cc.windowFromPoint = DescribeWindow(at);
  DWORD pid = 0;
  if (at) GetWindowThreadProcessId(at, &pid);
  if (pid != GetCurrentProcessId() && pid != overlayPid) {
    // Never click into an unrelated application.
    cc.skipped = true;
    cc.skipReason = "a foreign window is at the click point: " + cc.windowFromPoint;
    return cc;
  }
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN));
  const int vh = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
  const LONG nx = static_cast<LONG>((static_cast<int64_t>(cc.point.x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
  const LONG ny = static_cast<LONG>((static_cast<int64_t>(cc.point.y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
  INPUT in[3] = {};
  const DWORD flags[3] = {MOUSEEVENTF_MOVE, MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN,
                          MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTUP};
  for (int i = 0; i < 3; ++i) {
    in[i].type = INPUT_MOUSE;
    in[i].mi.dx = nx;
    in[i].mi.dy = ny;
    in[i].mi.dwFlags = flags[i] | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  }
  const long before = g_targetClicks.load();
  const int64_t t0 = Qpc();
  cc.inputsSent = SendInput(3, in, sizeof(INPUT));
  while (QpcToMs(Qpc() - t0) < 1000.0) {
    if (g_targetClicks.load() > before) {
      cc.clicked = true;
      cc.latencyMs = QpcToMs(Qpc() - t0);
      break;
    }
    Sleep(2);
  }
  GetCursorPos(&cc.cursorAfter);
  SetCursorPos(ctx.parkCursor.x, ctx.parkCursor.y);
  return cc;
}

// ---- reporting ------------------------------------------------------------------

struct Row {
  std::vector<std::string> cells;
};

std::vector<std::string> TableHeader() {
  return {"mode",    "rate", "sweep",   "frames",  "valid",  "dx mean", "dx p95", "dx max", "dx bias",
          "dx<=1%",  "dy mean", "dy p95", "dy max", "dy bias", "dy<=1%", "extra",  "result"};
}

std::vector<Row> TableRows(const Context& ctx) {
  std::vector<Row> rows;
  for (const ModeResult& m : ctx.modes) {
    for (const SweepResult& r : m.sweeps) {
      const SweepSummary& s = r.s;
      Row row;
      row.cells = {m.mode.name,
                   std::to_string(r.rate),
                   SweepName(r.sweep),
                   std::to_string(s.frames),
                   std::to_string(s.validBoth),
                   std::format("{:.2f}", s.x.meanAbs),
                   std::format("{:.0f}", s.x.p95Abs),
                   std::format("{:.0f}", s.x.maxAbs),
                   std::format("{:+.2f}", s.x.bias),
                   std::format("{:.1f}", s.x.pctWithin1),
                   std::format("{:.2f}", s.y.meanAbs),
                   std::format("{:.0f}", s.y.p95Abs),
                   std::format("{:.0f}", s.y.maxAbs),
                   std::format("{:+.2f}", s.y.bias),
                   std::format("{:.1f}", s.y.pctWithin1),
                   ExtraText(r.sweep, s),
                   r.pass ? "PASS" : "FAIL"};
      if (s.validX == 0) {
        for (int i = 5; i <= 9; ++i) row.cells[static_cast<size_t>(i)] = "-";
      }
      if (s.validY == 0) {
        for (int i = 10; i <= 14; ++i) row.cells[static_cast<size_t>(i)] = "-";
      }
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

std::string TextTable(const Context& ctx) {
  const std::vector<std::string> header = TableHeader();
  const std::vector<Row> rows = TableRows(ctx);
  std::vector<size_t> width(header.size());
  for (size_t i = 0; i < header.size(); ++i) width[i] = header[i].size();
  for (const Row& r : rows)
    for (size_t i = 0; i < r.cells.size(); ++i) width[i] = std::max(width[i], r.cells[i].size());
  auto line = [&](const std::vector<std::string>& cells) {
    std::string o;
    for (size_t i = 0; i < cells.size(); ++i) {
      const bool left = i < 3 || i >= 15;
      const std::string pad(width[i] - cells[i].size(), ' ');
      o += left ? cells[i] + pad : pad + cells[i];
      o += i + 1 < cells.size() ? "  " : "\n";
    }
    return o;
  };
  std::string o = line(header);
  for (const Row& r : rows) o += line(r.cells);
  return o;
}

bool OverallPass(const Context& ctx) {
  if (ctx.modes.empty()) return false;
  for (const ModeResult& m : ctx.modes)
    if (!m.pass) return false;
  return true;
}

bool ForegroundEverOverlay(const Context& ctx) {
  for (const ModeResult& m : ctx.modes)
    if (m.fg.overlaySamples > 0) return true;
  return false;
}

std::string BorderSummary(const BorderCheck& b) {
  if (!b.ran) return "not run";
  if (!b.valid) return "FAILED TO MEASURE: " + b.note;
  return std::format(
      "{} - visible {}; GetWindowRect {} (delta L{:+} T{:+} R{:+} B{:+}); extended frame bounds {} (delta L{:+} "
      "T{:+} R{:+} B{:+})",
      b.pass ? "PASS" : "FAIL", RectText(b.visible), RectText(b.windowRect), b.WrDelta(0), b.WrDelta(1), b.WrDelta(2),
      b.WrDelta(3), b.frameBoundsOk ? RectText(b.frameBounds) : std::string("unavailable"), b.EfbDelta(0),
      b.EfbDelta(1), b.EfbDelta(2), b.EfbDelta(3));
}

std::string ClickSummary(const ClickCheck& c) {
  if (!c.ran) return "not run";
  if (c.skipped) return "SKIPPED: " + c.skipReason;
  return std::format("{} - click at ({},{}) {}; WindowFromPoint = {}{}; cursor landed at ({},{}); inputs sent {}",
                     c.Pass() ? "PASS" : "FAIL", c.point.x, c.point.y,
                     c.clicked ? std::format("reached the test window after {:.1f} ms", c.latencyMs)
                               : std::string("did NOT reach the test window within 1 s"),
                     c.windowFromPoint, c.windowFromPointIsTarget ? " (test window)" : " (NOT the test window)",
                     c.cursorAfter.x, c.cursorAfter.y, c.inputsSent);
}

std::string AxisJson(const AxisStats& a) {
  return std::format(
      "{{\"count\": {}, \"mean_abs\": {}, \"p95_abs\": {}, \"max_abs\": {}, \"bias\": {}, \"pct_within_1\": {}}}",
      a.count, JsonNumber(a.meanAbs), a.count ? JsonNumber(a.p95Abs, 0) : "null", JsonNumber(a.maxAbs, 0),
      JsonNumber(a.bias), JsonNumber(a.pctWithin1, 2));
}

std::string BuildJson(const Context& ctx, int exitCode) {
  const DisplayInfo& d = ctx.disp;
  std::string j = "{\n";
  j += "  \"tool\": \"SnowHarness\",\n  \"format_version\": 1,\n";
  j += std::format("  \"started_local\": {},\n", JsonString(ctx.started));
  j += std::format("  \"overlay_exe\": {},\n", JsonString(PathUtf8(ctx.exe)));
  j += std::format("  \"quick\": {},\n  \"strict\": {},\n", ctx.opt.quick, ctx.opt.strict);
  j += std::format("  \"capture_backend\": {},\n  \"capture_description\": {},\n  \"capture_access_lost\": {},\n",
                   JsonString(CaptureBackendName(ctx.cap.backend())), JsonString(ctx.cap.description()),
                   ctx.cap.accessLostCount());
  j += std::format("  \"mover_timer\": {},\n",
                   JsonString(!ctx.mover.started()     ? "not started"
                              : ctx.mover.highResTimer() ? "high-resolution"
                                                         : "standard+timeBeginPeriod(1)"));
  j += std::format(
      "  \"display\": {{\"device\": {}, \"monitor_rect\": {}, \"work_rect\": {}, \"width\": {}, \"height\": {}, "
      "\"refresh_hz\": {}, \"dwm_refresh_hz\": {}, \"dpi\": {}, \"scale_percent\": {}, \"os_version\": {}, "
      "\"per_monitor_v2\": {}, \"dpi_awareness\": {}}},\n",
      JsonString(d.device), JsonRect(d.rcMonitor), JsonRect(d.rcWork), d.width, d.height, d.refreshHz,
      JsonNumber(d.dwmRefreshHz), d.dpi, d.dpi * 100 / 96, JsonString(d.os), d.perMonitorV2,
      JsonString(d.dpiAwareness));
  j += std::format(
      "  \"target\": {{\"width\": {}, \"height\": {}, \"sweep_area\": {{\"min_x\": {}, \"min_y\": {}, \"max_x\": {}, "
      "\"max_y\": {}}}}},\n",
      kTargetW, kTargetH, ctx.area.minX, ctx.area.minY, ctx.area.maxX, ctx.area.maxY);

  const BorderCheck& b = ctx.border;
  j += "  \"checks\": {\n";
  j += std::format(
      "    \"invisible_border\": {{\"ran\": {}, \"valid\": {}, \"pass\": {}, \"visible\": {}, \"window_rect\": {}, "
      "\"extended_frame_bounds\": {}, \"window_rect_minus_visible\": [{}, {}, {}, {}], "
      "\"efb_minus_visible\": [{}, {}, {}, {}], \"note\": {}}},\n",
      b.ran, b.valid, b.pass, JsonRect(b.visible), JsonRect(b.windowRect),
      b.frameBoundsOk ? JsonRect(b.frameBounds) : "null", b.WrDelta(0), b.WrDelta(1), b.WrDelta(2), b.WrDelta(3),
      b.EfbDelta(0), b.EfbDelta(1), b.EfbDelta(2), b.EfbDelta(3), JsonString(b.note));
  const ClickCheck& c = ctx.click;
  j += std::format(
      "    \"click_through\": {{\"ran\": {}, \"pass\": {}, \"skipped\": {}, \"skip_reason\": {}, \"point\": [{}, {}], "
      "\"cursor_after\": [{}, {}], \"inputs_sent\": {}, \"clicked\": {}, \"latency_ms\": {}, "
      "\"window_from_point_is_target\": {}, \"window_from_point\": {}}}\n",
      c.ran, c.Pass(), c.skipped, JsonString(c.skipReason), c.point.x, c.point.y, c.cursorAfter.x, c.cursorAfter.y,
      c.inputsSent, c.clicked, c.clicked ? JsonNumber(c.latencyMs, 1) : "null", c.windowFromPointIsTarget,
      JsonString(c.windowFromPoint));
  j += "  },\n";
  j += std::format("  \"foreground_ever_overlay\": {},\n", ForegroundEverOverlay(ctx));
  j += std::format("  \"valid_frames_total\": {},\n", ctx.validTotal);

  j += "  \"modes\": [";
  for (size_t mi = 0; mi < ctx.modes.size(); ++mi) {
    const ModeResult& m = ctx.modes[mi];
    j += mi ? ",\n    {" : "\n    {";
    j += std::format(
        "\"mode\": {}, \"sync\": {}, \"predict\": {}, \"launched\": {}, \"ready\": {}, \"ready_ms\": {}, "
        "\"error\": {}, \"exit_code\": {}, \"terminated\": {}, \"foreground_samples\": {}, "
        "\"foreground_overlay_samples\": {}, \"pass\": {}, \"failures\": {}, \"results\": [",
        JsonString(m.mode.name), JsonString(m.mode.vsync ? "vsync" : "latch"), m.mode.predict, m.launched, m.ready,
        JsonNumber(m.readyMs, 1), JsonString(m.error), m.exitCode, m.terminated, m.fg.samples, m.fg.overlaySamples,
        m.pass, JsonStringArray(m.failures));
    for (size_t ri = 0; ri < m.sweeps.size(); ++ri) {
      const SweepResult& r = m.sweeps[ri];
      const SweepSummary& s = r.s;
      j += ri ? ",\n      {" : "\n      {";
      j += std::format(
          "\"rate\": {}, \"sweep\": {}, \"duration_ms\": {}, \"mover_steps\": {}, \"mover_rate_hz\": {}, "
          "\"frames\": {}, \"valid\": {}, \"invalid\": {}, \"valid_x\": {}, \"valid_y\": {}, \"dx\": {}, \"dy\": {}",
          r.rate, JsonString(SweepName(r.sweep)), JsonNumber(r.durationMs, 0), r.moverSteps,
          JsonNumber(r.moverRateHz, 1), s.frames, s.validBoth, s.invalid, s.validX, s.validY, AxisJson(s.x),
          AxisJson(s.y));
      if (r.sweep == SweepKind::Still)
        j += std::format(", \"still\": {{\"distinct_positions\": {}, \"max_abs_d\": {}}}", s.distinctPositions,
                         s.maxAbsD);
      if (r.sweep == SweepKind::Stop)
        j += std::format(", \"stop\": {{\"stop_instant_ms\": {}, \"frames_off_after_stop\": {}, \"settle_ms\": {}}}",
                         JsonNumber(StopInstantSec(ctx.scale) * 1000.0, 0), s.framesOffAfterStop,
                         JsonNumber(s.settleMs, 1));
      if (r.sweep == SweepKind::VSweep)
        j += std::format(
            ", \"drift\": {{\"first_mean_dy\": {}, \"first_n\": {}, \"last_mean_dy\": {}, \"last_n\": {}, "
            "\"drift_px\": {}}}",
            JsonNumber(s.firstWindowMeanDy), s.firstWindowN, JsonNumber(s.lastWindowMeanDy), s.lastWindowN,
            JsonNumber(s.drift));
      j += std::format(", \"pass\": {}, \"failures\": {}}}", r.pass, JsonStringArray(r.failures));
    }
    j += m.sweeps.empty() ? "]}" : "\n    ]}";
  }
  j += ctx.modes.empty() ? "],\n" : "\n  ],\n";
  j += std::format(
      "  \"acceptance\": {{\"pass\": {}, \"limits\": {{\"p95_abs_max_px\": 1, \"still_distinct_positions\": 1, "
      "\"still_max_abs_d_px\": 1, \"drift_max_px\": 0.5}}}},\n",
      OverallPass(ctx));
  const FullscreenResult& fr = ctx.fullscreen;
  j += std::format(
      "  \"fullscreen\": {{\"ran\": {}, \"pass\": {}, \"hide_ms\": {}, \"show_ms\": {}, \"cpu_before_pct\": {}, "
      "\"cpu_during_pct\": {}, \"presents_before\": {}, \"presents_during\": {}, \"presents_after\": {}, "
      "\"snow_px_before\": {}, \"snow_px_during\": {}, \"snow_px_after\": {}}},\n",
      fr.ran, fr.pass, JsonNumber(fr.hideMs, 1), JsonNumber(fr.showMs, 1), JsonNumber(fr.before.cpuPct, 2),
      JsonNumber(fr.during.cpuPct, 2), fr.before.presents, fr.during.presents, fr.after.presents, fr.before.maxBright,
      fr.during.maxBright, fr.after.maxBright);
  j += std::format("  \"exit_code\": {}\n}}\n", exitCode);
  return j;
}

std::string BuildMarkdown(const Context& ctx, int exitCode) {
  const DisplayInfo& d = ctx.disp;
  std::string m = "# SnowHarness results\n\n";
  m += std::format("- Started: {}\n", ctx.started);
  m += std::format("- OS: Windows {}\n", d.os.empty() ? "?" : d.os);
  m += std::format("- Test monitor: {} {}x{} at {}, work area {}\n", d.device, d.width, d.height,
                   RectText(d.rcMonitor), RectText(d.rcWork));
  m += std::format("- Refresh: {} Hz (display settings), {} Hz (DWM); DPI {} ({}%)\n", d.refreshHz,
                   JsonNumber(d.dwmRefreshHz, 3), d.dpi, d.dpi * 100 / 96);
  m += std::format("- Capture: {}\n", ctx.cap.description());
  m += std::format("- Mover timer: {}\n", !ctx.mover.started()     ? "not started"
                                          : ctx.mover.highResTimer() ? "high-resolution waitable timer"
                                                                     : "waitable timer + timeBeginPeriod(1)");
  m += std::format("- Overlay: `{}`\n", PathUtf8(ctx.exe));
  m += std::format("- Quick: {}; overall acceptance: **{}**; exit code {}\n\n", ctx.opt.quick ? "yes" : "no",
                   OverallPass(ctx) ? "PASS" : "FAIL", exitCode);
  m += "## Checks\n\n";
  m += "- Invisible border: " + BorderSummary(ctx.border) + "\n";
  m += "- Click-through: " + ClickSummary(ctx.click) + "\n";
  m += std::format("- Overlay ever foreground: {}\n\n", ForegroundEverOverlay(ctx) ? "**YES**" : "no");

  m += "## Modes\n\n";
  for (const ModeResult& mr : ctx.modes) {
    m += std::format("- `{}`: {}", mr.mode.name,
                     !mr.ready ? "overlay not ready (" + mr.error + ")" : (mr.pass ? "PASS" : "FAIL"));
    if (mr.ready) m += std::format(", ready after {:.0f} ms", mr.readyMs);
    if (mr.terminated) m += ", had to be terminated";
    m += "\n";
    for (const std::string& f : mr.failures) m += "  - " + f + "\n";
  }
  m += "\n## Results\n\n";
  m += "Offsets in px: 0 = aligned, positive dy = overlay drawn lower than the real edge, positive dx = to the right. "
       "p95 is nearest-rank. Acceptance per mode: p95 |dx| and |dy| <= 1 in every sweep, still: one distinct "
       "position and max |d| <= 1, vsweep drift <= 0.5 px.\n\n";
  const std::vector<std::string> header = TableHeader();
  m += "|";
  for (const std::string& h : header) m += " " + h + " |";
  m += "\n|";
  for (size_t i = 0; i < header.size(); ++i) m += (i < 3 || i >= 15) ? " --- |" : " ---: |";
  m += "\n";
  for (const Row& r : TableRows(ctx)) {
    m += "|";
    for (const std::string& c : r.cells) m += " " + c + " |";
    m += "\n";
  }
  return m;
}

void WriteText(const fs::path& path, const std::string& text) {
  std::ofstream o(path, std::ios::binary);
  o << text;
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
  }
  return lines;
}

// The overlay's once-a-minute frame statistics ("engine: last 60 s: ... fps ...").
std::vector<std::string> StatsLines(const fs::path& log) {
  std::vector<std::string> out;
  for (std::string& l : ReadLines(log))
    if (l.find("engine: last ") != std::string::npos) out.push_back(std::move(l));
  return out;
}

// Plain snowfall with nothing moving: the overlay's ordinary frame rate and
// frame-wait timeouts on this PC, from its per-minute log lines.
void RunSnowfall(Context& ctx, HANDLE quitEvent, HANDLE job) {
  const int sec = ctx.opt.snowfallSec;
  if (sec <= 0) {
    ctx.snowfallNote = "skipped (--snowfall 0)";
    return;
  }
  ctx.snowfallNote.clear();
  Printf("== Plain snowfall: {} s, nothing moving ==\n", sec);
  ctx.mover.Stop();   // remove the test windows: the overlay sees the normal desktop
  ctx.cap.Release();  // and no screen capture is running
  ResetEvent(quitEvent);
  const fs::path log = ctx.outDir / L"overlay-snowfall.log";
  std::error_code ec;
  fs::remove(log, ec);
  ctx.overlayLogs.push_back(log);
  Overlay ov;
  std::string err;
  if (!ov.LaunchPlain(ctx.exe, log, job, &err)) {
    ctx.snowfallNote = "the overlay did not start: " + err;
    Printf("ERROR: {}\n", ctx.snowfallNote);
    return;
  }
  Printf("  launched: {}\n", ov.commandLine());
  for (int s = 1; s <= sec; ++s) {
    SleepMs(1000);
    if (ov.Exited()) {
      ctx.snowfallNote = std::format("the overlay exited on its own after {} s", s);
      Printf("ERROR: {}\n", ctx.snowfallNote);
      break;
    }
    if (s % 30 == 0 && s < sec) Printf("  {} of {} s\n", s, sec);
  }
  ov.Stop(quitEvent, kQuitTimeoutMs);
  Printf("  overlay exited with code {}{}\n", ov.exitCode(), ov.terminated() ? " (terminated after timeout)" : "");
  ctx.snowfallStats = StatsLines(log);
  for (const std::string& l : ctx.snowfallStats) Printf("  overlay: {}\n", l);
}

// ---- fullscreen pause test ----------------------------------------------------------

// A thread that owns windows and pumps their messages; Invoke runs code on it.
class WindowThread {
 public:
  ~WindowThread() { Stop(); }

  bool Start() {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    started_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake_ || !started_) return false;
    thread_ = std::thread([this] { Main(); });
    WaitForSingleObject(started_, INFINITE);
    return ok_;
  }

  void Invoke(const std::function<void()>& fn) {
    if (!ok_) return;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back([&fn, done] {
        fn();
        SetEvent(done);
      });
    }
    SetEvent(wake_);
    WaitForSingleObject(done, INFINITE);
    CloseHandle(done);
  }

  void Stop() {
    if (thread_.joinable()) {
      Invoke([this] { quit_ = true; });
      thread_.join();
    }
    for (HANDLE* h : {&wake_, &started_}) {
      if (*h) CloseHandle(*h);
      *h = nullptr;
    }
    ok_ = false;
  }

 private:
  void Main() {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    ok_ = RegisterClasses();
    SetEvent(started_);
    if (!ok_) return;
    while (!quit_) {
      MsgWaitForMultipleObjectsEx(1, &wake_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      for (;;) {
        std::function<void()> fn;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (queue_.empty()) break;
          fn = std::move(queue_.front());
          queue_.pop_front();
        }
        fn();
      }
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }

  std::thread thread_;
  HANDLE wake_ = nullptr, started_ = nullptr;
  std::atomic<bool> ok_{false};
  bool quit_ = false;
  std::mutex mutex_;
  std::deque<std::function<void()>> queue_;
};

// The overlay's live counters (see src/core/shared_stats.h).
class StatsView {
 public:
  ~StatsView() {
    if (view_) UnmapViewOfFile(view_);
    if (map_) CloseHandle(map_);
  }
  bool Open() {
    if (view_) return true;
    map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kStatsMapName);
    if (!map_) return false;
    view_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, sizeof(snow::SharedStats));
    if (!view_) {
      CloseHandle(map_);
      map_ = nullptr;
    }
    return view_ != nullptr;
  }
  snow::SharedStats Read() const {
    snow::SharedStats s{};
    if (view_) std::memcpy(&s, view_, sizeof s);
    return s;
  }
  bool Shown(int idx) const {
    snow::SharedStats s = Read();
    return idx >= 0 && idx < static_cast<int>(s.overlays) && s.overlay[idx].shown != 0;
  }

 private:
  HANDLE map_ = nullptr;
  void* view_ = nullptr;
};

double CpuSeconds(HANDLE process) {
  FILETIME c, e, k, u;
  if (!process || !GetProcessTimes(process, &c, &e, &k, &u)) return 0;
  auto sec = [](const FILETIME& f) {
    return static_cast<double>((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
  };
  return sec(k) + sec(u);
}

// Watches the overlay for `seconds`: its counters, CPU time, whether the test
// monitor's overlay is visible, and the snow pixels captured over `roi`.
FsPhase MeasurePhase(Context& ctx, const StatsView& sv, HANDLE process, const RECT& roi, double seconds, int idx) {
  FsPhase ph;
  ph.topWindow = DescribeWindow(WindowFromPoint(POINT{(roi.left + roi.right) / 2, (roi.top + roi.bottom) / 2}));
  const snow::SharedStats s0 = sv.Read();
  const double cpu0 = CpuSeconds(process);
  const int64_t t0 = Qpc();
  ctx.cap.ResetChangeDetection();
  CapturedFrame f;
  auto consider = [&]() {
    ++ph.captures;
    const int n = CountBrightPixels(f.pixels.data(), f.width, f.height, f.stride);
    if (n > ph.maxBright || ph.brightest.pixels.empty()) {
      ph.maxBright = std::max(ph.maxBright, n);
      ph.brightest = f;
    }
  };
  while (QpcToMs(Qpc() - t0) < seconds * 1000.0) {
    const CaptureResult cr = ctx.cap.Next(roi, 50, f);
    if (cr == CaptureResult::Frame) {
      consider();
    } else if (cr == CaptureResult::Error) {
      Sleep(10);
    }
    ++ph.samples;
    if (sv.Shown(idx)) ++ph.shownSamples;
  }
  if (ctx.cap.GrabGdi(roi, f)) consider();  // a still screen gives Desktop Duplication nothing new
  const snow::SharedStats s1 = sv.Read();
  ph.seconds = QpcToMs(Qpc() - t0) / 1000.0;
  ph.frames = s1.frames - s0.frames;
  ph.presents = s1.presents - s0.presents;
  ph.cpuPct = ph.seconds > 0 ? (CpuSeconds(process) - cpu0) / ph.seconds * 100.0 : 0;
  return ph;
}

// Milliseconds until the overlay's visibility becomes `shown`, -1 on timeout.
double WaitShown(const StatsView& sv, int idx, bool shown, double timeoutMs) {
  const int64_t t0 = Qpc();
  while (QpcToMs(Qpc() - t0) < timeoutMs) {
    if (sv.Shown(idx) == shown) return QpcToMs(Qpc() - t0);
    Sleep(5);
  }
  return -1;
}

std::string PhaseText(const FsPhase& p) {
  return std::format("{:.1f} s: {} frames, {} presents, CPU {:.2f}% of a core, snow pixels up to {} ({} captures), "
                     "overlay visible in {}/{} samples, on top: {}",
                     p.seconds, p.frames, p.presents, p.cpuPct, p.maxBright, p.captures, p.shownSamples, p.samples,
                     p.topWindow);
}

// Plain overlay (normal mode, default settings) over a black canvas window; a
// borderless window then covers the whole test monitor like a game or an F11
// browser, and closes again. Rendering must stop (overlay hidden, no presents),
// the overlay's CPU use must drop, and the snow must come back afterwards.
void RunFullscreenTest(Context& ctx, HANDLE quitEvent, HANDLE job) {
  FullscreenResult& r = ctx.fullscreen;
  if (!ctx.opt.fullscreenTest) {
    r.note = "skipped (--fullscreen-test 0)";
    return;
  }
  Print("== Fullscreen pause: a fake fullscreen app covers the test monitor, then closes ==\n");
  r.ran = true;
  ctx.mover.Stop();  // its backdrop covers the whole monitor: it would itself count as a fullscreen app
  ResetEvent(quitEvent);
  const fs::path log = ctx.outDir / L"overlay-fullscreen.log";
  std::error_code ec;
  fs::remove(log, ec);
  ctx.overlayLogs.push_back(log);
  auto fail = [&](const std::string& why) { r.failures.push_back(why); };
  std::vector<std::string> caseFailures;  // reported after the main checks

  WindowThread wt;
  if (!wt.Start()) {
    fail("cannot create the test windows' thread");
    Printf("ERROR: {}\n", r.failures.back());
    return;
  }
  const RECT mon = ctx.disp.rcMonitor, work = ctx.disp.rcWork;
  const RECT cv{work.left + 60, work.top + 60, work.right - 60, work.bottom - 60};
  HWND canvas = nullptr, full = nullptr;
  wt.Invoke([&] {
    canvas = CreateWindowExW(0, kCanvasClass, L"SnowHarness canvas", WS_POPUP, cv.left, cv.top, cv.right - cv.left,
                             cv.bottom - cv.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!canvas) return;
    ShowWindow(canvas, SW_SHOWNOACTIVATE);
    SetWindowPos(canvas, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(canvas);
  });
  const int cx = static_cast<int>((cv.left + cv.right) / 2), cy = static_cast<int>((cv.top + cv.bottom) / 2);
  const int hw = std::max(40, std::min(450, static_cast<int>(cv.right - cv.left) / 2 - 40));
  const int hh = std::max(40, std::min(250, static_cast<int>(cv.bottom - cv.top) / 2 - 40));
  const RECT roi{cx - hw, cy - hh, cx + hw, cy + hh};

  Overlay ov;
  std::string err;
  if (!canvas || !ov.LaunchPlain(ctx.exe, log, job, &err)) {
    fail(canvas ? "the overlay did not start: " + err : "cannot create the canvas window");
    Printf("ERROR: {}\n", r.failures.back());
    wt.Invoke([&] {
      if (canvas) DestroyWindow(canvas);
    });
    return;
  }
  Printf("  launched: {}\n", ov.commandLine());
  StatsView sv;
  const int64_t t0 = Qpc();
  bool up = false;
  while (QpcToMs(Qpc() - t0) < 15000 && !ov.Exited()) {
    if (sv.Open() && sv.Read().presents > 0) {
      up = true;
      break;
    }
    Sleep(20);
  }
  int idx = -1;
  const snow::SharedStats s = sv.Read();
  for (uint32_t i = 0; i < s.overlays && i < static_cast<uint32_t>(snow::kSharedStatsMaxOverlays); ++i)
    if (s.overlay[i].left == mon.left && s.overlay[i].top == mon.top && s.overlay[i].right == mon.right &&
        s.overlay[i].bottom == mon.bottom)
      idx = static_cast<int>(i);
  if (!up || idx < 0) {
    fail(!up ? "the overlay never presented a frame (15 s)" : "no overlay covers the test monitor");
    Printf("ERROR: {}\n", r.failures.back());
  } else {
    SleepMs(3000);  // snow builds up; the start-up fade-in ends
    r.before = MeasurePhase(ctx, sv, ov.process(), roi, 3.0, idx);
    Printf("  before:  {}\n", PhaseText(r.before));

    wt.Invoke([&] {
      full = CreateWindowExW(0, kFullscreenClass, L"SnowHarness fake fullscreen app", WS_POPUP, mon.left, mon.top,
                             mon.right - mon.left, mon.bottom - mon.top, nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
      if (!full) return;
      ShowWindow(full, SW_SHOW);
      SetWindowPos(full, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      SetForegroundWindow(full);  // best effort; detection doesn't need it
      UpdateWindow(full);
    });
    if (!full) {
      fail("cannot create the fullscreen window");
    } else {
      r.hideMs = WaitShown(sv, idx, false, 3000);
      Printf("  fullscreen window up: overlay hidden after {}\n",
             r.hideMs < 0 ? std::string("- (still visible after 3 s)") : std::format("{:.0f} ms", r.hideMs));
      SleepMs(500);
      r.during = MeasurePhase(ctx, sv, ov.process(), roi, 3.0, idx);
      const snow::SharedStats sd = sv.Read();
      r.allHidden = sd.overlays > 0;
      for (uint32_t i = 0; i < sd.overlays && i < static_cast<uint32_t>(snow::kSharedStatsMaxOverlays); ++i)
        r.allHidden = r.allHidden && sd.overlay[i].shown == 0;
      Printf("  during:  {}\n", PhaseText(r.during));

      wt.Invoke([&] {
        DestroyWindow(full);
        full = nullptr;
      });
      r.showMs = WaitShown(sv, idx, true, 3000);
      Printf("  fullscreen window closed: overlay back after {}\n",
             r.showMs < 0 ? std::string("- (still hidden after 3 s)") : std::format("{:.0f} ms", r.showMs));
      SleepMs(1000);  // fade-in
      // Windows may activate (and raise) some other window when the fake app
      // closes; put the black canvas back on top so only snow can be bright.
      const POINT mid{(roi.left + roi.right) / 2, (roi.top + roi.bottom) / 2};
      if (HWND at = WindowFromPoint(mid); at != canvas) {
        Printf("  after closing, {} was on top of the canvas - raised the canvas again\n", DescribeWindow(at));
        wt.Invoke([&] { SetWindowPos(canvas, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); });
        SleepMs(300);
      }
      r.after = MeasurePhase(ctx, sv, ov.process(), roi, 3.0, idx);
      Printf("  after:   {}\n", PhaseText(r.after));

      // Real apps differ from the borderless test window: a window maximised with
      // the normal button, a fullscreen window a pixel or two short of the monitor
      // and an always-on-top fullscreen window must all hide the snow; a window
      // that almost fills the work area must not.
      struct PauseCase {
        const char* name;
        DWORD style, ex;
        RECT rc;
        bool maximize, expectHidden;
      };
      const PauseCase cases[] = {
          {"maximized window (normal maximize button)", WS_OVERLAPPEDWINDOW, 0, work, true, true},
          {"borderless fullscreen 2 px short", WS_POPUP, 0, RECT{mon.left, mon.top, mon.right - 2, mon.bottom - 2},
           false, true},
          {"always-on-top fullscreen window", WS_POPUP, WS_EX_TOPMOST, mon, false, true},
          {"window 24 px short of the work area", WS_POPUP, 0, RECT{work.left, work.top, work.right, work.bottom - 24},
           false, false},
      };
      for (const PauseCase& pc : cases) {
        HWND w = nullptr;
        wt.Invoke([&] {
          w = CreateWindowExW(pc.ex, kFullscreenClass, L"SnowHarness pause case", pc.style, pc.rc.left, pc.rc.top,
                              pc.rc.right - pc.rc.left, pc.rc.bottom - pc.rc.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
          if (!w) return;
          ShowWindow(w, pc.maximize ? SW_SHOWMAXIMIZED : SW_SHOW);
          SetForegroundWindow(w);  // best effort
          UpdateWindow(w);
        });
        std::string res;
        if (!w) {
          res = "FAIL - cannot create the window";
        } else if (pc.expectHidden) {
          const double hideMs = WaitShown(sv, idx, false, 3000);
          wt.Invoke([&] { DestroyWindow(w); });
          const double showMs = hideMs >= 0 ? WaitShown(sv, idx, true, 3000) : -1;
          if (hideMs < 0) res = "FAIL - snow still visible after 3 s";
          else if (showMs < 0) res = "FAIL - snow did not come back within 3 s after it closed";
          else res = std::format("PASS - hidden after {:.0f} ms, back after {:.0f} ms", hideMs, showMs);
        } else {
          const double hideMs = WaitShown(sv, idx, false, 1500);
          wt.Invoke([&] { DestroyWindow(w); });
          if (hideMs >= 0) WaitShown(sv, idx, true, 3000);
          res = hideMs < 0 ? std::string("PASS - snow kept falling") : std::format("FAIL - snow hidden after {:.0f} ms", hideMs);
        }
        Printf("  case {}: {}\n", pc.name, res);
        r.cases.push_back(std::string(pc.name) + ": " + res);
        if (res.rfind("FAIL", 0) == 0) caseFailures.push_back(std::string(pc.name) + ": " + res.substr(7));
        SleepMs(700);  // debounce settles before the next case
      }
    }
    // The brightest capture of each phase, for a look at what was counted.
    for (const auto& [name, ph] : {std::pair<const char*, const FsPhase*>{"before", &r.before},
                                   {"during", &r.during}, {"after", &r.after}})
      if (!ph->brightest.pixels.empty())
        WriteBmp(ctx.outDir / Wide(std::string("fullscreen-") + name + ".bmp"), ph->brightest);
  }
  ov.Stop(quitEvent, kQuitTimeoutMs);
  Printf("  overlay exited with code {}{}\n", ov.exitCode(), ov.terminated() ? " (terminated after timeout)" : "");
  wt.Invoke([&] {
    if (full) DestroyWindow(full);
    if (canvas) DestroyWindow(canvas);
  });
  wt.Stop();

  if (r.failures.empty()) {
    if (r.before.presents <= 0) fail("the overlay rendered nothing before the fullscreen app (0 presents)");
    if (r.before.shownSamples < r.before.samples)
      fail("the overlay was already hidden before the test (another fullscreen app running?)");
    if (r.hideMs < 0) fail("the overlay did not hide within 3 s of the fullscreen app appearing");
    if (r.during.shownSamples > 0)
      fail(std::format("overlay visible in {} of {} samples over the fullscreen app", r.during.shownSamples,
                       r.during.samples));
    if (r.during.maxBright > 0) fail(std::format("{} snow pixels captured over the fullscreen app", r.during.maxBright));
    if (r.allHidden && r.during.presents > 0)
      fail(std::format("{} presents while every overlay was hidden", r.during.presents));
    const double cpuLimit = 0.25 * r.before.cpuPct + 0.3;
    if (r.allHidden && r.during.cpuPct > cpuLimit)
      fail(std::format("CPU {:.2f}% of a core while hidden (limit {:.2f}%)", r.during.cpuPct, cpuLimit));
    if (r.showMs < 0) fail("the overlay did not come back within 3 s after the fullscreen app closed");
    if (r.after.presents <= 0) fail("no frames presented after the fullscreen app closed");
    if (r.before.maxBright > 0 && r.after.maxBright == 0) fail("no snow captured after the fullscreen app closed");
  }
  for (const std::string& f : caseFailures) fail(f);
  r.pass = r.failures.empty();
  for (const std::string& f : r.failures) Printf("  FAIL: {}\n", f);
  if (r.before.maxBright == 0 && r.pass)
    Print("  note: the capture saw no snow before the test either; visibility was judged from the overlay's state\n");
}

std::string FullscreenSummary(const FullscreenResult& r) {
  if (!r.ran) return "not run" + (r.note.empty() ? std::string() : " (" + r.note + ")");
  std::string s = r.pass ? "PASS" : "FAIL";
  if (r.hideMs >= 0)
    s += std::format(" - hidden after {:.0f} ms, CPU {:.2f}% -> {:.2f}% of a core, {} -> {} presents", r.hideMs,
                     r.before.cpuPct, r.during.cpuPct, r.before.presents, r.during.presents);
  if (r.showMs >= 0) s += std::format(", back after {:.0f} ms with {} presents", r.showMs, r.after.presents);
  if (!r.cases.empty()) {
    size_t ok = 0;
    for (const std::string& c : r.cases) ok += c.find(": PASS") != std::string::npos ? 1 : 0;
    s += std::format(", extra cases {}/{} passed", ok, r.cases.size());
  }
  for (const std::string& f : r.failures) s += "; " + f;
  return s;
}

std::string ToCrlf(const std::string& s) {
  std::string o;
  o.reserve(s.size() + s.size() / 32);
  for (char ch : s) {
    if (ch == '\n') o += '\r';
    o += ch;
  }
  return o;
}

// One plain-text file with everything needed to judge a run: the console
// transcript (environment, checks, per-sweep table, snowfall fps) and the full
// overlay logs. Written next to SnowHarness.exe (falling back to the output
// folder when that isn't writable), plus a copy in the output folder.
void WriteResultsFile(const Context& ctx) {
  const fs::path archived = ctx.outDir / L"SnowHarness-results.txt";
  fs::path path = ExeDirectory() / L"SnowHarness-results.txt";
  std::ofstream o(path, std::ios::binary | std::ios::trunc);
  if (!o) {
    Printf("WARNING: cannot write {} - using the output folder instead\n", PathUtf8(path));
    path = archived;
    o.open(path, std::ios::binary | std::ios::trunc);
  }
  if (!o) {
    Printf("WARNING: cannot write {}\n", PathUtf8(path));
    return;
  }
  Printf("\nResults file (send this back): {}\n", PathUtf8(path));

  std::string text = std::format("SnowHarness results - build {}, started {}\nCommand line: {}\n\n", SNOW_BUILD_ID,
                                 ctx.started, Utf8(GetCommandLineW()));
  text += g_transcript;
  text += "\n==== Overlay logs ====\n";
  constexpr size_t kMaxLines = 400;
  for (const fs::path& p : ctx.overlayLogs) {
    std::vector<std::string> lines = ReadLines(p);
    text += "\n---- " + PathUtf8(p.filename()) + " ----\n";
    if (lines.empty()) text += "(missing or empty)\n";
    if (lines.size() > kMaxLines) {
      text += std::format("({} earlier lines omitted)\n", lines.size() - kMaxLines);
      lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(kMaxLines));
    }
    for (const std::string& l : lines) text += l + "\n";
  }
  text = ToCrlf(text);
  o << text;
  o.close();
  if (path != archived) WriteText(archived, text);
}

int Finish(Context& ctx, int abortCode) {
  for (ModeResult& m : ctx.modes) {
    if (!m.ready) continue;
    m.pass = !m.sweeps.empty();
    m.failures.clear();
    for (const SweepResult& r : m.sweeps) {
      if (r.pass) continue;
      m.pass = false;
      for (const std::string& f : r.failures)
        m.failures.push_back(std::format("{} Hz {}: {}", r.rate, SweepName(r.sweep), f));
    }
    if (!m.error.empty()) {
      m.pass = false;
      m.failures.push_back(m.error);
    }
  }
  int code = kExitOk;
  const bool fullscreenOk = !ctx.fullscreen.ran || ctx.fullscreen.pass;
  if (abortCode != kExitOk) {
    code = abortCode;
  } else if (ctx.validTotal < 10 && !ctx.opt.fullscreenOnly) {
    code = kExitNoCapture;
  } else if (ctx.opt.strict && (!(ctx.opt.fullscreenOnly || OverallPass(ctx)) || !fullscreenOk)) {
    code = kExitAcceptance;
  }

  Print("\n==== SnowHarness summary ====\n");
  Print(TextTable(ctx));
  Print("\nInvisible border: " + BorderSummary(ctx.border) + "\n");
  Print("Click-through:    " + ClickSummary(ctx.click) + "\n");
  Printf("Overlay ever foreground: {}\n", ForegroundEverOverlay(ctx) ? "YES" : "no");
  for (const ModeResult& m : ctx.modes) {
    Printf("Mode {}: {}\n", m.mode.name, !m.ready ? "NOT READY (" + m.error + ")" : (m.pass ? "PASS" : "FAIL"));
    for (const std::string& f : m.failures) Printf("    {}\n", f);
  }
  Printf("Valid frames measured: {}{}\n", ctx.validTotal, ctx.validTotal < 10 ? " (fewer than 10!)" : "");
  Printf("Acceptance: {}\n", OverallPass(ctx) ? "PASS" : "FAIL");
  Print("Fullscreen pause: " + FullscreenSummary(ctx.fullscreen) + "\n");
  for (const std::string& c : ctx.fullscreen.cases) Print("  case " + c + "\n");
  Print("Plain snowfall (nothing moving):\n");
  for (const std::string& l : ctx.snowfallStats) Printf("    {}\n", l);
  if (!ctx.snowfallNote.empty()) Printf("    {}\n", ctx.snowfallNote);
  else if (ctx.snowfallStats.empty()) Print("    no fps lines were logged\n");

  WriteText(ctx.outDir / "summary.json", BuildJson(ctx, code));
  WriteText(ctx.outDir / "summary.md", BuildMarkdown(ctx, code));
  Printf("Results written to {}\nExit code {}\n", PathUtf8(ctx.outDir), code);
  WriteResultsFile(ctx);
  return code;
}

// ---- main flow --------------------------------------------------------------------

int Run(const Options& opt) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcFreq = f.QuadPart;
  timeBeginPeriod(1);
  struct PeriodGuard {
    ~PeriodGuard() { timeEndPeriod(1); }
  } periodGuard;

  auto ctxPtr = std::make_unique<Context>();
  Context& ctx = *ctxPtr;
  ctx.opt = opt;
  ctx.scale = opt.quick ? 0.5 : 1.0;
  ctx.dumpsLeft = opt.dumpInvalid;
  SYSTEMTIME now;
  GetLocalTime(&now);
  ctx.started = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", now.wYear, now.wMonth, now.wDay, now.wHour,
                            now.wMinute, now.wSecond);

  ctx.disp = QueryDisplay();
  const DisplayInfo& d = ctx.disp;
  ctx.snowfallNote = "not run: the harness stopped before this phase";
  Print("SnowHarness\n");
  Printf("  Build:        {}\n", SNOW_BUILD_ID);
  Printf("  OS:           Windows {}\n", d.os.empty() ? "?" : d.os);
  Printf("  DPI aware:    {}{}\n", d.dpiAwareness,
         d.perMonitorV2 ? "" : " - WARNING: not per-monitor v2, coordinates may be scaled");
  Printf("  Test monitor: {} {}x{} at {}, work area {}\n", d.device, d.width, d.height, RectText(d.rcMonitor),
         RectText(d.rcWork));
  Printf("  Refresh:      {} Hz (EnumDisplaySettings), {} Hz (DWM)\n", d.refreshHz, JsonNumber(d.dwmRefreshHz, 3));
  Printf("  DPI:          {} ({}%)\n", d.dpi, d.dpi * 100 / 96);

  ctx.exe = opt.exe.empty() ? ExeDirectory() / L"DesktopSnow.exe" : fs::absolute(opt.exe);
  ctx.outDir = opt.out;
  if (ctx.outDir.empty()) {
    ctx.outDir = fs::path(L"harness-results") /
                 Wide(std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", now.wYear, now.wMonth, now.wDay, now.wHour,
                                  now.wMinute, now.wSecond));
  }
  std::error_code ec;
  ctx.outDir = fs::absolute(ctx.outDir, ec);
  (void)fs::create_directories(ctx.outDir, ec);  // checked via is_directory below
  if (!fs::is_directory(ctx.outDir)) {
    Printf("ERROR: cannot create output directory {}\n", PathUtf8(ctx.outDir));
    return kExitUsage;
  }
  ctx.framesCsv.open(ctx.outDir / "frames.csv", std::ios::binary);
  ctx.movesCsv.open(ctx.outDir / "movements.csv", std::ios::binary);
  if (!ctx.framesCsv || !ctx.movesCsv) {
    Printf("ERROR: cannot write into {}\n", PathUtf8(ctx.outDir));
    return kExitUsage;
  }
  ctx.framesCsv << "mode,rate,sweep,t_ms,cmd_x,cmd_y,win_left,win_top,outline_left,outline_top,dx,dy,valid_x,valid_y\n";
  ctx.movesCsv << "mode,rate,sweep,t_ms,x,y\n";
  Printf("  Overlay:      {}\n", PathUtf8(ctx.exe));
  Printf("  Output:       {}\n", PathUtf8(ctx.outDir));

  // The work area keeps the target's edges clear of the taskbar; it is inside the monitor.
  ctx.area = MakeSweepArea(d.rcWork.left + kSideInset, std::max(d.rcWork.top, d.rcMonitor.top) + kTopInset,
                           d.rcWork.right - kSideInset, d.rcWork.bottom - kSideInset, kTargetW, kTargetH);
  if (!ctx.area.Valid()) {
    Printf("ERROR: the test monitor's work area {} is too small for a {}x{} window plus margins\n", RectText(d.rcWork),
           kTargetW, kTargetH);
    return kExitUsage;
  }
  Printf("  Sweep area:   x {}..{}, y {}..{} (window top-left)\n", ctx.area.minX, ctx.area.maxX, ctx.area.minY,
         ctx.area.maxY);
  {
    std::string modes, rates, sweeps;
    for (const Mode& m : opt.modes) modes += (modes.empty() ? "" : ",") + m.name;
    for (int r : opt.rates) rates += (rates.empty() ? "" : ",") + std::to_string(r);
    for (SweepKind k : opt.sweeps) sweeps += (sweeps.empty() ? "" : ",") + std::string(SweepName(k));
    Printf("  Modes:        {}\n  Rates:        {} Hz\n  Sweeps:       {}{}\n", modes, rates, sweeps,
           opt.quick ? " (quick: durations halved)" : "");
  }

  if (!fs::exists(ctx.exe)) {
    Printf("ERROR: overlay executable not found: {} (use --exe)\n", PathUtf8(ctx.exe));
    return Finish(ctx, kExitNotReady);
  }

  struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() {
      if (h) CloseHandle(h);
    }
  };
  HandleGuard ready, quit, job;
  SetLastError(0);
  ready.h = CreateEventW(nullptr, TRUE, FALSE, kReadyEventName);
  const bool readyExisted = GetLastError() == ERROR_ALREADY_EXISTS;
  quit.h = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
  if (!ready.h || !quit.h) {
    Printf("ERROR: cannot create the harness events (error {})\n", GetLastError());
    return kExitUsage;
  }
  if (readyExisted) Print("  WARNING: the Ready event already existed - is another harness or overlay running?\n");
  job.h = CreateJobObjectW(nullptr, nullptr);
  if (job.h) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &li, sizeof li);
  }

  std::string err;
  if (!ctx.mover.Start(d.rcMonitor, ctx.area, &err)) {
    Printf("ERROR: {}\n", err);
    return kExitUsage;
  }
  Printf("  Mover timer:  {}\n",
         ctx.mover.highResTimer() ? "high-resolution waitable timer" : "waitable timer + timeBeginPeriod(1)");
  ctx.parkCursor = POINT{d.rcMonitor.left, d.rcMonitor.top};
  SetCursorPos(ctx.parkCursor.x, ctx.parkCursor.y);
  SleepMs(500);

  if (!ctx.cap.Init(opt.capture, d.monitor, d.rcMonitor, CaptureRefreshHz(d))) {
    Printf("ERROR: {}\n", ctx.cap.description());
    return Finish(ctx, kExitNoCapture);
  }
  Printf("  Capture:      {}\n\n", ctx.cap.description());

  for (size_t mi = 0; mi < opt.modes.size(); ++mi) {
    ctx.modes.emplace_back();
    ModeResult& mr = ctx.modes.back();
    mr.mode = opt.modes[mi];
    Printf("== Mode {} ==\n", mr.mode.name);
    ResetEvent(ready.h);
    ResetEvent(quit.h);
    Overlay ov;
    const fs::path telemetry = ctx.outDir / Wide("overlay-" + mr.mode.name + ".csv");
    const fs::path overlayLog = ctx.outDir / Wide("overlay-" + mr.mode.name + ".log");
    std::error_code logEc;
    fs::remove(overlayLog, logEc);  // the overlay appends
    ctx.overlayLogs.push_back(overlayLog);
    const int64_t launchT = Qpc();
    if (!ov.Launch(ctx.exe, mr.mode, telemetry, overlayLog, job.h, &err)) {
      mr.error = err;
      Printf("ERROR: cannot launch the overlay: {}\n", err);
      return Finish(ctx, kExitNotReady);
    }
    mr.launched = true;
    Printf("  launched: {}\n", ov.commandLine());
    const Overlay::Ready rs = ov.WaitReady(ready.h, kReadyTimeoutMs);
    if (rs != Overlay::Ready::Ok) {
      mr.error = rs == Overlay::Ready::Exited ? std::format("overlay exited with code {} before signalling ready",
                                                             ov.exitCode())
                                              : std::format("no ready signal within {} s", kReadyTimeoutMs / 1000);
      Printf("ERROR: {}\n", mr.error);
      ov.Kill();
      mr.exitCode = ov.exitCode();
      mr.terminated = ov.terminated();
      return Finish(ctx, kExitNotReady);
    }
    mr.ready = true;
    mr.readyMs = QpcToMs(Qpc() - launchT);
    mr.fg.overlayPid = ov.pid();
    Printf("  ready after {:.0f} ms (pid {})\n", mr.readyMs, ov.pid());
    ctx.mover.MoveTo(CenterPoint(ctx.area));
    SleepMs(500);
    mr.fg.Sample();

    if (mi == 0) {
      ctx.border = RunBorderCheck(ctx);
      Print("  invisible border: " + BorderSummary(ctx.border) + "\n");
      ctx.mover.MoveTo(CenterPoint(ctx.area));
      SleepMs(300);
      ctx.click = RunClickCheck(ctx, ov.pid());
      Print("  click-through:    " + ClickSummary(ctx.click) + "\n");
      mr.fg.Sample();
    }

    for (size_t ri = 0; ri < opt.rates.size() && mr.error.empty(); ++ri) {
      const int rate = opt.rates[ri];
      for (SweepKind kind : opt.sweeps) {
        SweepResult r = RunSweep(ctx, mr, rate, kind);
        Printf("  {:>4} Hz {:<6}  frames {:>4} valid {:>4} (steps {} = {:.0f} Hz)  p95|dx| {}  p95|dy| {}  {}  {}\n",
               rate, SweepName(kind), r.s.frames, r.s.validBoth, r.moverSteps, r.moverRateHz,
               r.s.validX ? JsonNumber(r.s.x.p95Abs, 0) : "-", r.s.validY ? JsonNumber(r.s.y.p95Abs, 0) : "-",
               ExtraText(kind, r.s), r.pass ? "PASS" : "FAIL");
        mr.sweeps.push_back(std::move(r));
        if (ov.Exited()) {
          mr.error = "overlay exited during the run";
          Printf("ERROR: {} - skipping the rest of this mode\n", mr.error);
          break;
        }
      }
    }
    ov.Stop(quit.h, kQuitTimeoutMs);
    mr.exitCode = ov.exitCode();
    mr.terminated = ov.terminated();
    Printf("  overlay exited with code {}{}\n", mr.exitCode, mr.terminated ? " (terminated after timeout)" : "");
    for (const std::string& l : StatsLines(overlayLog)) Printf("  overlay: {}\n", l);
    if (mr.fg.overlaySamples > 0)
      Printf("  WARNING: the overlay owned the foreground window in {} of {} samples\n", mr.fg.overlaySamples,
             mr.fg.samples);
  }
  RunFullscreenTest(ctx, quit.h, job.h);
  RunSnowfall(ctx, quit.h, job.h);
  return Finish(ctx, kExitOk);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // failure is reported later
  SetConsoleOutputCP(CP_UTF8);
  Options opt;
  std::string err;
  if (!ParseOptions(argc, argv, &opt, &err)) {
    Printf("error: {}\n\n", err);
    PrintUsage();
    return kExitUsage;
  }
  if (opt.help) {
    PrintUsage();
    return kExitOk;
  }
  return Run(opt);
}
