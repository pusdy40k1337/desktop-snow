#include <cstring>
#include <filesystem>
#include <string>

#include "../src/core/settings.h"
#include "../src/core/settings_schema.h"
#include "../src/core/theme.h"
#include "../src/core/ui_strings.h"
#include "test.h"

using namespace snow;
namespace fs = std::filesystem;

namespace {

fs::path SchemaTempDir(const char* name) {
  fs::path d = fs::temp_directory_path() / (std::string("snowtest_schema_") + name);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

const Lang kLangs[] = {Lang::En, Lang::Ru, Lang::Uk};

bool NonEmpty(const char* s) { return s && std::strlen(s) > 0; }

}  // namespace

TEST(ui_strings_every_id_has_text_in_every_language) {
  for (int i = 0; i < static_cast<int>(UiStr::Count); ++i)
    for (Lang l : kLangs) CHECK_MSG(NonEmpty(UiTr(static_cast<UiStr>(i), l)), "UiStr #" + std::to_string(i));
  CHECK(std::string(UiTr(UiStr::Count, Lang::En)).empty());
  CHECK(std::string(UiTr(UiStr::TraySettings, Lang::En)) == "Settings…");
  CHECK(std::string(UiTr(UiStr::SettingPauseMaximized, Lang::Ru)) == "Пауза, когда окно развёрнуто");
}

TEST(ui_format_replaces_placeholders) {
  CHECK(UiFormat("Trees: {0} of {1}", {"2", "3"}) == "Trees: 2 of 3");
  CHECK(UiFormat("{1}-{0}-{5}", {"a", "b"}) == "b-a-");
  CHECK(UiFormat("no args {", {}) == "no args {");
}

TEST(schema_every_item_has_labels_in_every_language) {
  CHECK(SchemaSections().size() == static_cast<size_t>(SectionId::Count));
  for (const auto& sec : SchemaSections()) {
    for (Lang l : kLangs) CHECK(NonEmpty(UiTr(sec.title, l)));
    CHECK(!SectionItems(sec.id).empty());
  }
  CHECK(SchemaItems().size() == static_cast<size_t>(ItemId::Count));
  for (int i = 0; i < static_cast<int>(ItemId::Count); ++i)
    CHECK_MSG(FindItem(static_cast<ItemId>(i)), std::to_string(i));
  for (const auto& it : SchemaItems()) {
    std::string name = "item #" + std::to_string(static_cast<int>(it.id));
    for (Lang l : kLangs) {
      CHECK_MSG(NonEmpty(UiTr(it.label, l)), name);
      if (it.desc != kNoText) CHECK_MSG(NonEmpty(UiTr(it.desc, l)), name);
      for (const auto& o : it.options) CHECK_MSG(NonEmpty(OptionText(o, l)), name);
    }
    if (it.kind == ItemKind::Choice || it.kind == ItemKind::Swatches || it.kind == ItemKind::Buttons)
      CHECK_MSG(!it.options.empty(), name);
    if (it.kind == ItemKind::Slider) CHECK_MSG(it.min < it.max && it.step > 0, name);
  }
}

TEST(schema_toggles_round_trip) {
  for (const auto& it : SchemaItems()) {
    if (it.kind != ItemKind::Toggle) continue;
    Settings s;
    if (it.session) {
      CHECK(!SetToggle(s, it.id, true));  // owned by the host, not by Settings
      continue;
    }
    CHECK(SetToggle(s, it.id, false));
    CHECK(!GetToggle(s, it.id));
    CHECK(SetToggle(s, it.id, true));
    CHECK(GetToggle(s, it.id));
  }
  Settings s;
  CHECK(SetToggle(s, ItemId::PauseMaximized, false) && !s.pauseMaximized && s.pauseFullscreen);
  CHECK(SetToggle(s, ItemId::TaskbarPile, false) && !s.piles.onTaskbar);
  CHECK(!SetToggle(s, ItemId::Intensity, true));  // not a toggle
}

TEST(schema_sliders_clamp_snap_and_survive_sanitize) {
  for (const auto& it : SchemaItems()) {
    if (it.kind != ItemKind::Slider) continue;
    std::string name = "slider #" + std::to_string(static_cast<int>(it.id));
    Settings s;
    CHECK_MSG(SetSlider(s, it.id, it.max + 1000), name);
    CHECK_NEAR(GetSlider(s, it.id), it.max, 1e-6);
    CHECK(SetSlider(s, it.id, it.min - 1000));
    CHECK_NEAR(GetSlider(s, it.id), it.min, 1e-6);
    double mid = it.min + (it.max - it.min) * 0.37;
    CHECK(SetSlider(s, it.id, mid));
    double v = GetSlider(s, it.id);
    double steps = (v - it.min) / it.step;
    CHECK_MSG(std::fabs(steps - std::round(steps)) < 1e-4, name);
    Settings sanitized = s;
    Sanitize(sanitized);
    CHECK_NEAR(GetSlider(sanitized, it.id), v, 1e-4);
  }
  Settings s;
  CHECK(SetSlider(s, ItemId::Intensity, 127));
  CHECK(s.intensity == 125);
  CHECK(!SetSlider(s, ItemId::Enabled, 1));
  CHECK(!SetSlider(s, ItemId::Wind, std::nan("")));
  s.scatter.speedOn = 9000;  // valid for Sanitize, beyond the slider: shown at the end of the track
  CHECK_NEAR(GetSlider(s, ItemId::ShakeOff), 3000, 1e-6);
}

TEST(schema_flake_size_keeps_min_below_max) {
  for (double v = 0; v <= 20; v += 0.25) {
    Settings s;
    CHECK(SetSlider(s, ItemId::FlakeSize, v));
    CHECK(s.snow.flakeMin >= 1);
    CHECK(s.snow.flakeMin <= s.snow.flakeMax);
    Settings c = s;
    Sanitize(c);
    CHECK_NEAR(c.snow.flakeMin, s.snow.flakeMin, 1e-6);
    CHECK_NEAR(c.snow.flakeMax, s.snow.flakeMax, 1e-6);
  }
  Settings s;
  CHECK(SetSlider(s, ItemId::FlakeSize, 7));
  CHECK_NEAR(s.snow.flakeMin, 3, 1e-6);
}

TEST(schema_choices_reject_unknown_values) {
  Settings s;
  CHECK(!SetChoice(s, ItemId::Language, "de"));
  CHECK(s.language == "auto");
  CHECK(SetChoice(s, ItemId::Language, "uk") && s.language == "uk");
  CHECK(!SetChoice(s, ItemId::Effect, "rain"));
  CHECK(SetChoice(s, ItemId::Effect, "confetti") && s.effect == "confetti");
  CHECK(!SetChoice(s, ItemId::Theme, "neon"));
  CHECK(SetChoice(s, ItemId::Theme, "golden") && s.theme == "golden");
  CHECK(!SetChoice(s, ItemId::ReducedMotion, "maybe"));
  CHECK(!SetChoice(s, ItemId::Enabled, "on"));  // not a choice
  CHECK(!SetChoice(s, ItemId::SnowColor, "#123456"));
  CHECK(SetChoice(s, ItemId::SnowColor, "#ddeeff") && s.snow.color == "#ddeeff");
  CHECK(!SetChoice(s, ItemId::FpsCap, "75"));
  // Every theme the renderer knows is selectable, and every choice survives Sanitize.
  const SchemaItem* theme = FindItem(ItemId::Theme);
  CHECK(theme && theme->options.size() == ThemeIds().size());
  for (const auto& id : ThemeIds()) {
    Settings t;
    CHECK(SetChoice(t, ItemId::Theme, id));
  }
  for (const auto& it : SchemaItems()) {
    if (it.kind != ItemKind::Choice && it.kind != ItemKind::Swatches) continue;
    for (const auto& o : it.options) {
      Settings t;
      CHECK(SetChoice(t, it.id, o.value));
      Sanitize(t);
      CHECK_MSG(GetChoice(t, it.id) == o.value, o.value);
      CHECK(ChoiceIndex(t, it) >= 0);
    }
  }
}

TEST(schema_custom_color_selects_no_swatch) {
  Settings s;
  s.snow.color = "#FFFFFF";
  CHECK(GetChoice(s, ItemId::SnowColor) == "#ffffff");
  s.snow.color = "#ccddff";
  CHECK(GetChoice(s, ItemId::SnowColor).empty());
  CHECK(ChoiceIndex(s, *FindItem(ItemId::SnowColor)) == -1);
}

TEST(schema_battery_three_way_mapping) {
  Settings s;
  CHECK(GetChoice(s, ItemId::Battery) == "save");  // defaults: saver on, no pause
  CHECK(SetChoice(s, ItemId::Battery, "full"));
  CHECK(!s.batterySaver && !s.pauseOnBattery);
  CHECK(GetChoice(s, ItemId::Battery) == "full");
  CHECK(SetChoice(s, ItemId::Battery, "pause"));
  CHECK(s.batterySaver && s.pauseOnBattery);
  CHECK(GetChoice(s, ItemId::Battery) == "pause");
  CHECK(SetChoice(s, ItemId::Battery, "save"));
  CHECK(s.batterySaver && !s.pauseOnBattery);
  // Pause wins however the flags were combined in the file.
  s.batterySaver = false;
  s.pauseOnBattery = true;
  CHECK(GetChoice(s, ItemId::Battery) == "pause");
}

TEST(schema_fps_nearest_option) {
  CHECK(NearestFpsOption(20) == 30);
  CHECK(NearestFpsOption(44) == 30);
  CHECK(NearestFpsOption(46) == 60);
  CHECK(NearestFpsOption(60) == 60);
  CHECK(NearestFpsOption(100) == 90);
  CHECK(NearestFpsOption(140) == 144);
  CHECK(NearestFpsOption(240) == 144);
  Settings s;
  s.fpsCap = 75;
  CHECK(GetChoice(s, ItemId::FpsCap) == "60");
  CHECK(SetChoice(s, ItemId::FpsCap, "144") && s.fpsCap == 144);
}

TEST(schema_monitor_enable_disable) {
  Settings s;
  const std::string a = "\\\\.\\DISPLAY1", b = "\\\\.\\DISPLAY2";
  CHECK(IsMonitorEnabled(s, a) && IsMonitorEnabled(s, b));
  SetMonitorEnabled(s, b, false);
  SetMonitorEnabled(s, b, false);  // no duplicates
  CHECK(s.disabledMonitors.size() == 1);
  CHECK(!IsMonitorEnabled(s, b) && IsMonitorEnabled(s, a));
  SetMonitorEnabled(s, a, false);
  SetMonitorEnabled(s, b, true);
  CHECK(s.disabledMonitors.size() == 1 && s.disabledMonitors[0] == a);
  CHECK(MonitorLabel(1, true, 1920, 1080, 1.25f, Lang::En) ==
        "Monitor 1 (main) \xE2\x80\x94 1920 \xC3\x97 1080, 125 %");
  CHECK(MonitorLabel(2, false, 0, 0, 1, Lang::Ru) == "Монитор 2");
}

TEST(schema_hotkey_format_and_validation) {
  CHECK(FormatHotkey(true, true, false, false, "S") == "Ctrl+Alt+S");
  CHECK(FormatHotkey(true, false, true, true, "F12") == "Ctrl+Shift+Win+F12");
  CHECK(IsValidHotkey("Ctrl+Alt+S"));
  CHECK(IsValidHotkey("Shift+F24"));
  CHECK(IsValidHotkey("win+7"));
  CHECK(!IsValidHotkey("S"));          // needs a modifier
  CHECK(!IsValidHotkey("Ctrl+Alt"));   // needs a key
  CHECK(!IsValidHotkey("Ctrl+F25"));
  CHECK(!IsValidHotkey("Ctrl+Esc"));
  CHECK(HotkeyDisplay("Ctrl+Alt+S") == "Ctrl + Alt + S");
  Settings s;
  CHECK(!SetHotkey(s, ItemId::HotkeyToggle, "Q"));
  CHECK(s.hotkeyToggle == "Ctrl+Alt+S");
  CHECK(SetHotkey(s, ItemId::HotkeyDecorate, "Ctrl+Shift+D") && s.hotkeyDecorate == "Ctrl+Shift+D");
  CHECK(GetHotkey(s, ItemId::HotkeyDecorate) == "Ctrl+Shift+D");
}

TEST(schema_slider_value_text) {
  CHECK(FormatSliderValue(ItemId::Intensity, 125, Lang::En) == "125 %");
  CHECK(FormatSliderValue(ItemId::FlakeSize, 7.5, Lang::En) == "7.5 px");
  CHECK(FormatSliderValue(ItemId::FlakeSize, 7.5, Lang::Ru) == "7,5 пикс.");
  CHECK(FormatSliderValue(ItemId::Wind, 0, Lang::En) == "0");
  CHECK(FormatSliderValue(ItemId::Wind, -12, Lang::En) == "\xE2\x86\x90 12");
  CHECK(FormatSliderValue(ItemId::Wind, 5, Lang::En) == "5 \xE2\x86\x92");
}

TEST(settings_reset_to_defaults_keeps_startup) {
  Settings s;
  s.launchAtStartup = true;
  s.pauseMaximized = false;
  s.pauseFullscreen = false;
  s.pauseOnBattery = true;
  s.intensity = 50;
  s.disabledMonitors.push_back("\\\\.\\DISPLAY2");
  s.language = "ru";
  ResetToDefaults(s);
  CHECK(s.launchAtStartup);
  CHECK(s.pauseMaximized && s.pauseFullscreen && !s.pauseOnBattery);
  CHECK(s.intensity == 100);
  CHECK(s.disabledMonitors.empty());
  CHECK(s.language == "auto");
  Settings off;
  ResetToDefaults(off);
  CHECK(!off.launchAtStartup);
}

TEST(settings_new_fields_round_trip_through_file) {
  fs::path dir = SchemaTempDir("roundtrip");
  StateStore store(dir / "state.json");
  AppState st;
  st.settings.pauseMaximized = false;
  st.settings.pauseOnBattery = true;
  st.settings.disabledMonitors = {"\\\\.\\DISPLAY2", "\\\\.\\DISPLAY3"};
  CHECK(store.save(st));
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.fromFile);
  CHECK(!loaded.settings.pauseMaximized);
  CHECK(loaded.settings.pauseOnBattery);
  CHECK(loaded.settings.pauseFullscreen);
  CHECK(loaded.settings.disabledMonitors.size() == 2 && loaded.settings.disabledMonitors[1] == "\\\\.\\DISPLAY3");
}

TEST(settings_v2_file_without_new_fields_migrates) {
  fs::path dir = SchemaTempDir("v2");
  StateStore store(dir / "state.json");
  CHECK(WriteTextFile(store.file(),
                      "{\"version\":2,\"settings\":{\"enabled\":true,\"intensity\":80,\"pauseFullscreen\":false,"
                      "\"batterySaver\":false},\"decor\":{}}"));
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.fromFile && !r.newerVersion);
  CHECK(loaded.settings.intensity == 80);
  CHECK(!loaded.settings.pauseFullscreen);
  CHECK(loaded.settings.pauseMaximized);
  CHECK(!loaded.settings.pauseOnBattery);
  CHECK(loaded.settings.disabledMonitors.empty());
  CHECK(GetChoice(loaded.settings, ItemId::Battery) == "full");
}

TEST(settings_legacy_import_enables_pause_when_maximized) {
  fs::path dir = SchemaTempDir("legacy");
  fs::path legacy = dir / "legacy-settings.json";
  CHECK(WriteTextFile(legacy,
                      "{\"activeEffect\":\"snow\",\"values\":{\"snow\":{\"density\":60,\"accumulate\":true}}}"));
  StateStore store(dir / "state.json");
  AppState loaded;
  LoadResult r = store.load(loaded, legacy);
  CHECK(r.migratedLegacy);
  CHECK(loaded.settings.pauseMaximized);
  CHECK(loaded.settings.pauseFullscreen);
  CHECK(!loaded.settings.pauseOnBattery);
  CHECK(loaded.settings.disabledMonitors.empty());
}
