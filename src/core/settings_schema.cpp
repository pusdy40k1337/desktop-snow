#include "settings_schema.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "theme.h"

namespace snow {

namespace {

using K = ItemKind;
using S = SectionId;

SchemaItem Make(ItemId id, SectionId sec, ItemKind kind, UiStr label, UiStr desc = kNoText) {
  SchemaItem it{id, sec, kind, label, desc, 0, 0, 0, {}, false, false};
  return it;
}
// Every slider lives in the Snow section.
SchemaItem Slider(ItemId id, UiStr label, UiStr desc, double lo, double hi, double step) {
  SchemaItem it = Make(id, S::Snow, K::Slider, label, desc);
  it.min = lo;
  it.max = hi;
  it.step = step;
  return it;
}
SchemaItem Choice(ItemId id, SectionId sec, UiStr label, UiStr desc, std::vector<ChoiceOption> opts,
                  ItemKind kind = K::Choice) {
  SchemaItem it = Make(id, sec, kind, label, desc);
  it.options = std::move(opts);
  return it;
}
SchemaItem Buttons(ItemId id, SectionId sec, UiStr label, UiStr desc, std::vector<ChoiceOption> buttons,
                   bool accent = false) {
  SchemaItem it = Choice(id, sec, label, desc, std::move(buttons), K::Buttons);
  it.accent = accent;
  return it;
}

std::vector<SchemaItem> BuildItems() {
  std::vector<SchemaItem> v;
  // General
  v.push_back(Make(ItemId::Enabled, S::General, K::Toggle, UiStr::SettingEnabled, UiStr::DescEnabled));
  v.push_back(Make(ItemId::Startup, S::General, K::Toggle, UiStr::SettingStartup, UiStr::DescStartup));
  v.push_back(Choice(ItemId::Language, S::General, UiStr::SettingLanguage, UiStr::DescLanguage,
                     {{"auto", UiStr::OptAuto},
                      {"en", kNoText, "English"},
                      {"ru", kNoText, "Русский"},
                      {"uk", kNoText, "Українська"}}));
  v.push_back(Make(ItemId::HotkeyToggle, S::General, K::Hotkey, UiStr::SettingHotkeyToggle, UiStr::DescHotkey));
  v.push_back(Make(ItemId::HotkeyDecorate, S::General, K::Hotkey, UiStr::SettingHotkeyDecorate, UiStr::DescHotkey));
  v.push_back(Make(ItemId::PauseFullscreen, S::General, K::Toggle, UiStr::SettingPauseFullscreen,
                   UiStr::DescPauseFullscreen));
  v.push_back(Make(ItemId::PauseMaximized, S::General, K::Toggle, UiStr::SettingPauseMaximized,
                   UiStr::DescPauseMaximized));
  v.push_back(Choice(ItemId::Battery, S::General, UiStr::SettingBattery, UiStr::DescBattery,
                     {{"full", UiStr::BatteryFull}, {"save", UiStr::BatterySave}, {"pause", UiStr::BatteryPause}}));
  // Snow
  v.push_back(Choice(ItemId::Effect, S::Snow, UiStr::SettingEffect, kNoText,
                     {{"snow", UiStr::EffectSnow}, {"confetti", UiStr::EffectConfetti}, {"none", UiStr::EffectNone}}));
  v.push_back(Slider(ItemId::Intensity, UiStr::SettingIntensity, UiStr::DescIntensity, 25, 200, 5));
  v.push_back(Slider(ItemId::FlakeSize, UiStr::SettingFlakeSize, kNoText, 2, 16, 0.5));
  v.push_back(Slider(ItemId::FallSpeed, UiStr::SettingFallSpeed, kNoText, 20, 300, 5));
  v.push_back(Slider(ItemId::Wind, UiStr::SettingWind, UiStr::DescWind, -50, 50, 1));
  v.push_back(Slider(ItemId::Breeze, UiStr::SettingBreeze, UiStr::DescBreeze, 0, 100, 5));
  v.push_back(Make(ItemId::WindGusts, S::Snow, K::Toggle, UiStr::SettingWindGusts));
  v.push_back(Choice(ItemId::SnowColor, S::Snow, UiStr::SettingColor, kNoText,
                     {{"#ffffff", UiStr::ColorWhite},
                      {"#ddeeff", UiStr::ColorIce},
                      {"#fff3e0", UiStr::ColorWarm},
                      {"#ffe3f1", UiStr::ColorPink}},
                     K::Swatches));
  v.push_back(Make(ItemId::Sparkle, S::Snow, K::Toggle, UiStr::SettingSparkle, UiStr::DescSparkle));
  v.push_back(Make(ItemId::Piles, S::Snow, K::Toggle, UiStr::SettingPiles, UiStr::DescPiles));
  v.push_back(Slider(ItemId::PileHeight, UiStr::SettingPileHeight, kNoText, 6, 120, 1));
  v.push_back(Make(ItemId::TaskbarPile, S::Snow, K::Toggle, UiStr::SettingTaskbar, UiStr::DescTaskbar));
  v.push_back(Slider(ItemId::ShakeOff, UiStr::SettingShakeOff, UiStr::DescShakeOff, 300, 3000, 50));
  v.push_back(Make(ItemId::CursorSweep, S::Snow, K::Toggle, UiStr::SettingCursorSweep));
  v.push_back(Make(ItemId::Melt, S::Snow, K::Toggle, UiStr::SettingMelt, UiStr::DescMelt));
  v.push_back(Make(ItemId::DayNight, S::Snow, K::Toggle, UiStr::SettingDayNight, UiStr::DescDayNight));
  v.push_back(Choice(ItemId::ReducedMotion, S::Snow, UiStr::SettingReducedMotion, UiStr::DescReducedMotion,
                     {{"auto", UiStr::OptAuto}, {"on", UiStr::OptOn}, {"off", UiStr::OptOff}}));
  // Decorations
  v.push_back(Buttons(ItemId::DecorateNow, S::Decorations, UiStr::SettingDecorateNow, UiStr::DescDecorateNow,
                      {{"decorate", UiStr::BtnDecorate}}, true));
  v.push_back(Make(ItemId::DecorCount, S::Decorations, K::Info, UiStr::SettingDecorCount, UiStr::DecorCount));
  v.push_back(Choice(ItemId::Theme, S::Decorations, UiStr::SettingTheme, kNoText,
                     {{"classic", UiStr::ThemeClassic},
                      {"frost", UiStr::ThemeFrost},
                      {"candy", UiStr::ThemeCandy},
                      {"golden", UiStr::ThemeGolden}}));
  v.push_back(Make(ItemId::Santa, S::Decorations, K::Toggle, UiStr::SettingSanta, UiStr::DescSanta));
  v.push_back(Make(ItemId::Train, S::Decorations, K::Toggle, UiStr::SettingTrain, UiStr::DescTrain));
  v.push_back(Make(ItemId::TrainKnocks, S::Decorations, K::Toggle, UiStr::SettingTrainKnocks, UiStr::DescTrainKnocks));
  v.push_back(Make(ItemId::Snowballs, S::Decorations, K::Toggle, UiStr::SettingSnowballs, UiStr::DescSnowballs));
  v.push_back(Make(ItemId::Yeti, S::Decorations, K::Toggle, UiStr::SettingYeti, UiStr::DescYeti));
  v.push_back(Buttons(ItemId::CallNow, S::Decorations, UiStr::SettingCallNow, kNoText,
                      {{"santa", UiStr::BtnSanta}, {"train", UiStr::BtnTrain}, {"yeti", UiStr::BtnYeti}}));
  v.push_back(Buttons(ItemId::ClearDecorations, S::Decorations, UiStr::SettingClearDecor, UiStr::DescClearDecor,
                      {{"clear-decorations", UiStr::BtnClear}}));
  v.push_back(Buttons(ItemId::ClearSnow, S::Decorations, UiStr::SettingClearSnow, UiStr::DescClearSnow,
                      {{"clear-snow", UiStr::BtnClear}}));
  // Monitors
  v.push_back(Make(ItemId::MonitorsInfo, S::Monitors, K::Info, UiStr::SettingMonitorsInfo, UiStr::DescMonitorsInfo));
  v.push_back(Make(ItemId::MonitorList, S::Monitors, K::MonitorList, UiStr::MonitorName));
  // Performance
  v.push_back(Choice(ItemId::FpsCap, S::Performance, UiStr::SettingFps, UiStr::DescFps,
                     {{"30", kNoText, "30"},
                      {"60", kNoText, "60"},
                      {"90", kNoText, "90"},
                      {"120", kNoText, "120"},
                      {"144", kNoText, "144"}}));
  v.push_back(Make(ItemId::FpsNow, S::Performance, K::Info, UiStr::SettingFpsNow, UiStr::FpsValue));
  v.push_back(Make(ItemId::BatteryNote, S::Performance, K::Info, UiStr::SettingBatteryNote, UiStr::DescBatteryNote));
  // Diagnostics
  v.push_back(Make(ItemId::DebugOverlay, S::Diagnostics, K::Toggle, UiStr::SettingDebug, UiStr::DescDebug));
  {
    SchemaItem log = Make(ItemId::DiagLogging, S::Diagnostics, K::Toggle, UiStr::SettingDiagLog, UiStr::DescDiagLog);
    log.session = true;
    v.push_back(log);
  }
  v.push_back(Make(ItemId::PauseLines, S::Diagnostics, K::StatusLines, UiStr::SettingPauseNow, UiStr::DescPauseNow));
  v.push_back(Buttons(ItemId::CopyDiag, S::Diagnostics, UiStr::SettingCopyDiag, UiStr::DescCopyDiag,
                      {{"copy-diagnostics", UiStr::BtnCopy}}, true));
  v.push_back(Buttons(ItemId::SaveDiag, S::Diagnostics, UiStr::SettingSaveDiag, UiStr::DescSaveDiag,
                      {{"save-diagnostics", UiStr::BtnSave}}));
  v.push_back(Buttons(ItemId::OpenSettingsFolder, S::Diagnostics, UiStr::SettingOpenSettings, kNoText,
                      {{"open-settings-folder", UiStr::BtnOpen}}));
  v.push_back(Buttons(ItemId::OpenLogFolder, S::Diagnostics, UiStr::SettingOpenLogs, kNoText,
                      {{"open-log-folder", UiStr::BtnOpen}}));
  // About
  v.push_back(Make(ItemId::AboutHeader, S::About, K::AboutHeader, UiStr::AboutBuild, UiStr::AboutText));
  v.push_back(Buttons(ItemId::Reset, S::About, UiStr::SettingReset, UiStr::DescReset, {{"reset", UiStr::BtnReset}}));
  return v;
}

bool* ToggleField(Settings& s, ItemId id) {
  switch (id) {
    case ItemId::Enabled: return &s.enabled;
    case ItemId::Startup: return &s.launchAtStartup;
    case ItemId::PauseFullscreen: return &s.pauseFullscreen;
    case ItemId::PauseMaximized: return &s.pauseMaximized;
    case ItemId::WindGusts: return &s.extras.windGusts;
    case ItemId::Sparkle: return &s.extras.sparkle;
    case ItemId::Piles: return &s.piles.enabled;
    case ItemId::TaskbarPile: return &s.piles.onTaskbar;
    case ItemId::CursorSweep: return &s.extras.cursorSweep;
    case ItemId::Melt: return &s.extras.melt;
    case ItemId::DayNight: return &s.extras.dayNightTint;
    case ItemId::Santa: return &s.extras.santa;
    case ItemId::Train: return &s.extras.train;
    case ItemId::TrainKnocks: return &s.extras.trainKnocksSnowmen;
    case ItemId::Snowballs: return &s.extras.snowballs;
    case ItemId::Yeti: return &s.extras.yeti;
    case ItemId::DebugOverlay: return &s.debugOverlay;
    default: return nullptr;
  }
}

const SchemaItem* ItemOfKind(ItemId id, ItemKind kind) {
  const SchemaItem* it = FindItem(id);
  return it && it->kind == kind ? it : nullptr;
}

bool HasOption(const SchemaItem& it, const std::string& value) {
  for (const auto& o : it.options)
    if (value == o.value) return true;
  return false;
}

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::string Num(double v, Lang lang) {
  char buf[32];
  double r = std::round(v);
  if (std::fabs(v - r) < 1e-6) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(r));
  else std::snprintf(buf, sizeof buf, "%.1f", v);
  std::string out = buf;
  if (lang != Lang::En) std::replace(out.begin(), out.end(), '.', ',');
  return out;
}

const int kFpsOptions[] = {30, 60, 90, 120, 144};

}  // namespace

const std::vector<SectionDef>& SchemaSections() {
  static const std::vector<SectionDef> v = {
      {S::General, UiStr::SecGeneral, 0xE713, "general"},
      {S::Snow, UiStr::SecSnow, 0xE9CA, "snow"},
      {S::Decorations, UiStr::SecDecorations, 0xE734, "decorations"},
      {S::Monitors, UiStr::SecMonitors, 0xE7F4, "monitors"},
      {S::Performance, UiStr::SecPerformance, 0xE9D9, "performance"},
      {S::Diagnostics, UiStr::SecDiagnostics, 0xEBE8, "diagnostics"},
      {S::About, UiStr::SecAbout, 0xE946, "about"},
  };
  return v;
}

const std::vector<SchemaItem>& SchemaItems() {
  static const std::vector<SchemaItem> v = BuildItems();
  return v;
}

std::vector<const SchemaItem*> SectionItems(SectionId section) {
  std::vector<const SchemaItem*> out;
  for (const auto& it : SchemaItems())
    if (it.section == section) out.push_back(&it);
  return out;
}

const SchemaItem* FindItem(ItemId id) {
  for (const auto& it : SchemaItems())
    if (it.id == id) return &it;
  return nullptr;
}

const char* OptionText(const ChoiceOption& o, Lang lang) {
  if (o.literal) return o.literal;
  return UiTr(o.label, lang);
}

bool GetToggle(const Settings& s, ItemId id) {
  bool* f = ToggleField(const_cast<Settings&>(s), id);
  return f ? *f : false;
}

bool SetToggle(Settings& s, ItemId id, bool on) {
  bool* f = ToggleField(s, id);
  if (!f) return false;
  *f = on;
  return true;
}

double GetSlider(const Settings& s, ItemId id) {
  const SchemaItem* it = ItemOfKind(id, K::Slider);
  if (!it) return 0;
  double v = 0;
  switch (id) {
    case ItemId::Intensity: v = s.intensity; break;
    case ItemId::Breeze: v = s.breezePercent; break;
    case ItemId::FlakeSize: v = s.snow.flakeMax; break;
    case ItemId::FallSpeed: v = s.snow.fallSpeed; break;
    case ItemId::Wind: v = s.snow.windSpeed; break;
    case ItemId::PileHeight: v = s.piles.maxHeight; break;
    case ItemId::ShakeOff: v = s.scatter.speedOn; break;
    default: return 0;
  }
  if (!std::isfinite(v)) v = it->min;
  return std::clamp(v, it->min, it->max);
}

bool SetSlider(Settings& s, ItemId id, double v) {
  const SchemaItem* it = ItemOfKind(id, K::Slider);
  if (!it || !std::isfinite(v)) return false;
  v = std::clamp(v, it->min, it->max);
  if (it->step > 0) v = std::clamp(it->min + std::round((v - it->min) / it->step) * it->step, it->min, it->max);
  float f = static_cast<float>(v);
  switch (id) {
    case ItemId::Intensity: s.intensity = static_cast<int>(std::lround(v)); break;
    case ItemId::Breeze: s.breezePercent = static_cast<int>(std::lround(v)); break;
    case ItemId::FlakeSize:
      s.snow.flakeMax = f;
      s.snow.flakeMin = FlakeMinFor(f);
      break;
    case ItemId::FallSpeed: s.snow.fallSpeed = f; break;
    case ItemId::Wind: s.snow.windSpeed = f; break;
    case ItemId::PileHeight: s.piles.maxHeight = f; break;
    case ItemId::ShakeOff: s.scatter.speedOn = f; break;
    default: return false;
  }
  return true;
}

std::string GetChoice(const Settings& s, ItemId id) {
  switch (id) {
    case ItemId::Language: return s.language;
    case ItemId::Battery: return s.pauseOnBattery ? "pause" : s.batterySaver ? "save" : "full";
    case ItemId::Effect: return s.effect;
    case ItemId::SnowColor: {
      std::string c = Lower(s.snow.color);
      const SchemaItem* it = FindItem(id);
      return it && HasOption(*it, c) ? c : std::string();
    }
    case ItemId::ReducedMotion: return s.reducedMotion;
    case ItemId::Theme: return s.theme;
    case ItemId::FpsCap: return std::to_string(NearestFpsOption(s.fpsCap));
    default: return {};
  }
}

bool SetChoice(Settings& s, ItemId id, const std::string& value) {
  const SchemaItem* it = FindItem(id);
  if (!it || (it->kind != K::Choice && it->kind != K::Swatches) || !HasOption(*it, value)) return false;
  switch (id) {
    case ItemId::Language: s.language = value; break;
    case ItemId::Battery:
      s.pauseOnBattery = value == "pause";
      s.batterySaver = value != "full";
      break;
    case ItemId::Effect: s.effect = value; break;
    case ItemId::SnowColor: s.snow.color = value; break;
    case ItemId::ReducedMotion: s.reducedMotion = value; break;
    case ItemId::Theme: s.theme = value; break;
    case ItemId::FpsCap: s.fpsCap = std::atoi(value.c_str()); break;
    default: return false;
  }
  return true;
}

int ChoiceIndex(const Settings& s, const SchemaItem& item) {
  std::string v = GetChoice(s, item.id);
  for (size_t i = 0; i < item.options.size(); ++i)
    if (v == item.options[i].value) return static_cast<int>(i);
  return -1;
}

std::string GetHotkey(const Settings& s, ItemId id) {
  if (id == ItemId::HotkeyToggle) return s.hotkeyToggle;
  if (id == ItemId::HotkeyDecorate) return s.hotkeyDecorate;
  return {};
}

bool SetHotkey(Settings& s, ItemId id, const std::string& text) {
  if (!IsValidHotkey(text)) return false;
  if (id == ItemId::HotkeyToggle) s.hotkeyToggle = text;
  else if (id == ItemId::HotkeyDecorate) s.hotkeyDecorate = text;
  else return false;
  return true;
}

std::string FormatSliderValue(ItemId id, double v, Lang lang) {
  switch (id) {
    case ItemId::Intensity: return Num(v, lang) + " %";
    case ItemId::Breeze: return Num(v, lang) + " %";
    case ItemId::FlakeSize:
    case ItemId::PileHeight: return Num(v, lang) + " " + UiTr(UiStr::UnitPx, lang);
    case ItemId::FallSpeed:
    case ItemId::ShakeOff: return Num(v, lang) + " " + UiTr(UiStr::UnitPxPerSec, lang);
    case ItemId::Wind:
      if (std::fabs(v) < 0.5) return "0";
      return v < 0 ? "\xE2\x86\x90 " + Num(-v, lang) : Num(v, lang) + " \xE2\x86\x92";  // ← / →
    default: return Num(v, lang);
  }
}

int NearestFpsOption(int fps) {
  int best = kFpsOptions[0];
  for (int o : kFpsOptions)
    if (std::abs(o - fps) < std::abs(best - fps)) best = o;
  return best;
}

float FlakeMinFor(float flakeMax) {
  return std::max(1.f, std::round(flakeMax * 0.43f * 2.f) / 2.f);
}

bool IsMonitorEnabled(const Settings& s, const std::string& monitorId) {
  return std::find(s.disabledMonitors.begin(), s.disabledMonitors.end(), monitorId) == s.disabledMonitors.end();
}

void SetMonitorEnabled(Settings& s, const std::string& monitorId, bool on) {
  auto& v = s.disabledMonitors;
  v.erase(std::remove(v.begin(), v.end(), monitorId), v.end());
  if (!on && !monitorId.empty()) v.push_back(monitorId);
}

std::string MonitorLabel(int number, bool primary, int width, int height, float scale, Lang lang) {
  std::string out = UiFormat(UiTr(UiStr::MonitorName, lang), {std::to_string(number)});
  if (primary) out += std::string(" (") + UiTr(UiStr::MonitorMain, lang) + ")";
  if (width > 0 && height > 0) {
    out += " \xE2\x80\x94 " + std::to_string(width) + " \xC3\x97 " + std::to_string(height);  // — and ×
    if (scale > 0) out += ", " + std::to_string(static_cast<int>(std::lround(scale * 100.f))) + " %";
  }
  return out;
}

std::string FormatHotkey(bool ctrl, bool alt, bool shift, bool win, const std::string& key) {
  std::string out;
  if (ctrl) out += "Ctrl+";
  if (alt) out += "Alt+";
  if (shift) out += "Shift+";
  if (win) out += "Win+";
  return out + key;
}

bool IsValidHotkey(const std::string& text) {
  // Mirrors ParseHotkey() in the Windows platform layer.
  bool mod = false, key = false;
  size_t start = 0;
  while (start <= text.size()) {
    size_t plus = text.find('+', start);
    std::string part = Lower(text.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
    while (!part.empty() && part.front() == ' ') part.erase(0, 1);
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (part == "ctrl" || part == "control" || part == "alt" || part == "shift" || part == "win") {
      mod = true;
    } else if (part.size() == 1 && ((part[0] >= 'a' && part[0] <= 'z') || (part[0] >= '0' && part[0] <= '9'))) {
      key = true;
    } else if (part.size() >= 2 && part.size() <= 3 && part[0] == 'f') {
      int n = std::atoi(part.c_str() + 1);
      if (n < 1 || n > 24) return false;
      key = true;
    } else if (!part.empty()) {
      return false;
    }
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  return mod && key && text.size() <= 40;
}

std::string HotkeyDisplay(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '+') out += " + ";
    else if (c != ' ') out += c;
  }
  return out;
}

}  // namespace snow
