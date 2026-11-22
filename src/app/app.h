// Main-thread controller: state, tray, hotkeys, system events, decorate mode.
#pragma once
#include "engine.h"
#include "platform.h"
#include "settings_window.h"
#include "tracker.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../core/fullscreen.h"
#include "../core/settings.h"

namespace snow::win {

// One monitor's latest pause decision with everything needed to explain it.
struct MonitorDiag {
  std::string id;
  RectI rect, work;
  float scale = 1;
  bool primary = false;
  PauseDecision decision;
  std::string window;  // deciding window: hwnd, class, exe, frame, flags ("" = none)
  std::string exe;     // deciding window's process
};

class App {
 public:
  int Run(const LaunchOptions& opts);

 private:
  static LRESULT CALLBACK ControlProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK CatcherProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleCatcher(HWND, UINT, WPARAM, LPARAM);

  void LoadState();
  void ScheduleSave();
  void SaveNow();
  void PushConfig();
  void PushDecor();
  void RefreshSystemState();
  // Re-evaluates which monitors show a fullscreen app or a maximised window;
  // changes apply after the debounce unless `immediate`.
  void CheckFullscreen(bool immediate = false);
  void RegisterHotkeys();
  void UnregisterHotkeys();

  // settings window
  void InitSettingsWindow();
  void ApplySettings(Settings s);
  void SettingsAction(UiAction a);
  UiStatus Status();
  void SetPauseHour(bool on);

  // diagnostics
  std::vector<std::string> DiagnosticsLines() const;
  std::string DiagnosticsReport() const;
  void CopyDiagnostics();
  void SaveDiagnostics();
  bool DiagnosticsLogging() const { return opts_.diagnostics || diagLogging_; }

  // tray
  void AddTrayIcon();
  void RemoveTrayIcon();
  void Balloon(const std::string& text);
  void ShowMenu(POINT pt);
  void OnCommand(UINT id);
  HICON MakeIcon(int size);

  // decorate mode
  void EnterDecorate();
  void ExitDecorate();
  void DecorClick(POINT pt);
  int HitItem(POINT pt, std::vector<DecorLayoutItem>& layout, bool& isTree);
  bool AnchorAt(POINT pt, WindowAnchor& anchor, float& relX, bool* overWindow = nullptr);
  void OpenColorPicker(int target, uint32_t snowmanId, uint32_t freeId, POINT at, int selected);
  void ApplyPickerColor(int index);
  void UpdateHoverPreview(uint32_t hover);
  void RestoreHoverPreview();
  const char* T(Str s) const;

  LaunchOptions opts_;
  HWND hwnd_ = nullptr;
  UINT taskbarCreatedMsg_ = 0;
  HICON icon_ = nullptr;
  bool trayAdded_ = false;
  std::unique_ptr<StateStore> store_;
  AppState state_;
  WindowTracker tracker_;
  Engine engine_;
  Lang lang_ = Lang::En;
  bool systemPaused_ = false;   // lock / sleep / display off
  bool displayOff_ = false;
  bool battery_ = false, remote_ = false, reduced_ = false;
  bool onBattery_ = false;      // on battery power right now (regardless of settings)
  double pauseUntil_ = 0;       // "Pause for 1 hour": Now() when it ends, 0 = not paused
  float night_ = 0;
  std::vector<std::string> hiddenMonitors_;
  FullscreenDebouncer fsDebounce_{0.3};
  bool hotkeyFailedShown_ = false;
  HANDLE quitEvent_ = nullptr;
  HPOWERNOTIFY powerNotify_[3] = {};
  SettingsWindow settingsWnd_;

  // diagnostics
  std::vector<MonitorDiag> diag_;
  std::string diagShell_;   // SHQueryUserNotificationState, e.g. "QUNS_ACCEPTS_NOTIFICATIONS(5)"
  std::string diagFg_;      // foreground window description
  std::string diagLastKey_; // last decision summary, to record changes only
  std::deque<std::string> diagHistory_;  // timestamped decision changes
  bool diagLogging_ = false;  // session toggle from the settings window

  // decorate mode
  bool decorate_ = false;
  Tool tool_ = Tool::Tree;
  std::vector<HWND> catchers_;
  uint32_t hoverId_ = 0;
  bool dragging_ = false, pressed_ = false;
  enum class DragKind { None = 0, Tree, Snowman, Free };
  DragKind dragKind_ = DragKind::None;
  uint32_t dragId_ = 0;
  float dragOffX_ = 0, dragOffY_ = 0;
  POINT pressPt_{};
  int colorCycle_ = 0;
  // Colour picker popup (hats/scarves).
  struct ColorPopup {
    bool open = false;
    POINT at{};
    int target = 0;  // 1 = snowman hat, 2 = snowman scarf, 3 = free decoration
    uint32_t snowmanId = 0;
    uint32_t freeId = 0;
    int selected = 0;
  } colorPopup_;
  // Hover preview: hat/scarf that would be put on the hovered snowman.
  bool previewActive_ = false;
  uint32_t previewSnowman_ = 0;
  int previewSaved_ = 0;
  int previewTool_ = 0;  // 1 = hat, 2 = scarf
};

}  // namespace snow::win
