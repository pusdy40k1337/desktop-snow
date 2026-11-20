// Localised UI strings (English, Russian, Ukrainian). UTF-8.
#pragma once
#include <cstdint>
#include <string>

namespace snow {

enum class Lang : int { En = 0, Ru = 1, Uk = 2 };

enum class Str : int {
  AppName,
  Tagline,
  MenuEnabled,
  MenuIntensity,
  MenuIntensityLight,
  MenuIntensityNormal,
  MenuIntensityHeavy,
  MenuIntensityBlizzard,
  MenuEffect,
  EffectSnow,
  EffectConfetti,
  EffectNone,
  MenuTheme,
  ThemeClassic,
  ThemeFrost,
  ThemeCandy,
  ThemeGolden,
  MenuQuick,
  QuickPiles,
  QuickTaskbarPile,
  QuickWind,
  QuickCursorSweep,
  QuickSparkle,
  QuickMelt,
  QuickDayNight,
  QuickReducedMotion,
  QuickBatterySaver,
  QuickVisitors,
  MenuDecorate,
  MenuSurprise,
  VisitorSanta,
  VisitorTrain,
  VisitorYeti,
  MenuClearSnow,
  MenuClearDecorations,
  MenuDebug,
  MenuLanguage,
  LangAuto,
  MenuStartup,
  MenuOpenFolder,
  MenuQuit,
  TipToggled,
  TipHotkeyFailed,
  TipDecorateHint,
  TipStateRecovered,
  TipTreeLimit,
  ToolTree,
  ToolBauble,
  ToolLights,
  ToolCandyCane,
  ToolStar,
  ToolSnowman,
  ToolHat,
  ToolScarf,
  ToolErase,
  ToolDone,
  Paused,
  MenuPauseFullscreen,
  Count
};

const char* Tr(Str id, Lang lang);  // never null
// setting: "auto" | "en" | "ru" | "uk"; systemLangId = PRIMARYLANGID of the UI language.
Lang ResolveLanguage(const std::string& setting, uint16_t systemPrimaryLangId);
const char* LangCode(Lang l);

}  // namespace snow
