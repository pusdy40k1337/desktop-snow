// Desktop Snow - entry point.
#include "app.h"
#include "platform.h"
#include "settings_window.h"
#include "snapshot.h"

#ifndef SNOW_BUILD_ID
#define SNOW_BUILD_ID "dev"
#endif

#include <shellapi.h>
#include <dbghelp.h>
#include <objbase.h>

#include <cstdio>
#include <cwctype>
#include <string>

namespace {

std::wstring g_dumpDir;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  // Best effort minidump. WinEvent hooks, hotkeys and the tray icon are owned by
  // the process and are released by the OS when it terminates.
  std::wstring path = g_dumpDir + L"\\crash.dmp";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
    CloseHandle(f);
  }
  snow::win::Log("CRASH: exception 0x%08lx, dump written to logs\\crash.dmp",
                 ep && ep->ExceptionRecord ? static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode) : 0ul);
  return EXCEPTION_CONTINUE_SEARCH;
}

void EnablePerMonitorV2() {
  // The manifest already declares PerMonitorV2; this is a fallback for odd launchers.
  using Fn = BOOL(WINAPI*)(HANDLE);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  auto fn = user32 ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
                   : nullptr;
  if (fn) fn(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

void PrintUsage() {
  std::printf(
      "DesktopSnow.exe [options]\n"
      "  --debug              debug overlay (tracked rects, latency, error readout)\n"
      "  --console            log to a console window as well as the log file\n"
      "  --sync latch|vsync   frame pacing mode (default: settings, latch)\n"
      "  --predict on|off     one-frame-ahead extrapolation (default: settings, on)\n"
      "  --telemetry <csv>    per-frame log of window vs drawn pile position\n"
      "  --log <file>         append the log to this file (default: %%LOCALAPPDATA%%\\DesktopSnow\\logs)\n"
      "  --instance <name>    run a separate instance (own state file and mutex)\n"
      "  --no-persist         do not write the state file\n"
      "  --harness            measurement mode used by SnowHarness.exe\n"
      "  --diagnostics        log the pause decision of every monitor every 0.5 s\n"
      "  --quit-after <s>     exit after <s> seconds\n"
      "  --render-decor <dir> render every decoration and visitor with Direct2D into\n"
      "                       <dir>\\decor-<theme>.png and exit\n"
      "  --render-settings <dir>  render every settings page into <dir>\\settings-*.png and exit\n");
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  EnablePerMonitorV2();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  snow::win::LaunchOptions opts;
  bool help = false;
  std::wstring renderDecorDir, renderSettingsDir;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
    if (a == L"--debug") opts.debug = true;
    else if (a == L"--render-decor") renderDecorDir = next();
    else if (a == L"--render-settings") renderSettingsDir = next();
    else if (a == L"--diagnostics") opts.diagnostics = true;
    else if (a == L"--quit-after") opts.quitAfter = _wtoi(next().c_str());
    else if (a == L"--console") opts.console = true;
    else if (a == L"--harness") {
      opts.harness = true;
      opts.noPersist = true;
    } else if (a == L"--no-persist") opts.noPersist = true;
    else if (a == L"--instance") opts.instance = next();
    else if (a == L"--telemetry") opts.telemetryPath = next();
    else if (a == L"--log") opts.logPath = next();
    else if (a == L"--sync") opts.syncOverride = snow::win::Narrow(next());
    else if (a == L"--predict") opts.predictOverride = next() == L"off" ? 0 : 1;
    else if (a == L"--predictor") opts.predictorOverride = snow::win::Narrow(next());
    else if (a == L"--latch-margin") opts.latchMarginOverride = static_cast<float>(_wtof(next().c_str()));
    else if (a == L"--help" || a == L"-h" || a == L"/?") help = true;
  }
  if (argv) LocalFree(argv);
  if (opts.instance.empty()) opts.instance = L"main";
  for (wchar_t& ch : opts.instance)
    if (!iswalnum(ch) && ch != L'-' && ch != L'_') ch = L'_';
  if (opts.syncOverride != "latch" && opts.syncOverride != "vsync") opts.syncOverride.clear();

  if (!renderDecorDir.empty() || !renderSettingsDir.empty()) opts.console = true;
  if (opts.console || help) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    opts.console = true;
  }
  if (help) {
    PrintUsage();
    return 0;
  }
  if (!renderDecorDir.empty() || !renderSettingsDir.empty()) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = !renderDecorDir.empty() ? snow::win::RenderDecorSnapshots(renderDecorDir, SNOW_BUILD_ID)
                                     : snow::win::RenderSettingsSnapshots(renderSettingsDir, SNOW_BUILD_ID);
    std::fflush(stdout);
    CoUninitialize();
    return rc;
  }
  g_dumpDir = snow::win::LogDir();
  SetUnhandledExceptionFilter(CrashFilter);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int rc;
  {
    snow::win::App app;
    rc = app.Run(opts);
  }
  CoUninitialize();
  return rc;
}
