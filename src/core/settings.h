// Application settings + persistent state (versioned JSON).
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "decor.h"
#include "json.h"

namespace snow {

constexpr int kStateVersion = 2;  // v1 = legacy Electron settings.json, v2 = this app

struct SnowSettings {
  int density = 45;          // 0..100 (legacy default)
  float windSpeed = 5;       // -50..50
  float flakeMin = 3, flakeMax = 7;  // DIP
  float fallSpeed = 90;      // DIP/s, 20..300
  float sway = 35;           // 0..100
  std::string color = "#ffffff";
};

struct PileSettings {
  bool enabled = true;       // legacy "accumulate"
  float maxHeight = 34;      // DIP
  float growth = 1.0f;       // 0.25..4 multiplier
  bool onTaskbar = true;
  float taskbarMaxHeight = 40;  // DIP (legacy dockMoundHeight)
  float cornerRadius = -1;   // DIP, -1 = auto (Windows 11 = 8)
};

struct ScatterSettings {
  float speedOn = 900;       // px/s
  float hysteresis = 0.67f;  // speedOff = speedOn * hysteresis
  float accelOn = 22000;     // px/s^2
  float regrowDelay = 1.5f;  // s
  float inherit = 0.55f;     // share of window velocity given to debris
};

struct SyncSettings {
  std::string mode = "latch";  // "latch" (sample late, just before composition) or "vsync"
  bool predict = true;
  std::string predictor = "stepped";  // "stepped" (whole input steps for coarse input) | "linear"
  float latchMarginMs = 1.5f;  // extra safety margin before the composition deadline
};

struct ExtrasSettings {
  bool windGusts = true;
  bool cursorSweep = true;
  bool melt = false;
  bool sparkle = true;
  bool dayNightTint = true;
  bool santa = true;
  bool train = true;
  bool trainKnocksSnowmen = true;
  bool snowballs = true;
  bool yeti = true;
};

struct Settings {
  bool enabled = true;
  std::string effect = "snow";
  int intensity = 100;       // percent, 25..200
  std::string theme = "classic";
  int fpsCap = 60;           // 20..240
  std::string language = "auto";  // auto | en | ru | uk
  bool launchAtStartup = false;
  bool debugOverlay = false;
  std::string reducedMotion = "auto";  // auto | on | off
  bool batterySaver = true;  // halve intensity on battery / battery saver
  bool pauseFullscreen = true;  // hide the snow on monitors showing a fullscreen app
  bool pauseMaximized = true;   // ...and on monitors whose top window is maximised / fills the work area
  bool pauseOnBattery = false;  // no snow at all while on battery
  std::vector<std::string> disabledMonitors;  // device names of monitors with snow off, e.g. \\.\DISPLAY2
  std::string hotkeyToggle = "Ctrl+Alt+S";
  std::string hotkeyDecorate = "Ctrl+Alt+D";
  // Decorate-mode UI: remembered colour and the dragged toolbar position
  // (absolute virtual-screen pixels, -1 = the default spot).
  int decorColor = -1;
  int paletteLeft = -1;
  int paletteTop = -1;
  SnowSettings snow;
  PileSettings piles;
  ScatterSettings scatter;
  SyncSettings sync;
  ExtrasSettings extras;
};

struct AppState {
  Settings settings;
  DecorState decor;
};

// Clamp every field into its valid range (hand-edited / corrupt values).
void Sanitize(Settings& s);
// "Reset to defaults": every setting back to its default, except launch at
// startup, which mirrors the registry and is changed only on request.
void ResetToDefaults(Settings& s);
void Sanitize(DecorState& d);

json::Value ToJson(const AppState& st);
// Missing fields keep their defaults; wrong types are ignored.
void FromJson(const json::Value& v, AppState& st);
// Imports the original Electron app's settings.json ({activeEffect, values:{snow:{...}}}).
bool ImportLegacy(const json::Value& v, Settings& s);

struct LoadResult {
  bool fromFile = false;
  bool fromBackup = false;
  bool corruptFileQuarantined = false;
  bool migratedLegacy = false;
  bool newerVersion = false;
  std::string message;
};

class StateStore {
 public:
  explicit StateStore(std::filesystem::path file) : file_(std::move(file)) {}
  const std::filesystem::path& file() const { return file_; }
  std::filesystem::path backupFile() const;
  // Never throws. On a corrupt main file it is renamed to *.corrupt and the
  // backup (last good save) is tried; otherwise defaults are used.
  LoadResult load(AppState& out, const std::filesystem::path& legacyFile = {});
  // Atomic: write *.tmp, keep previous file as *.bak, rename tmp over main.
  bool save(const AppState& st, std::string* error = nullptr);

 private:
  std::filesystem::path file_;
};

bool ReadTextFile(const std::filesystem::path& p, std::string& out);
bool WriteTextFile(const std::filesystem::path& p, const std::string& data);

}  // namespace snow
