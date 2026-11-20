// Main-thread controller: state, tray, hotkeys, system events, decorate mode.
#pragma once
#include "engine.h"
#include "platform.h"
#include "tracker.h"

#include <memory>
#include <string>
#include <vector>

#include "../core/fullscreen.h"
#include "../core/settings.h"

namespace snow::win {

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
  // Re-evaluates which monitors show a fullscreen app; changes apply after the
  // debounce unless `immediate`.
  void CheckFullscreen(bool immediate = false);
  void RegisterHotkeys();
  void UnregisterHotkeys();

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
  bool AnchorAt(POINT pt, WindowAnchor& anchor, float& relX);
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
  float night_ = 0;
  std::vector<std::string> hiddenMonitors_;
  FullscreenDebouncer fsDebounce_{0.3};
  bool hotkeyFailedShown_ = false;
  HANDLE quitEvent_ = nullptr;
  HPOWERNOTIFY powerNotify_[3] = {};

  // decorate mode
  bool decorate_ = false;
  Tool tool_ = Tool::Tree;
  std::vector<HWND> catchers_;
  uint32_t hoverId_ = 0;
  bool dragging_ = false, pressed_ = false;
  uint32_t dragId_ = 0;
  bool dragTree_ = false;
  POINT pressPt_{};
  int colorCycle_ = 0;
};

}  // namespace snow::win
