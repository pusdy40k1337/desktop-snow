// Data-driven description of the settings window: sections, rows and how each
// row reads and writes a Settings value. Portable so the mapping is unit-tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"
#include "ui_strings.h"

namespace snow {

enum class SectionId : int { General, Snow, Decorations, Monitors, Performance, Diagnostics, About, Count };

enum class ItemKind : int {
  Toggle,       // bool setting
  Slider,       // number in [min, max] with a step
  Choice,       // segmented buttons over `options`
  Swatches,     // colour circles over `options` (value = "#rrggbb")
  Hotkey,       // shortcut capture
  Buttons,      // one or more push buttons; option value = action key
  Info,         // read-only line (text may be live, filled by the window)
  MonitorList,  // one toggle per connected monitor
  StatusLines,  // live pause decision, one line per monitor
  AboutHeader,  // icon, name, tagline, build and description
};

enum class ItemId : int {
  // General
  Enabled, Startup, Language, HotkeyToggle, HotkeyDecorate, PauseFullscreen, PauseMaximized, Battery,
  // Snow
  Effect, Intensity, FlakeSize, FallSpeed, Wind, Breeze, WindGusts, SnowColor, Sparkle, Piles, PileHeight, TaskbarPile,
  ShakeOff, CursorSweep, Melt, DayNight, ReducedMotion,
  // Decorations
  DecorateNow, DecorCount, Theme, Santa, Train, TrainKnocks, Snowballs, Yeti, CallNow, ClearDecorations, ClearSnowmen, ClearSnow,
  // Monitors
  MonitorsInfo, MonitorList,
  // Performance
  FpsCap, FpsNow, BatteryNote,
  // Diagnostics
  DebugOverlay, DiagLogging, PauseLines, CopyDiag, SaveDiag, OpenSettingsFolder, OpenLogFolder,
  // About
  AboutHeader, Reset,
  Count
};

constexpr UiStr kNoText = UiStr::Count;

struct ChoiceOption {
  const char* value;              // stored value, or the action key of a button
  UiStr label = kNoText;          // localised label...
  const char* literal = nullptr;  // ...or a fixed label shown in every language ("English", "60")
};

struct SchemaItem {
  ItemId id;
  SectionId section;
  ItemKind kind;
  UiStr label;
  UiStr desc = kNoText;
  double min = 0, max = 0, step = 0;  // sliders
  std::vector<ChoiceOption> options;  // choices, swatches and buttons
  bool session = false;  // not stored in Settings (diagnostics logging, owned by the host)
  bool accent = false;   // buttons: the first button uses the accent style
};

struct SectionDef {
  SectionId id;
  UiStr title;
  uint32_t icon;     // Segoe Fluent Icons / Segoe MDL2 Assets code point
  const char* key;   // "general", used in snapshot file names
};

const std::vector<SectionDef>& SchemaSections();
const std::vector<SchemaItem>& SchemaItems();
std::vector<const SchemaItem*> SectionItems(SectionId section);
const SchemaItem* FindItem(ItemId id);
const char* OptionText(const ChoiceOption& o, Lang lang);

// Value access. Every setter returns false (and leaves `s` untouched) for an
// item of another kind or an unknown value.
bool GetToggle(const Settings& s, ItemId id);
bool SetToggle(Settings& s, ItemId id, bool on);
double GetSlider(const Settings& s, ItemId id);  // clamped into the slider range
bool SetSlider(Settings& s, ItemId id, double v);  // clamps and snaps to the step
std::string GetChoice(const Settings& s, ItemId id);  // "" when the value is not an option (custom colour)
bool SetChoice(Settings& s, ItemId id, const std::string& value);
int ChoiceIndex(const Settings& s, const SchemaItem& item);  // -1 when no option matches
std::string GetHotkey(const Settings& s, ItemId id);
bool SetHotkey(Settings& s, ItemId id, const std::string& text);  // only valid shortcuts
std::string FormatSliderValue(ItemId id, double v, Lang lang);

// Custom mappings.
int NearestFpsOption(int fps);       // 30 / 60 / 90 / 120 / 144
float FlakeMinFor(float flakeMax);   // smallest flake, kept proportional to the largest
bool IsMonitorEnabled(const Settings& s, const std::string& monitorId);
void SetMonitorEnabled(Settings& s, const std::string& monitorId, bool on);
// "Monitor 1 (main) — 1920 × 1080, 125 %"
std::string MonitorLabel(int number, bool primary, int width, int height, float scale, Lang lang);

// Shortcuts in the format ParseHotkey() accepts: "Ctrl+Alt+Shift+Win+K"; key is
// "A".."Z", "0".."9" or "F1".."F24" and at least one modifier is required.
std::string FormatHotkey(bool ctrl, bool alt, bool shift, bool win, const std::string& key);
bool IsValidHotkey(const std::string& text);
std::string HotkeyDisplay(const std::string& text);  // "Ctrl + Alt + S"

}  // namespace snow
