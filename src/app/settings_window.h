// Settings window: a custom-drawn (Direct2D / DirectWrite) Windows 11 style
// window with a section sidebar. Changes apply instantly through SettingsHost.
#pragma once
#include "platform.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/settings.h"
#include "../core/strings.h"

namespace snow::win {

enum class UiAction {
  Decorate,
  ClearSnow,
  ClearDecorations,
  ClearSnowmen,
  Santa,
  Train,
  Yeti,
  CopyDiagnostics,
  SaveDiagnostics,
  OpenSettingsFolder,
  OpenLogFolder,
  ResetDefaults,
};

struct UiMonitor {
  std::string id;             // device name, as stored in Settings::disabledMonitors
  int width = 0, height = 0;  // physical px
  float scale = 1;            // 1.25 = 125 %
  bool primary = false;
};

struct UiStatus {
  std::vector<std::wstring> pauseLines;  // live pause decision, one line per monitor
  int trees = 0, maxTrees = 0, snowmen = 0, maxSnowmen = 0;
  bool diagnosticsLogging = false;
  bool pausedForHour = false;
  double fps = 0;     // overlay frame rate over the last second, 0 = unknown
  std::string build;  // build id (commit)
};

// Implemented by the app. Every callback runs on the UI (main) thread.
struct SettingsHost {
  std::function<Settings()> get;
  std::function<void(const Settings&)> set;  // apply instantly and persist (the host sanitises)
  std::function<void(UiAction)> action;
  std::function<std::vector<UiMonitor>()> monitors;
  std::function<UiStatus()> status;
  std::function<void(bool)> setDiagnosticsLogging;
  std::function<void(bool)> hotkeyCapture;  // true while a new shortcut is being typed
  std::function<Lang()> lang;
};

class SettingsWindow {
 public:
  SettingsWindow();
  ~SettingsWindow();
  void Init(HINSTANCE inst, SettingsHost host);
  void Show();     // creates on first use, restores, brings to front and focuses
  void Hide();     // "closes to tray"
  void Destroy();  // app exit
  bool IsOpen() const;
  HWND Hwnd() const;  // nullptr until first shown
  void Refresh();     // settings, language or status changed outside the window

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// --render-settings <dir>: draws every section in dark and light theme into
// <dir>\settings-<section>-<dark|light>.png (software rendering) and returns 0.
int RenderSettingsSnapshots(const std::wstring& dir, const char* buildId);

}  // namespace snow::win
