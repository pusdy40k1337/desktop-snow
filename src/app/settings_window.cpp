// Settings window: custom Direct2D / DirectWrite widgets in a normal top-level
// window (taskbar button, Alt+Tab). The page content comes from the portable
// settings schema; the same paint code renders on screen and into PNG snapshots.
#include "settings_window.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <unordered_map>

#include "../core/decor.h"
#include "../core/settings_schema.h"
#include "../core/theme.h"
#include "../core/ui_strings.h"

namespace snow::win {

namespace {

using Microsoft::WRL::ComPtr;

// Layout, in DIP.
constexpr float kSidebarW = 280.f;
constexpr float kNavTop = 100.f;
constexpr float kNavStep = 40.f;
constexpr float kNavH = 36.f;
constexpr float kTitleTop = 24.f;
constexpr float kRowsTop = 92.f;
constexpr float kRowGap = 4.f;
constexpr float kRadius = 7.f;
constexpr float kPadX = 20.f;
constexpr float kPadY = 14.f;
constexpr float kCtlH = 32.f;
constexpr float kValueW = 76.f;
constexpr float kMaxCardW = 1000.f;
constexpr float kDefaultW = 980.f, kDefaultH = 700.f, kMinW = 760.f, kMinH = 520.f;
constexpr float kHoverSpeed = 1.f / 0.12f;   // full fade in 120 ms
constexpr float kToggleSpeed = 1.f / 0.15f;  // knob slide in 150 ms
constexpr UINT_PTR kAnimTimer = 1, kStatusTimer = 2;
constexpr wchar_t kClassName[] = L"DesktopSnowSettings";

// DWM attributes that older SDK / MinGW headers may lack.
constexpr DWORD kDwmDarkModeOld = 19, kDwmDarkMode = 20, kDwmCorners = 33, kDwmBackdrop = 38;
constexpr int kDwmRound = 2, kDwmBackdropMainWindow = 2;

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.f) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 255) / 255.f, static_cast<float>((rgb >> 8) & 255) / 255.f,
                      static_cast<float>(rgb & 255) / 255.f, a);
}
D2D1_COLOR_F Lerp(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
D2D1_COLOR_F Fade(D2D1_COLOR_F c, float a) {
  c.a *= a;
  return c;
}
float Ease(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t < 0.5f ? 2.f * t * t : 1.f - (2.f - 2.f * t) * (2.f - 2.f * t) / 2.f;
}
D2D1_RECT_F Rect(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return Rect(r.left - d, r.top - d, r.right + d, r.bottom + d); }
bool Contains(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
std::wstring W(const std::string& s) { return Widen(s); }

// ---- theme --------------------------------------------------------------------------------------------------

struct Palette {
  bool dark = true, mica = false;
  D2D1_COLOR_F window, sidebar, layer, card, cardHover, cardBorder, text, text2, accent, accentHover, onAccent, ctrl,
      ctrlHover, ctrlBorder, strong, track, thumb, navSel, navHover, focus;
};

Palette MakePalette(bool dark, bool hasAccent, uint32_t accentRgb, bool mica) {
  Palette p;
  p.dark = dark;
  p.mica = mica;
  D2D1_COLOR_F acc = hasAccent ? Hex(accentRgb) : Hex(dark ? 0x4CC2FF : 0x005FB8);
  // Keep the system accent readable: lighter on dark, darker on light (like Windows' accent shades).
  float lum = 0.2126f * acc.r + 0.7152f * acc.g + 0.0722f * acc.b;
  if (dark && lum < 0.5f) acc = Lerp(acc, Hex(0xFFFFFF), std::min(0.6f, 0.75f - lum));
  if (!dark && lum > 0.35f) acc = Lerp(acc, Hex(0x000000), std::min(0.5f, lum - 0.2f));
  p.accent = acc;
  if (dark) {
    p.window = Hex(0x202020);
    p.sidebar = Hex(0x1C1C1C);
    p.layer = Hex(0x3A3A3A, 0.3f);
    p.card = mica ? Hex(0xFFFFFF, 0.05f) : Hex(0x2B2B2B);
    p.cardHover = mica ? Hex(0xFFFFFF, 0.08f) : Hex(0x323232);
    p.cardBorder = Hex(0xFFFFFF, 0.06f);
    p.text = Hex(0xFFFFFF);
    p.text2 = Hex(0xC5C5C5);
    p.accentHover = Lerp(acc, Hex(0x000000), 0.1f);
    p.onAccent = Hex(0x000000);
    p.ctrl = Hex(0xFFFFFF, 0.06f);
    p.ctrlHover = Hex(0xFFFFFF, 0.09f);
    p.ctrlBorder = Hex(0xFFFFFF, 0.09f);
    p.strong = Hex(0xFFFFFF, 0.6f);
    p.track = Hex(0xFFFFFF, 0.35f);
    p.thumb = Hex(0x454545);
    p.navSel = Hex(0xFFFFFF, 0.06f);
    p.navHover = Hex(0xFFFFFF, 0.04f);
    p.focus = Hex(0xFFFFFF);
  } else {
    p.window = Hex(0xF3F3F3);
    p.sidebar = Hex(0xEEEEEE);
    p.layer = Hex(0xFFFFFF, 0.5f);
    p.card = mica ? Hex(0xFFFFFF, 0.7f) : Hex(0xFBFBFB);
    p.cardHover = mica ? Hex(0xFFFFFF, 0.85f) : Hex(0xF6F6F6);
    p.cardBorder = Hex(0x000000, 0.06f);
    p.text = Hex(0x1A1A1A);
    p.text2 = Hex(0x5C5C5C);
    p.accentHover = Lerp(acc, Hex(0xFFFFFF), 0.1f);
    p.onAccent = Hex(0xFFFFFF);
    p.ctrl = Hex(0xFFFFFF);
    p.ctrlHover = Hex(0xF3F3F3);
    p.ctrlBorder = Hex(0x000000, 0.1f);
    p.strong = Hex(0x000000, 0.55f);
    p.track = Hex(0x000000, 0.3f);
    p.thumb = Hex(0xFFFFFF);
    p.navSel = Hex(0x000000, 0.045f);
    p.navHover = Hex(0x000000, 0.03f);
    p.focus = Hex(0x000000, 0.9f);
  }
  return p;
}

bool ReadDword(const wchar_t* path, const wchar_t* name, DWORD& out) {
  DWORD size = sizeof(out);
  return RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}
bool SystemDarkMode() {
  DWORD light = 1;
  ReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", light);
  return light == 0;
}
bool SystemAccent(uint32_t& rgb) {
  DWORD abgr = 0;
  if (!ReadDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", abgr)) return false;
  rgb = ((abgr & 0xFFu) << 16) | (abgr & 0xFF00u) | ((abgr >> 16) & 0xFFu);
  return true;
}

// ---- text ---------------------------------------------------------------------------------------------------

struct Fonts {
  ComPtr<IDWriteTextFormat> title, body, caption, line, lineRight, center, icon, app, header, mono;
};

bool HasFamily(IDWriteFactory* dw, const wchar_t* name) {
  ComPtr<IDWriteFontCollection> c;
  if (FAILED(dw->GetSystemFontCollection(&c, FALSE))) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  return SUCCEEDED(c->FindFamilyName(name, &index, &exists)) && exists;
}

ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* dw, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                     DWRITE_TEXT_ALIGNMENT align, bool wrap, bool centerV) {
  ComPtr<IDWriteTextFormat> f;
  if (FAILED(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                  L"", &f)))
    return nullptr;
  f->SetTextAlignment(align);
  f->SetParagraphAlignment(centerV ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (!wrap) {
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> sign;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f.Get(), &sign))) f->SetTrimming(&trim, sign.Get());
  }
  return f;
}

// ---- model --------------------------------------------------------------------------------------------------

enum class CtlKind : uint8_t { Nav = 1, Toggle, Monitor, Slider, Segment, Swatch, Hotkey, Button, Card, NavStop };

uint64_t MakeKey(CtlKind k, int item, int sub) {
  return (static_cast<uint64_t>(k) << 40) | (static_cast<uint64_t>(static_cast<uint32_t>(item) & 0xFFFFu) << 20) |
         static_cast<uint64_t>(static_cast<uint32_t>(sub + 1) & 0xFFFFFu);
}
constexpr uint64_t kToggleAnimBit = 1ull << 63;
constexpr int kNoSub = -1;

struct Ctl {
  CtlKind kind;
  const SchemaItem* item = nullptr;
  int sub = kNoSub;     // option, button, monitor or section index
  int row = -1;
  D2D1_RECT_F hit{};    // content controls use unscrolled coordinates
  D2D1_RECT_F box{};    // the visible control (switch, track, button)
  bool content = true;
  uint64_t key = 0;     // hover / press identity
  uint64_t focus = 0;   // the focus stop it belongs to
};

struct Stop {
  uint64_t key;
  CtlKind kind;
  const SchemaItem* item;
  int sub;
  D2D1_RECT_F rect;
  bool content;
  float radius;
};

struct Row {
  const SchemaItem* item = nullptr;
  int sub = kNoSub;
  float y = 0, h = 0, textTop = 0, textW = 0, labelH = 0, descH = 0, ctlTop = 0;
  bool below = false;  // control on its own line under the text
  bool hover = false;  // the card reacts to the mouse
  uint64_t key = 0;
  ComPtr<IDWriteTextLayout> label, desc;
  std::vector<std::wstring> lines;
  D2D1_RECT_F group{};  // segmented choice / swatch container
};

struct Anim {
  float v = 0, target = 0, speed = 1;
};

bool SameStatus(const UiStatus& a, const UiStatus& b) {
  return a.pauseLines == b.pauseLines && a.trees == b.trees && a.maxTrees == b.maxTrees && a.snowmen == b.snowmen &&
         a.maxSnowmen == b.maxSnowmen && a.diagnosticsLogging == b.diagnosticsLogging &&
         a.pausedForHour == b.pausedForHour && std::lround(a.fps) == std::lround(b.fps) && a.build == b.build;
}
bool SameMonitors(const std::vector<UiMonitor>& a, const std::vector<UiMonitor>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].id != b[i].id || a[i].width != b[i].width || a[i].height != b[i].height || a[i].scale != b[i].scale ||
        a[i].primary != b[i].primary)
      return false;
  return true;
}

UiAction ActionFor(const std::string& key, bool& ok) {
  ok = true;
  if (key == "decorate") return UiAction::Decorate;
  if (key == "santa") return UiAction::Santa;
  if (key == "train") return UiAction::Train;
  if (key == "yeti") return UiAction::Yeti;
  if (key == "clear-decorations") return UiAction::ClearDecorations;
  if (key == "clear-snow") return UiAction::ClearSnow;
  if (key == "copy-diagnostics") return UiAction::CopyDiagnostics;
  if (key == "save-diagnostics") return UiAction::SaveDiagnostics;
  if (key == "open-settings-folder") return UiAction::OpenSettingsFolder;
  if (key == "open-log-folder") return UiAction::OpenLogFolder;
  if (key == "reset") return UiAction::ResetDefaults;
  ok = false;
  return UiAction::Decorate;
}

}  // namespace

// ---- implementation -----------------------------------------------------------------------------------------

struct SettingsWindow::Impl {
  HINSTANCE inst = nullptr;
  SettingsHost host;
  HWND hwnd = nullptr;
  UINT dpi = 96;
  HICON iconBig = nullptr, iconSmall = nullptr;

  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dw;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<ID2D1HwndRenderTarget> hwndRt;
  ID2D1RenderTarget* rt = nullptr;  // target being painted
  ID2D1RenderTarget* boundRt = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1Bitmap> icon;
  bool iconTried = false;
  Fonts fonts;
  bool fontsReady = false;

  // Model.
  Settings settings;
  UiStatus status;
  std::vector<UiMonitor> monitors;
  Lang lang = Lang::En;
  bool dark = true, hasAccent = false, mica = false;
  uint32_t accent = 0;
  Palette pal = MakePalette(true, false, 0, false);
  float width = kDefaultW, height = kDefaultH;  // DIP
  float cx = 0, cw = 0;                         // content column
  int section = 0;
  float scroll = 0, contentBottom = 0;
  uint64_t focus = MakeKey(CtlKind::NavStop, 0, kNoSub);
  bool focusVisible = false;
  uint64_t hoverKey = 0, pressKey = 0, dragKey = 0;
  int hoverRow = -1;
  float mouseX = -1, mouseY = -1;
  bool trackingMouse = false;
  ItemId capturing = ItemId::Count;
  std::vector<Row> rows;
  std::vector<Ctl> ctls;
  std::vector<Stop> stops;
  std::unordered_map<uint64_t, Anim> anims;
  bool animating = false, animTimer = false, statusTimer = false;
  ULONGLONG lastTick = 0;

  // ---- setup ----

  bool InitFactories() {
    if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
      return false;
    if (!wic)
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                       reinterpret_cast<void**>(wic.GetAddressOf()));
    return true;
  }

  void EnsureFonts() {
    if (fontsReady || !dw) return;
    IDWriteFactory* f = dw.Get();
    const wchar_t* text = HasFamily(f, L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display = HasFamily(f, L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = HasFamily(f, L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const wchar_t* mono = HasFamily(f, L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    const auto lead = DWRITE_TEXT_ALIGNMENT_LEADING;
    fonts.title = MakeFormat(f, display, 28.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, lead, false, false);
    fonts.body = MakeFormat(f, text, 14.f, DWRITE_FONT_WEIGHT_NORMAL, lead, true, false);
    fonts.caption = MakeFormat(f, text, 12.f, DWRITE_FONT_WEIGHT_NORMAL, lead, true, false);
    fonts.line = MakeFormat(f, text, 14.f, DWRITE_FONT_WEIGHT_NORMAL, lead, false, true);
    fonts.lineRight = MakeFormat(f, text, 14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, false, true);
    fonts.center = MakeFormat(f, text, 14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true);
    fonts.icon = MakeFormat(f, icons, 16.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true);
    fonts.app = MakeFormat(f, display, 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, lead, false, false);
    fonts.header = MakeFormat(f, display, 20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, lead, true, false);
    fonts.mono = MakeFormat(f, mono, 12.f, DWRITE_FONT_WEIGHT_NORMAL, lead, false, false);
    fontsReady = fonts.title && fonts.body && fonts.caption && fonts.line && fonts.lineRight && fonts.center &&
                 fonts.icon && fonts.app && fonts.header && fonts.mono;
  }

  void ReadTheme() {
    dark = SystemDarkMode();
    hasAccent = SystemAccent(accent);
    pal = MakePalette(dark, hasAccent, accent, mica);
  }

  void ApplyDwm() {
    if (!hwnd) return;
    BOOL darkAttr = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmDarkMode, &darkAttr, sizeof darkAttr)))
      DwmSetWindowAttribute(hwnd, kDwmDarkModeOld, &darkAttr, sizeof darkAttr);  // Windows 10 1809-1909
    int corners = kDwmRound;
    DwmSetWindowAttribute(hwnd, kDwmCorners, &corners, sizeof corners);
    bool wasMica = mica;
    mica = false;
    if (OsBuild() >= 22621) {
      int backdrop = kDwmBackdropMainWindow;
      if (SUCCEEDED(DwmSetWindowAttribute(hwnd, kDwmBackdrop, &backdrop, sizeof backdrop))) {
        MARGINS m{-1, -1, -1, -1};
        mica = SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &m));
      }
    }
    pal = MakePalette(dark, hasAccent, accent, mica);
    if (wasMica != mica) DiscardTarget();  // the pixel format depends on it
  }

  std::wstring T(UiStr id) const { return W(UiTr(id, lang)); }

  std::wstring Title() const { return W(Tr(Str::AppName, lang)) + L" \x2014 " + T(UiStr::WindowTitle); }

  // ---- host ----

  void Reload() {
    if (host.get) settings = host.get();
    Lang old = lang;
    if (host.lang) lang = host.lang();
    if (hwnd && old != lang) SetWindowTextW(hwnd, Title().c_str());
  }

  void Commit(Settings s) {
    if (host.set) host.set(s);
    Reload();
    Layout();
    Invalidate();
  }

  void RefreshStatus(bool force) {
    bool changed = force;
    if (host.status) {
      UiStatus st = host.status();
      if (!SameStatus(st, status)) changed = true;
      status = std::move(st);
    }
    if (host.monitors && (force || SchemaSections()[static_cast<size_t>(section)].id == SectionId::Monitors)) {
      std::vector<UiMonitor> m = host.monitors();
      if (!SameMonitors(m, monitors)) changed = true;
      monitors = std::move(m);
    }
    if (changed) {
      Layout();
      Invalidate();
    }
  }

  void Invalidate() {
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
  }

  // ---- layout ----

  ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& s, IDWriteTextFormat* fmt, float maxW, float* h) {
    ComPtr<IDWriteTextLayout> l;
    *h = 0;
    if (s.empty() || !fmt ||
        FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, std::max(maxW, 1.f), 10000.f, &l)))
      return nullptr;
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(l->GetMetrics(&m))) *h = m.height;
    return l;
  }

  float TextWidth(const std::wstring& s, IDWriteTextFormat* fmt) {
    ComPtr<IDWriteTextLayout> l;
    if (s.empty() || !fmt ||
        FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 10000.f, 100.f, &l)))
      return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
  }

  std::wstring RowLabel(const SchemaItem& it, int sub) const {
    if (it.kind == ItemKind::MonitorList) {
      if (sub < 0 || static_cast<size_t>(sub) >= monitors.size()) return T(UiStr::NoMonitors);
      const UiMonitor& m = monitors[static_cast<size_t>(sub)];
      return W(MonitorLabel(sub + 1, m.primary, m.width, m.height, m.scale, lang));
    }
    return T(it.label);
  }

  std::wstring RowDesc(const SchemaItem& it) const {
    switch (it.id) {
      case ItemId::DecorateNow:
        return W(UiFormat(UiTr(it.desc, lang), {HotkeyDisplay(settings.hotkeyDecorate)}));
      case ItemId::DecorCount:
        return W(UiFormat(UiTr(UiStr::DecorCount, lang),
                          {std::to_string(status.trees),
                           std::to_string(status.maxTrees > 0 ? status.maxTrees : kMaxTrees),
                           std::to_string(status.snowmen),
                           std::to_string(status.maxSnowmen > 0 ? status.maxSnowmen : kMaxSnowmen)}));
      case ItemId::FpsNow:
        if (status.fps <= 0) return L"\x2014";
        return W(UiFormat(UiTr(UiStr::FpsValue, lang), {std::to_string(std::lround(status.fps))}));
      case ItemId::HotkeyToggle:
      case ItemId::HotkeyDecorate:
        if (capturing == it.id) return T(UiStr::HotkeyHint);
        break;
      case ItemId::MonitorList:
        return L"";
      default:
        break;
    }
    return it.desc == kNoText ? std::wstring() : T(it.desc);
  }

  bool ToggleValue(const SchemaItem& it, int sub) const {
    if (it.kind == ItemKind::MonitorList)
      return sub >= 0 && static_cast<size_t>(sub) < monitors.size() &&
             IsMonitorEnabled(settings, monitors[static_cast<size_t>(sub)].id);
    if (it.session) return status.diagnosticsLogging;
    return GetToggle(settings, it.id);
  }

  float ToggleWidth() {
    float on = TextWidth(T(UiStr::OptOn), fonts.line.Get()), off = TextWidth(T(UiStr::OptOff), fonts.line.Get());
    return 40.f + 12.f + std::max(on, off);
  }
  float SliderTrackW() const { return cw >= 640.f ? 220.f : 150.f; }
  float SegmentW(const ChoiceOption& o) {
    return std::max(56.f, TextWidth(W(OptionText(o, lang)), fonts.center.Get()) + 28.f);
  }
  float ButtonW(const ChoiceOption& o) {
    return std::max(96.f, TextWidth(W(OptionText(o, lang)), fonts.center.Get()) + 40.f);
  }

  float ControlWidth(const SchemaItem& it, int sub) {
    switch (it.kind) {
      case ItemKind::Toggle: return ToggleWidth();
      case ItemKind::MonitorList: return sub >= 0 ? ToggleWidth() : 0.f;
      case ItemKind::Slider: return SliderTrackW() + 12.f + kValueW;
      case ItemKind::Hotkey: return 190.f;
      case ItemKind::Choice: {
        float w = 4.f;
        for (const auto& o : it.options) w += SegmentW(o);
        return w;
      }
      case ItemKind::Swatches: return static_cast<float>(it.options.size()) * 38.f - 10.f + 8.f;
      case ItemKind::Buttons: {
        float w = 0;
        for (const auto& o : it.options) w += ButtonW(o) + 8.f;
        return w - 8.f;
      }
      default: return 0.f;
    }
  }

  void AddRow(const SchemaItem& it, int sub, float& y) {
    Row r;
    r.item = &it;
    r.sub = sub;
    r.y = y;
    r.key = MakeKey(CtlKind::Card, static_cast<int>(it.id), sub);
    const int rowIndex = static_cast<int>(rows.size());
    const float inner = cw - 2.f * kPadX;
    const float left = cx + kPadX, right = cx + cw - kPadX;
    float ctlW = ControlWidth(it, sub);
    bool multi = it.kind == ItemKind::Choice || it.kind == ItemKind::Swatches || it.kind == ItemKind::Buttons;
    r.below = multi && ctlW > inner * 0.55f;
    r.textW = (r.below || ctlW <= 0.f) ? inner : inner - ctlW - 24.f;
    std::wstring desc = RowDesc(it);
    float h = 0;

    if (it.kind == ItemKind::AboutHeader) {
      r.textW = inner - 76.f;
      r.desc = MakeLayout(T(UiStr::AboutText), fonts.body.Get(), r.textW, &r.descH);
      r.textTop = y + kPadY + 4.f;
      h = 2.f * kPadY + 8.f + 28.f + 22.f + 20.f + 12.f + r.descH;
      r.h = h;
      rows.push_back(std::move(r));
      y += h + kRowGap;
      return;
    }

    r.label = MakeLayout(RowLabel(it, sub), fonts.body.Get(), r.textW, &r.labelH);
    if (!desc.empty()) r.desc = MakeLayout(desc, fonts.caption.Get(), r.textW, &r.descH);
    float textH = r.labelH + (r.desc ? 2.f + r.descH : 0.f);
    h = std::max(r.desc ? 64.f : 52.f, textH + 2.f * kPadY);
    if (it.kind == ItemKind::StatusLines) {
      r.lines = status.pauseLines;
      if (r.lines.empty()) r.lines.push_back(T(UiStr::NoStatus));
      h = 2.f * kPadY + textH + 8.f + 18.f * static_cast<float>(r.lines.size());
    }
    if (r.below) h += kCtlH + 12.f;
    r.h = h;
    r.textTop = (r.below || it.kind == ItemKind::StatusLines) ? y + kPadY : y + (h - textH) / 2.f;
    const float cy = r.below ? r.textTop + textH + 12.f + kCtlH / 2.f : y + h / 2.f;
    r.ctlTop = cy - kCtlH / 2.f;
    const D2D1_RECT_F card = Rect(cx, y, cx + cw, y + h);
    const int id = static_cast<int>(it.id);

    switch (it.kind) {
      case ItemKind::Toggle:
      case ItemKind::MonitorList: {
        if (it.kind == ItemKind::MonitorList && sub < 0) break;  // "no monitors" line
        Ctl c{it.kind == ItemKind::MonitorList ? CtlKind::Monitor : CtlKind::Toggle};
        c.item = &it;
        c.sub = sub;
        c.row = rowIndex;
        c.hit = card;
        c.box = Rect(right - 40.f, cy - 10.f, right, cy + 10.f);
        c.key = c.focus = MakeKey(c.kind, id, sub);
        ctls.push_back(c);
        stops.push_back({c.focus, c.kind, &it, sub, card, true, kRadius});
        r.hover = true;
        break;
      }
      case ItemKind::Slider: {
        float x1 = right - kValueW - 12.f, x0 = x1 - SliderTrackW();
        Ctl c{CtlKind::Slider};
        c.item = &it;
        c.row = rowIndex;
        c.box = Rect(x0, cy - 2.f, x1, cy + 2.f);
        c.hit = Rect(x0 - 12.f, cy - 16.f, x1 + 12.f, cy + 16.f);
        c.key = c.focus = MakeKey(CtlKind::Slider, id, kNoSub);
        ctls.push_back(c);
        stops.push_back({c.focus, c.kind, &it, kNoSub, c.hit, true, 6.f});
        r.hover = true;
        break;
      }
      case ItemKind::Choice:
      case ItemKind::Swatches: {
        bool sw = it.kind == ItemKind::Swatches;
        CtlKind kind = sw ? CtlKind::Swatch : CtlKind::Segment;
        float x = r.below ? left : right - ctlW;
        r.group = Rect(x, cy - kCtlH / 2.f, x + ctlW, cy + kCtlH / 2.f);
        x += sw ? 4.f : 2.f;
        uint64_t group = MakeKey(kind, id, kNoSub);
        for (size_t i = 0; i < it.options.size(); ++i) {
          float w = sw ? 28.f : SegmentW(it.options[i]);
          Ctl c{kind};
          c.item = &it;
          c.sub = static_cast<int>(i);
          c.row = rowIndex;
          c.box = sw ? Rect(x, cy - 14.f, x + 28.f, cy + 14.f)
                     : Rect(x, cy - kCtlH / 2.f + 2.f, x + w, cy + kCtlH / 2.f - 2.f);
          c.hit = sw ? Inflate(c.box, 4.f) : c.box;
          c.key = MakeKey(kind, id, static_cast<int>(i));
          c.focus = group;
          ctls.push_back(c);
          x += w + (sw ? 10.f : 0.f);
        }
        stops.push_back({group, kind, &it, kNoSub, r.group, true, sw ? 16.f : 6.f});
        r.hover = true;
        break;
      }
      case ItemKind::Hotkey: {
        Ctl c{CtlKind::Hotkey};
        c.item = &it;
        c.row = rowIndex;
        c.box = c.hit = Rect(right - 190.f, cy - kCtlH / 2.f, right, cy + kCtlH / 2.f);
        c.key = c.focus = MakeKey(CtlKind::Hotkey, id, kNoSub);
        ctls.push_back(c);
        stops.push_back({c.focus, c.kind, &it, kNoSub, c.box, true, 5.f});
        r.hover = true;
        break;
      }
      case ItemKind::Buttons: {
        float x = r.below ? left : right - ctlW;
        for (size_t i = 0; i < it.options.size(); ++i) {
          float w = ButtonW(it.options[i]);
          Ctl c{CtlKind::Button};
          c.item = &it;
          c.sub = static_cast<int>(i);
          c.row = rowIndex;
          c.box = c.hit = Rect(x, cy - kCtlH / 2.f, x + w, cy + kCtlH / 2.f);
          c.key = c.focus = MakeKey(CtlKind::Button, id, static_cast<int>(i));
          ctls.push_back(c);
          stops.push_back({c.focus, c.kind, &it, static_cast<int>(i), c.box, true, 5.f});
          x += w + 8.f;
        }
        break;
      }
      default:
        break;
    }
    rows.push_back(std::move(r));
    y += h + kRowGap;
  }

  void Layout() {
    if (!dw) return;
    EnsureFonts();
    if (!fontsReady) return;
    rows.clear();
    ctls.clear();
    stops.clear();
    const auto& secs = SchemaSections();
    section = std::clamp(section, 0, static_cast<int>(secs.size()) - 1);
    for (size_t i = 0; i < secs.size(); ++i) {
      Ctl c{CtlKind::Nav};
      c.sub = static_cast<int>(i);
      float top = kNavTop + kNavStep * static_cast<float>(i);
      c.hit = c.box = Rect(8.f, top, kSidebarW - 8.f, top + kNavH);
      c.content = false;
      c.key = MakeKey(CtlKind::Nav, 0, static_cast<int>(i));
      c.focus = MakeKey(CtlKind::NavStop, 0, kNoSub);
      ctls.push_back(c);
    }
    {
      float top = kNavTop + kNavStep * static_cast<float>(section);
      stops.push_back({MakeKey(CtlKind::NavStop, 0, kNoSub), CtlKind::NavStop, nullptr, kNoSub,
                       Rect(8.f, top, kSidebarW - 8.f, top + kNavH), false, 5.f});
    }
    float margin = width - kSidebarW >= 640.f ? 40.f : 24.f;
    cx = kSidebarW + margin;
    cw = std::max(240.f, std::min(width - cx - margin, kMaxCardW));
    float y = kRowsTop;
    for (const SchemaItem* it : SectionItems(secs[static_cast<size_t>(section)].id)) {
      if (it->kind == ItemKind::MonitorList) {
        if (monitors.empty()) AddRow(*it, kNoSub, y);
        for (size_t i = 0; i < monitors.size(); ++i) AddRow(*it, static_cast<int>(i), y);
        continue;
      }
      AddRow(*it, kNoSub, y);
    }
    contentBottom = y - kRowGap + 24.f;
    scroll = std::clamp(scroll, 0.f, MaxScroll());
    UpdateHover();
  }

  float MaxScroll() const { return std::max(0.f, contentBottom - height); }

  // ---- hit testing ----

  int HitCtl(float x, float y) const {
    for (size_t i = ctls.size(); i-- > 0;) {
      const Ctl& c = ctls[i];
      if (c.content) {
        if (y < kRowsTop - 8.f || x < kSidebarW) continue;
        if (Contains(c.hit, x, y + scroll)) return static_cast<int>(i);
      } else if (Contains(c.hit, x, y)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int HitRow(float x, float y) const {
    if (y < kRowsTop - 8.f) return -1;
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      if (r.hover && Contains(Rect(cx, r.y, cx + cw, r.y + r.h), x, y + scroll)) return static_cast<int>(i);
    }
    return -1;
  }

  const Ctl* FindCtl(uint64_t key) const {
    for (const auto& c : ctls)
      if (c.key == key) return &c;
    return nullptr;
  }

  int FindStop(uint64_t key) const {
    for (size_t i = 0; i < stops.size(); ++i)
      if (stops[i].key == key) return static_cast<int>(i);
    return -1;
  }

  void UpdateHover() {
    int ci = mouseX < 0 ? -1 : HitCtl(mouseX, mouseY);
    int ri = mouseX < 0 ? -1 : HitRow(mouseX, mouseY);
    uint64_t hk = ci >= 0 ? ctls[static_cast<size_t>(ci)].key : 0;
    if (hk != hoverKey || ri != hoverRow) {
      hoverKey = hk;
      hoverRow = ri;
      Invalidate();
    }
  }

  // ---- actions ----

  void SelectSection(int i) {
    int n = static_cast<int>(SchemaSections().size());
    i = std::clamp(i, 0, n - 1);
    if (i == section) return;
    EndCapture();
    section = i;
    scroll = 0;
    RefreshStatus(true);
  }

  void StartCapture(ItemId id) {
    if (capturing == id) return;
    EndCapture();
    capturing = id;
    if (host.hotkeyCapture) host.hotkeyCapture(true);
    Layout();
    Invalidate();
  }

  void EndCapture() {
    if (capturing == ItemId::Count) return;
    capturing = ItemId::Count;
    if (host.hotkeyCapture) host.hotkeyCapture(false);
    Layout();
    Invalidate();
  }

  bool CaptureKey(UINT vk) {
    if (vk == VK_ESCAPE) {
      EndCapture();
      return true;
    }
    std::string key;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) key = std::string(1, static_cast<char>(vk));
    else if (vk >= VK_F1 && vk <= VK_F24) key = "F" + std::to_string(vk - VK_F1 + 1);
    else return true;  // modifiers on their own and unsupported keys: keep waiting
    bool c = GetKeyState(VK_CONTROL) < 0, a = GetKeyState(VK_MENU) < 0, s = GetKeyState(VK_SHIFT) < 0;
    bool w = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    std::string text = FormatHotkey(c, a, s, w, key);
    if (!IsValidHotkey(text)) return true;  // e.g. no modifier: the hint below the row explains
    ItemId id = capturing;
    Settings st = host.get ? host.get() : settings;
    if (SetHotkey(st, id, text)) Commit(st);
    EndCapture();
    return true;
  }

  void RunAction(const std::string& key) {
    bool ok = false;
    UiAction a = ActionFor(key, ok);
    if (!ok || !host.action) return;
    if (a == UiAction::ResetDefaults) {
      std::wstring msg = T(UiStr::ConfirmReset), title = W(Tr(Str::AppName, lang));
      if (MessageBoxW(hwnd, msg.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }
    // Decorating happens on the desktop, so get out of the way.
    if (a == UiAction::Decorate) Hide();
    host.action(a);
    Reload();
    RefreshStatus(true);
  }

  void Activate(const Ctl& c) {
    const SchemaItem* it = c.item;
    switch (c.kind) {
      case CtlKind::Nav:
        SelectSection(c.sub);
        break;
      case CtlKind::Toggle:
        if (it->session) {
          if (host.setDiagnosticsLogging) host.setDiagnosticsLogging(!status.diagnosticsLogging);
          RefreshStatus(true);
        } else {
          Settings s = host.get();
          SetToggle(s, it->id, !GetToggle(settings, it->id));
          Commit(s);
        }
        break;
      case CtlKind::Monitor: {
        if (c.sub < 0 || static_cast<size_t>(c.sub) >= monitors.size()) break;
        const std::string id = monitors[static_cast<size_t>(c.sub)].id;
        Settings s = host.get();
        SetMonitorEnabled(s, id, !IsMonitorEnabled(settings, id));
        Commit(s);
        break;
      }
      case CtlKind::Segment:
      case CtlKind::Swatch: {
        Settings s = host.get();
        if (SetChoice(s, it->id, it->options[static_cast<size_t>(c.sub)].value)) Commit(s);
        break;
      }
      case CtlKind::Hotkey:
        if (capturing == it->id) EndCapture();
        else StartCapture(it->id);
        break;
      case CtlKind::Button:
        RunAction(it->options[static_cast<size_t>(c.sub)].value);
        break;
      default:
        break;
    }
  }

  void SetSliderValue(const SchemaItem& it, double v) {
    Settings s = host.get();
    if (!SetSlider(s, it.id, v)) return;
    if (std::fabs(GetSlider(s, it.id) - GetSlider(settings, it.id)) < 1e-9) return;
    Commit(s);
  }

  void DragSlider(float x) {
    const Ctl* c = FindCtl(dragKey);
    if (!c) return;
    const SchemaItem& it = *c->item;
    double t = std::clamp(static_cast<double>((x - c->box.left) / std::max(1.f, c->box.right - c->box.left)), 0.0, 1.0);
    SetSliderValue(it, it.min + t * (it.max - it.min));
  }

  void StepChoice(const SchemaItem& it, int d) {
    int n = static_cast<int>(it.options.size());
    int i = ChoiceIndex(settings, it);
    int next = i < 0 ? (d > 0 ? 0 : n - 1) : std::clamp(i + d, 0, n - 1);
    if (next == i) return;
    Settings s = host.get();
    if (SetChoice(s, it.id, it.options[static_cast<size_t>(next)].value)) Commit(s);
  }

  void EnsureVisible(const D2D1_RECT_F& r) {
    if (r.top - scroll < kRowsTop) scroll = r.top - kRowsTop;
    if (r.bottom - scroll > height - 12.f) scroll = r.bottom - height + 12.f;
    scroll = std::clamp(scroll, 0.f, MaxScroll());
  }

  void MoveFocus(int d) {
    if (stops.empty()) return;
    int n = static_cast<int>(stops.size());
    int i = FindStop(focus);
    i = i < 0 ? (d > 0 ? 0 : n - 1) : (i + d + n) % n;
    focus = stops[static_cast<size_t>(i)].key;
    if (stops[static_cast<size_t>(i)].content) EnsureVisible(stops[static_cast<size_t>(i)].rect);
    UpdateHover();
    Invalidate();
  }

  void ScrollBy(float dy) {
    float s = std::clamp(scroll + dy, 0.f, MaxScroll());
    if (s == scroll) return;
    scroll = s;
    UpdateHover();
    Invalidate();
  }

  // Returns false when the key should go to DefWindowProc.
  bool OnKey(UINT vk) {
    if (capturing != ItemId::Count) return CaptureKey(vk);
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int n = static_cast<int>(SchemaSections().size());
    int si = FindStop(focus);
    const Stop* st = si >= 0 ? &stops[static_cast<size_t>(si)] : nullptr;
    const bool onNav = st && st->kind == CtlKind::NavStop;
    const bool onSlider = st && st->kind == CtlKind::Slider;
    const bool onGroup = st && (st->kind == CtlKind::Segment || st->kind == CtlKind::Swatch);
    auto sliderStep = [&](double steps) {
      double v = GetSlider(settings, st->item->id) + steps * st->item->step;
      SetSliderValue(*st->item, v);
    };
    switch (vk) {
      case VK_TAB:
        focusVisible = true;
        if (ctrl) SelectSection((section + (shift ? n - 1 : 1)) % n);
        else MoveFocus(shift ? -1 : 1);
        Invalidate();
        return true;
      case VK_ESCAPE:
        Hide();
        return true;
      case VK_UP:
      case VK_DOWN: {
        int d = vk == VK_UP ? -1 : 1;
        focusVisible = true;
        if (onNav) SelectSection(section + d);
        else if (onSlider) sliderStep(-d);
        else MoveFocus(d);
        Invalidate();
        return true;
      }
      case VK_LEFT:
      case VK_RIGHT: {
        int d = vk == VK_LEFT ? -1 : 1;
        focusVisible = true;
        if (onSlider) sliderStep(d);
        else if (onGroup) StepChoice(*st->item, d);
        Invalidate();
        return true;
      }
      case VK_PRIOR:
      case VK_NEXT: {
        int d = vk == VK_PRIOR ? -1 : 1;
        if (onSlider) sliderStep(10.0 * d);
        else ScrollBy(static_cast<float>(d) * (height - kRowsTop) * 0.8f);
        return true;
      }
      case VK_HOME:
      case VK_END: {
        bool end = vk == VK_END;
        if (onSlider) SetSliderValue(*st->item, end ? st->item->max : st->item->min);
        else if (onNav) SelectSection(end ? n - 1 : 0);
        else ScrollBy(end ? MaxScroll() : -scroll);
        return true;
      }
      case VK_SPACE:
      case VK_RETURN: {
        focusVisible = true;
        if (!st || onNav || onSlider || onGroup) return true;
        const Ctl* c = FindCtl(st->key);
        if (c) {
          Ctl copy = *c;  // Activate rebuilds the layout
          Activate(copy);
        }
        Invalidate();
        return true;
      }
      default:
        return false;
    }
  }

  void OnMouseMove(float x, float y) {
    mouseX = x;
    mouseY = y;
    if (dragKey) {
      DragSlider(x);
      return;
    }
    UpdateHover();
  }

  void OnMouseDown(float x, float y) {
    mouseX = x;
    mouseY = y;
    int ci = HitCtl(x, y);
    uint64_t key = ci >= 0 ? ctls[static_cast<size_t>(ci)].key : 0;
    const Ctl* hk = ci >= 0 ? &ctls[static_cast<size_t>(ci)] : nullptr;
    if (capturing != ItemId::Count && !(hk && hk->kind == CtlKind::Hotkey && hk->item->id == capturing)) EndCapture();
    focusVisible = false;
    pressKey = 0;
    ci = HitCtl(x, y);  // EndCapture may have re-laid out the page
    if (ci < 0) {
      Invalidate();
      return;
    }
    const Ctl& c = ctls[static_cast<size_t>(ci)];
    key = c.key;
    focus = c.focus;
    pressKey = key;
    if (hwnd) SetCapture(hwnd);
    if (c.kind == CtlKind::Slider) {
      dragKey = key;
      DragSlider(x);
    }
    Invalidate();
  }

  void OnMouseUp(float x, float y) {
    mouseX = x;
    mouseY = y;
    uint64_t pressed = pressKey;
    pressKey = 0;
    bool wasDrag = dragKey != 0;
    dragKey = 0;
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
    if (!wasDrag && pressed) {
      int ci = HitCtl(x, y);
      if (ci >= 0 && ctls[static_cast<size_t>(ci)].key == pressed) {
        Ctl copy = ctls[static_cast<size_t>(ci)];
        Activate(copy);
      }
    }
    UpdateHover();
    Invalidate();
  }

  // ---- painting ----

  float A(uint64_t key, float target, float speed) {
    auto [itr, inserted] = anims.try_emplace(key);
    Anim& a = itr->second;
    if (inserted) a.v = target;
    a.target = target;
    a.speed = speed;
    if (std::fabs(a.v - a.target) > 1e-3f) animating = true;
    return a.v;
  }

  void Tick(float dt) {
    for (auto& kv : anims) {
      Anim& a = kv.second;
      float step = a.speed * dt;
      if (a.v < a.target) a.v = std::min(a.target, a.v + step);
      else if (a.v > a.target) a.v = std::max(a.target, a.v - step);
    }
  }

  ID2D1SolidColorBrush* B(const D2D1_COLOR_F& c) {
    brush->SetColor(c);
    return brush.Get();
  }
  void FillRR(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c) {
    if (c.a <= 0.f) return;
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), B(c));
  }
  void StrokeRR(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c, float w) {
    if (c.a <= 0.f) return;
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(r, -w / 2.f), rad, rad), B(c), w);
  }
  void Text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (s.empty() || !fmt) return;
    rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, B(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  void BindTarget(ID2D1RenderTarget* t) {
    rt = t;
    if (boundRt == t && brush) return;
    boundRt = t;
    brush.Reset();
    icon.Reset();
    iconTried = false;
    t->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 1.f), &brush);
  }

  void DrawSnowflake(const D2D1_RECT_F& r) {
    float x = (r.left + r.right) / 2.f, y = (r.top + r.bottom) / 2.f, R = (r.right - r.left) * 0.42f;
    ID2D1SolidColorBrush* b = B(Hex(0xFFFFFF));
    for (int i = 0; i < 6; ++i) {
      float a = static_cast<float>(i) * 3.14159265f / 3.f;
      float dx = std::cos(a), dy = std::sin(a);
      rt->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + dx * R, y + dy * R), b, 2.2f);
      for (int s = -1; s <= 1; s += 2) {
        float ba = a + static_cast<float>(s) * 0.7f;
        float bx = x + dx * R * 0.6f, by = y + dy * R * 0.6f;
        rt->DrawLine(D2D1::Point2F(bx, by), D2D1::Point2F(bx + std::cos(ba) * R * 0.3f, by + std::sin(ba) * R * 0.3f),
                     b, 1.6f);
      }
    }
  }

  void DrawAppIcon(const D2D1_RECT_F& r) {
    if (!iconTried) {
      iconTried = true;
      HICON hi = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
      ComPtr<IWICBitmap> bmp;
      ComPtr<IWICFormatConverter> conv;
      if (hi && wic && SUCCEEDED(wic->CreateBitmapFromHICON(hi, &bmp)) &&
          SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
          SUCCEEDED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &icon);
      if (hi) DestroyIcon(hi);
    }
    // The icon is a white snowflake: give it a tinted tile so it also reads on the light theme.
    float pad = (r.right - r.left) * 0.14f;
    FillRR(Inflate(r, pad), (r.right - r.left) * 0.25f,
           pal.dark ? Fade(pal.accent, 0.22f) : Lerp(pal.accent, Hex(0x0B1E33), 0.45f));
    if (icon) rt->DrawBitmap(icon.Get(), r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    else DrawSnowflake(r);
  }

  void FocusRing(const D2D1_RECT_F& r, float radius) {
    StrokeRR(Inflate(r, 3.f), radius + 3.f, pal.focus, 2.f);
  }

  void PaintSidebar() {
    DrawAppIcon(Rect(22.f, 26.f, 54.f, 58.f));
    Text(W(Tr(Str::AppName, lang)), fonts.app.Get(), Rect(66.f, 24.f, kSidebarW - 12.f, 46.f), pal.text);
    Text(W(Tr(Str::Tagline, lang)), fonts.caption.Get(), Rect(66.f, 46.f, kSidebarW - 12.f, 64.f), pal.text2);
    const auto& secs = SchemaSections();
    for (const Ctl& c : ctls) {
      if (c.kind != CtlKind::Nav) continue;
      bool sel = c.sub == section;
      float h = A(c.key, hoverKey == c.key ? 1.f : 0.f, kHoverSpeed);
      D2D1_COLOR_F fill = sel ? pal.navSel : Fade(pal.navHover, h);
      if (sel && h > 0.f) fill = Lerp(pal.navSel, Fade(pal.navSel, 1.5f), h);
      FillRR(c.box, 5.f, fill);
      float cy = (c.box.top + c.box.bottom) / 2.f;
      float pill = A(c.key ^ kToggleAnimBit, sel ? 1.f : 0.f, kToggleSpeed);
      if (pill > 0.f) {
        float ph = 16.f * Ease(pill);
        FillRR(Rect(c.box.left, cy - ph / 2.f, c.box.left + 3.f, cy + ph / 2.f), 1.5f, pal.accent);
      }
      const SectionDef& sd = secs[static_cast<size_t>(c.sub)];
      wchar_t glyph[2] = {static_cast<wchar_t>(sd.icon), 0};
      Text(glyph, fonts.icon.Get(), Rect(c.box.left + 10.f, c.box.top, c.box.left + 38.f, c.box.bottom), pal.text);
      Text(T(sd.title), fonts.line.Get(), Rect(c.box.left + 48.f, c.box.top, c.box.right - 8.f, c.box.bottom),
           pal.text);
    }
    if (focusVisible) {
      int si = FindStop(focus);
      if (si >= 0 && !stops[static_cast<size_t>(si)].content) FocusRing(stops[static_cast<size_t>(si)].rect, 5.f);
    }
  }

  void PaintToggle(const Ctl& c, bool on, bool rowHover) {
    float t = Ease(A(c.key ^ kToggleAnimBit, on ? 1.f : 0.f, kToggleSpeed));
    const D2D1_RECT_F& b = c.box;
    float cy = (b.top + b.bottom) / 2.f;
    FillRR(b, 10.f, Fade(rowHover ? pal.ctrlHover : pal.ctrl, 1.f - t));
    StrokeRR(b, 9.5f, Fade(pal.strong, 1.f - t), 1.f);
    FillRR(b, 10.f, Fade(rowHover ? pal.accentHover : pal.accent, t));
    float knob = (rowHover ? 14.f : 12.f) / 2.f;
    float kx = b.left + 10.f + 20.f * t;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), knob, knob), B(Lerp(pal.strong, pal.onAccent, t)));
    Text(T(on ? UiStr::OptOn : UiStr::OptOff), fonts.lineRight.Get(),
         Rect(b.left - 90.f, cy - 12.f, b.left - 12.f, cy + 12.f), pal.text);
  }

  void PaintSlider(const Ctl& c) {
    const SchemaItem& it = *c.item;
    double v = GetSlider(settings, it.id);
    float span = static_cast<float>(it.max - it.min);
    float frac = span > 0.f ? static_cast<float>(v - it.min) / span : 0.f;
    float origin = it.min < 0.0 ? static_cast<float>(-it.min) / span : 0.f;  // wind fills from calm
    const D2D1_RECT_F& b = c.box;
    float w = b.right - b.left, cy = (b.top + b.bottom) / 2.f;
    float x = b.left + w * frac, x0 = b.left + w * origin;
    FillRR(b, 2.f, pal.track);
    FillRR(Rect(std::min(x, x0), b.top, std::max(x, x0), b.bottom), 2.f, pal.accent);
    float h = A(c.key, hoverKey == c.key || dragKey == c.key ? 1.f : 0.f, kHoverSpeed);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 10.f, 10.f), B(pal.thumb));
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 10.f, 10.f), B(pal.ctrlBorder), 1.f);
    float inner = dragKey == c.key ? 5.f : 6.f + 1.f * h;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), inner, inner), B(pal.accent));
    Text(W(FormatSliderValue(it.id, v, lang)), fonts.line.Get(),
         Rect(b.right + 14.f, cy - 12.f, b.right + 12.f + kValueW, cy + 12.f), pal.text);
  }

  void PaintGroup(const Row& r) {
    const SchemaItem& it = *r.item;
    if (it.kind == ItemKind::Choice) {
      FillRR(r.group, 6.f, pal.ctrl);
      StrokeRR(r.group, 6.f, pal.ctrlBorder, 1.f);
    }
    int sel = ChoiceIndex(settings, it);
    for (const Ctl& c : ctls) {
      if (c.item != &it || (c.kind != CtlKind::Segment && c.kind != CtlKind::Swatch)) continue;
      bool on = c.sub == sel;
      float h = A(c.key, hoverKey == c.key ? 1.f : 0.f, kHoverSpeed);
      float s = A(c.key ^ kToggleAnimBit, on ? 1.f : 0.f, kToggleSpeed);
      const ChoiceOption& o = it.options[static_cast<size_t>(c.sub)];
      if (c.kind == CtlKind::Segment) {
        FillRR(c.box, 4.f, Fade(pal.ctrlHover, h * (1.f - s) * 1.5f));
        FillRR(c.box, 4.f, Fade(on && h > 0.f ? pal.accentHover : pal.accent, s));
        Text(W(OptionText(o, lang)), fonts.center.Get(), c.box, Lerp(pal.text, pal.onAccent, s));
      } else {
        Color col;
        ParseHexColor(o.value, col);
        D2D1_POINT_2F ctr = D2D1::Point2F((c.box.left + c.box.right) / 2.f, (c.box.top + c.box.bottom) / 2.f);
        rt->FillEllipse(D2D1::Ellipse(ctr, 12.f, 12.f), B(D2D1::ColorF(col.r, col.g, col.b, 1.f)));
        rt->DrawEllipse(D2D1::Ellipse(ctr, 12.f, 12.f), B(Fade(pal.text, 0.3f)), 1.f);
        if (s > 0.f) rt->DrawEllipse(D2D1::Ellipse(ctr, 16.f, 16.f), B(Fade(pal.accent, s)), 2.5f);
        if (h > 0.f && !on) rt->DrawEllipse(D2D1::Ellipse(ctr, 16.f, 16.f), B(Fade(pal.text2, 0.6f * h)), 1.5f);
      }
    }
  }

  void PaintButton(const Ctl& c) {
    bool accentStyle = c.item->accent && c.sub == 0;
    float h = A(c.key, hoverKey == c.key ? 1.f : 0.f, kHoverSpeed);
    bool pressed = pressKey == c.key && hoverKey == c.key;
    if (accentStyle) {
      FillRR(c.box, 5.f, Fade(Lerp(pal.accent, pal.accentHover, h), pressed ? 0.8f : 1.f));
    } else {
      FillRR(c.box, 5.f, Lerp(pal.ctrl, pal.ctrlHover, h));
      StrokeRR(c.box, 5.f, pal.ctrlBorder, 1.f);
    }
    D2D1_COLOR_F tc = accentStyle ? pal.onAccent : pal.text;
    Text(W(OptionText(c.item->options[static_cast<size_t>(c.sub)], lang)), fonts.center.Get(), c.box,
         pressed && !accentStyle ? pal.text2 : tc);
  }

  void PaintHotkey(const Ctl& c) {
    bool cap = capturing == c.item->id;
    float h = A(c.key, hoverKey == c.key ? 1.f : 0.f, kHoverSpeed);
    FillRR(c.box, 5.f, Lerp(pal.ctrl, pal.ctrlHover, h));
    if (cap) StrokeRR(c.box, 5.f, pal.accent, 2.f);
    else StrokeRR(c.box, 5.f, pal.ctrlBorder, 1.f);
    std::wstring s = cap ? T(UiStr::HotkeyPress) : W(HotkeyDisplay(GetHotkey(settings, c.item->id)));
    Text(s, fonts.center.Get(), Inflate(c.box, -6.f), cap ? pal.text2 : pal.text);
  }

  void PaintAbout(const Row& r) {
    float left = cx + kPadX, top = r.y + kPadY + 4.f;
    DrawAppIcon(Rect(left, top, left + 56.f, top + 56.f));
    float x = left + 76.f, right = cx + cw - kPadX;
    Text(W(Tr(Str::AppName, lang)), fonts.header.Get(), Rect(x, top, right, top + 28.f), pal.text);
    Text(W(Tr(Str::Tagline, lang)), fonts.line.Get(), Rect(x, top + 28.f, right, top + 50.f), pal.text2);
    std::string build = status.build.empty() ? "dev" : status.build;
    Text(W(UiFormat(UiTr(UiStr::AboutBuild, lang), {build})), fonts.caption.Get(),
         Rect(x, top + 52.f, right, top + 70.f), pal.text2);
    if (r.desc) rt->DrawTextLayout(D2D1::Point2F(x, top + 82.f), r.desc.Get(), B(pal.text));
  }

  void PaintRow(size_t index) {
    const Row& r = rows[index];
    D2D1_RECT_F card = Rect(cx, r.y, cx + cw, r.y + r.h);
    float h = r.hover ? A(r.key, hoverRow == static_cast<int>(index) ? 1.f : 0.f, kHoverSpeed) : 0.f;
    FillRR(card, kRadius, Lerp(pal.card, pal.cardHover, h));
    StrokeRR(card, kRadius, pal.cardBorder, 1.f);
    if (r.item->kind == ItemKind::AboutHeader) {
      PaintAbout(r);
      return;
    }
    float left = cx + kPadX;
    if (r.label) rt->DrawTextLayout(D2D1::Point2F(left, r.textTop), r.label.Get(), B(pal.text));
    if (r.desc) rt->DrawTextLayout(D2D1::Point2F(left, r.textTop + r.labelH + 2.f), r.desc.Get(), B(pal.text2));
    if (!r.lines.empty()) {
      float y = r.textTop + r.labelH + (r.desc ? 2.f + r.descH : 0.f) + 8.f;
      for (const auto& line : r.lines) {
        Text(line, fonts.mono.Get(), Rect(left, y, cx + cw - kPadX, y + 18.f), pal.text);
        y += 18.f;
      }
    }
    if (r.item->kind == ItemKind::Choice || r.item->kind == ItemKind::Swatches) {
      PaintGroup(r);
      return;
    }
    for (const Ctl& c : ctls) {
      if (c.row != static_cast<int>(index)) continue;
      bool rowHover = hoverRow == static_cast<int>(index);
      switch (c.kind) {
        case CtlKind::Toggle:
        case CtlKind::Monitor: PaintToggle(c, ToggleValue(*c.item, c.sub), rowHover); break;
        case CtlKind::Slider: PaintSlider(c); break;
        case CtlKind::Hotkey: PaintHotkey(c); break;
        case CtlKind::Button: PaintButton(c); break;
        default: break;
      }
    }
  }

  void Paint(ID2D1RenderTarget* t) {
    BindTarget(t);
    animating = false;
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    if (!fontsReady || !brush) {
      t->Clear(pal.window);
      return;
    }
    t->SetTextAntialiasMode(pal.mica ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
    if (pal.mica) {
      t->Clear(D2D1::ColorF(0, 0, 0, 0));
      t->FillRectangle(Rect(kSidebarW, 0.f, width, height), B(pal.layer));
    } else {
      t->Clear(pal.window);
      t->FillRectangle(Rect(0.f, 0.f, kSidebarW, height), B(pal.sidebar));
    }
    PaintSidebar();
    const SectionDef& sd = SchemaSections()[static_cast<size_t>(section)];
    Text(T(sd.title), fonts.title.Get(), Rect(cx, kTitleTop, width - 24.f, kTitleTop + 44.f), pal.text);

    t->PushAxisAlignedClip(Rect(kSidebarW, kRowsTop - 8.f, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
    t->SetTransform(D2D1::Matrix3x2F::Translation(0.f, -scroll));
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      if (r.y + r.h < scroll + kRowsTop - 8.f || r.y > scroll + height) continue;
      PaintRow(i);
    }
    if (focusVisible) {
      int si = FindStop(focus);
      if (si >= 0 && stops[static_cast<size_t>(si)].content)
        FocusRing(stops[static_cast<size_t>(si)].rect, stops[static_cast<size_t>(si)].radius);
    }
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    t->PopAxisAlignedClip();

    float max = MaxScroll();
    if (max > 0.f) {
      float top = kRowsTop - 4.f, bottom = height - 6.f, track = bottom - top;
      float viewH = height - kRowsTop;
      float thumb = std::max(32.f, track * viewH / (viewH + max));
      float y = top + (track - thumb) * (scroll / max);
      FillRR(Rect(width - 9.f, y, width - 5.f, y + thumb), 2.f, Fade(pal.text2, 0.55f));
    }
  }

  // ---- window ----

  void DiscardTarget() {
    hwndRt.Reset();
    if (boundRt) {
      boundRt = nullptr;
      brush.Reset();
      icon.Reset();
      iconTried = false;
    }
  }

  bool EnsureTarget() {
    if (hwndRt) return true;
    if (!d2d || !hwnd) return false;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, mica ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi), static_cast<float>(dpi));
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right)),
                          static_cast<UINT32>(std::max<LONG>(1, rc.bottom))));
    if (FAILED(d2d->CreateHwndRenderTarget(props, hp, &hwndRt))) return false;
    hwndRt->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    return true;
  }

  float Scale() const { return static_cast<float>(dpi) / 96.f; }

  void UpdateSize() {
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;
    width = static_cast<float>(rc.right) / Scale();
    height = static_cast<float>(rc.bottom) / Scale();
    if (hwndRt) hwndRt->Resize(D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
    Layout();
  }

  void OnPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    if (EnsureTarget()) {
      hwndRt->BeginDraw();
      Paint(hwndRt.Get());
      if (hwndRt->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        DiscardTarget();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
    }
    EndPaint(hwnd, &ps);
    if (animating && !animTimer) {
      animTimer = true;
      lastTick = GetTickCount64();
      SetTimer(hwnd, kAnimTimer, 16, nullptr);
    } else if (!animating && animTimer) {
      animTimer = false;
      KillTimer(hwnd, kAnimTimer);
    }
  }

  void StartStatusTimer() {
    if (hwnd && !statusTimer) {
      statusTimer = true;
      SetTimer(hwnd, kStatusTimer, 500, nullptr);
    }
  }

  void StopTimers() {
    if (!hwnd) return;
    KillTimer(hwnd, kAnimTimer);
    KillTimer(hwnd, kStatusTimer);
    animTimer = statusTimer = false;
  }

  RECT WindowRectFor(float wDip, float hDip, UINT forDpi) const {
    float s = static_cast<float>(forDpi) / 96.f;
    RECT r{0, 0, static_cast<LONG>(std::lround(wDip * s)), static_cast<LONG>(std::lround(hDip * s))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, forDpi);
    return r;
  }

  bool Create() {
    if (!InitFactories()) return false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    if (!GetClassInfoExW(inst, kClassName, &wc)) {
      wc = WNDCLASSEXW{};
      wc.cbSize = sizeof wc;
      wc.style = CS_HREDRAW | CS_VREDRAW;
      wc.lpfnWndProc = &Impl::WndProc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
      wc.lpszClassName = kClassName;
      if (!RegisterClassExW(&wc)) return false;
    }
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    RECT r = WindowRectFor(kDefaultW, kDefaultH, dx);
    const RECT& work = mi.rcWork;
    int w = std::min<int>(r.right - r.left, work.right - work.left);
    int h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
    int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    Reload();
    hwnd = CreateWindowExW(0, kClassName, Title().c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst,
                           this);
    if (!hwnd) return false;
    dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
    iconBig = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                            GetSystemMetricsForDpi(SM_CXICON, dpi),
                                            GetSystemMetricsForDpi(SM_CYICON, dpi), LR_DEFAULTCOLOR));
    iconSmall = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                              GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                              GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    if (iconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
    if (iconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    ReadTheme();
    ApplyDwm();
    UpdateSize();
    return true;
  }

  // Keeps the (topmost) snow overlay from drawing over the window while it is in use.
  void OnActivate(bool active, HWND other) {
    if (active) {
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      return;
    }
    EndCapture();
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    // HWND_NOTOPMOST lifts us above every normal window; go back behind the window that took the focus.
    HWND fg = other ? GetAncestor(other, GA_ROOT) : GetForegroundWindow();
    if (fg && fg != hwnd && !(GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST))
      SetWindowPos(hwnd, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }

  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
    const float s = Scale();
    auto px = [&](LPARAM l) { return static_cast<float>(static_cast<short>(LOWORD(l))) / s; };
    auto py = [&](LPARAM l) { return static_cast<float>(static_cast<short>(HIWORD(l))) / s; };
    switch (msg) {
      case WM_PAINT:
        OnPaint();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_SIZE:
        UpdateSize();
        Invalidate();
        return 0;
      case WM_GETMINMAXINFO: {
        UINT d = hwnd ? GetDpiForWindow(hwnd) : 96;
        RECT r = WindowRectFor(kMinW, kMinH, d ? d : 96);
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
      }
      case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        if (hwndRt) hwndRt->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateSize();
        Invalidate();
        return 0;
      }
      case WM_SETTINGCHANGE:
        if (lp && std::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorScheme") == 0) {
          ReadTheme();
          ApplyDwm();
          Invalidate();
        }
        break;
      case WM_DWMCOLORIZATIONCOLORCHANGED:
      case WM_THEMECHANGED:
        ReadTheme();
        ApplyDwm();
        Invalidate();
        break;
      case WM_ACTIVATE:
        OnActivate(LOWORD(wp) != WA_INACTIVE, reinterpret_cast<HWND>(lp));
        if (LOWORD(wp) != WA_INACTIVE) SetFocus(hwnd);
        return 0;
      case WM_MOUSEMOVE:
        if (!trackingMouse) {
          TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
          trackingMouse = TrackMouseEvent(&tme) != FALSE;
        }
        OnMouseMove(px(lp), py(lp));
        return 0;
      case WM_MOUSELEAVE:
        trackingMouse = false;
        if (!dragKey) {
          mouseX = mouseY = -1;
          UpdateHover();
        }
        return 0;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        OnMouseDown(px(lp), py(lp));
        return 0;
      case WM_LBUTTONUP:
        OnMouseUp(px(lp), py(lp));
        return 0;
      case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != hwnd && (dragKey || pressKey)) {
          dragKey = pressKey = 0;
          Invalidate();
        }
        return 0;
      case WM_MOUSEWHEEL:
        ScrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA) * 64.f);
        return 0;
      case WM_KEYDOWN:
        if (OnKey(static_cast<UINT>(wp))) return 0;
        break;
      case WM_SYSKEYDOWN:
        if (capturing != ItemId::Count && OnKey(static_cast<UINT>(wp))) return 0;
        break;
      case WM_SYSCHAR:
        if (capturing != ItemId::Count) return 0;  // no menu beep while typing a shortcut
        break;
      case WM_KILLFOCUS:
        EndCapture();
        break;
      case WM_TIMER:
        if (wp == kAnimTimer) {
          ULONGLONG now = GetTickCount64();
          Tick(static_cast<float>(now - lastTick) / 1000.f);
          lastTick = now;
          Invalidate();
        } else if (wp == kStatusTimer) {
          if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) RefreshStatus(false);
        }
        return 0;
      case WM_CLOSE:
        Hide();
        return 0;
      case WM_NCDESTROY:
        DiscardTarget();
        hwnd = nullptr;
        animTimer = statusTimer = false;
        break;
      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      if (self) self->hwnd = h;
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->Handle(msg, wp, lp);
  }

  void Show() {
    if (!hwnd && !Create()) return;
    Reload();
    RefreshStatus(true);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    StartStatusTimer();
    Invalidate();
  }

  void Hide() {
    EndCapture();
    dragKey = pressKey = 0;
    if (!hwnd) return;
    StopTimers();
    ShowWindow(hwnd, SW_HIDE);
  }

  void DestroyAll() {
    EndCapture();
    if (hwnd) DestroyWindow(hwnd);
    hwnd = nullptr;
    DiscardTarget();
    if (iconBig) DestroyIcon(iconBig);
    if (iconSmall) DestroyIcon(iconSmall);
    iconBig = iconSmall = nullptr;
  }
};

// ---- public API ---------------------------------------------------------------------------------------------

SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}
SettingsWindow::~SettingsWindow() { impl_->DestroyAll(); }

void SettingsWindow::Init(HINSTANCE inst, SettingsHost host) {
  impl_->inst = inst;
  impl_->host = std::move(host);
}
void SettingsWindow::Show() { impl_->Show(); }
void SettingsWindow::Hide() { impl_->Hide(); }
void SettingsWindow::Destroy() { impl_->DestroyAll(); }
bool SettingsWindow::IsOpen() const { return impl_->hwnd && IsWindowVisible(impl_->hwnd); }
HWND SettingsWindow::Hwnd() const { return impl_->hwnd; }

void SettingsWindow::Refresh() {
  Impl& m = *impl_;
  if (!m.hwnd) return;
  m.Reload();
  m.RefreshStatus(true);
}

// ---- offscreen snapshots ------------------------------------------------------------------------------------

namespace {

// Records the first failing step so a CI log says what went wrong.
struct StepCheck {
  const char* failed = nullptr;
  HRESULT hr = S_OK;
  bool operator()(HRESULT h, const char* what) {
    if (failed) return false;
    if (FAILED(h)) {
      failed = what;
      hr = h;
      return false;
    }
    return true;
  }
};

bool SavePng(IWICImagingFactory* wic, IWICBitmap* bmp, const std::wstring& path, StepCheck& ok) {
  UINT w = 0, h = 0;
  bmp->GetSize(&w, &h);
  ComPtr<IWICFormatConverter> conv;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  return ok(wic->CreateFormatConverter(&conv), "CreateFormatConverter") &&
         ok(conv->Initialize(bmp, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom),
            "FormatConverter::Initialize") &&
         ok(wic->CreateStream(&stream), "CreateStream") &&
         ok(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "InitializeFromFilename") &&
         ok(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "CreateEncoder") &&
         ok(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Encoder::Initialize") &&
         ok(enc->CreateNewFrame(&frame, nullptr), "CreateNewFrame") &&
         ok(frame->Initialize(nullptr), "Frame::Initialize") &&
         ok(frame->SetSize(w, h), "SetSize") && ok(frame->SetPixelFormat(&fmt), "SetPixelFormat") &&
         ok(frame->WriteSource(conv.Get(), nullptr), "WriteSource") && ok(frame->Commit(), "Frame::Commit") &&
         ok(enc->Commit(), "Encoder::Commit");
}

}  // namespace

int RenderSettingsSnapshots(const std::wstring& dir, const char* buildId) {
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  auto demo = std::make_shared<Settings>();
  demo->disabledMonitors.push_back("\\\\.\\DISPLAY2");
  std::string build = buildId ? buildId : "dev";

  SettingsWindow::Impl m;
  m.inst = GetModuleHandleW(nullptr);
  m.host.get = [demo] { return *demo; };
  m.host.set = [demo](const Settings& s) {
    *demo = s;
    Sanitize(*demo);
  };
  m.host.action = [](UiAction) {};
  m.host.monitors = [] {
    std::vector<UiMonitor> v(2);
    v[0].id = "\\\\.\\DISPLAY1";
    v[0].width = 1920;
    v[0].height = 1080;
    v[0].scale = 1.25f;
    v[0].primary = true;
    v[1].id = "\\\\.\\DISPLAY2";
    v[1].width = 2560;
    v[1].height = 1440;
    v[1].scale = 1.f;
    return v;
  };
  m.host.status = [build] {
    UiStatus st;
    st.pauseLines = {L"Monitor 1: paused: maximized window (chrome.exe)", L"Monitor 2: snowing"};
    st.trees = 2;
    st.maxTrees = kMaxTrees;
    st.snowmen = 3;
    st.maxSnowmen = kMaxSnowmen;
    st.diagnosticsLogging = true;
    st.fps = 60;
    st.build = build;
    return st;
  };
  m.host.setDiagnosticsLogging = [](bool) {};
  m.host.hotkeyCapture = [](bool) {};
  auto uiLang = std::make_shared<Lang>(Lang::En);
  m.host.lang = [uiLang] { return *uiLang; };
  if (!m.InitFactories() || !m.wic) {
    std::printf("render-settings: Direct2D / WIC setup failed\n");
    return 2;
  }
  m.Reload();
  m.RefreshStatus(true);

  struct Job {
    int section;
    bool dark;
    float scale;
    bool full;  // the whole page, however tall
    Lang lang = Lang::En;
  };
  std::vector<Job> jobs;
  const auto& secs = SchemaSections();
  for (size_t i = 0; i < secs.size(); ++i)
    for (bool dark : {true, false}) jobs.push_back({static_cast<int>(i), dark, 1.f, false});
  jobs.push_back({0, true, 1.5f, false});
  for (size_t i = 0; i < secs.size(); ++i) jobs.push_back({static_cast<int>(i), true, 1.f, true});
  // Cyrillic labels are longer: check that they fit.
  jobs.push_back({0, true, 1.f, false, Lang::Ru});
  jobs.push_back({1, false, 1.f, false, Lang::Uk});

  int failures = 0, written = 0;
  for (const Job& j : jobs) {
    *uiLang = j.lang;
    m.Reload();
    m.dark = j.dark;
    m.pal = MakePalette(j.dark, false, 0, false);
    m.section = j.section;
    m.scroll = 0;
    m.width = kDefaultW;
    m.height = kDefaultH;
    m.anims.clear();
    // Show the keyboard focus ring on the first control of General.
    m.focusVisible = j.section == 0;
    m.Layout();
    m.focus = m.stops.size() > 1 ? m.stops[1].key : m.stops.empty() ? 0 : m.stops[0].key;
    if (j.full) {
      if (m.contentBottom <= kDefaultH) continue;  // fits already: no extra image
      m.height = std::ceil(m.contentBottom);
      m.Layout();
    }
    UINT pw = static_cast<UINT>(std::lround(m.width * j.scale));
    UINT ph = static_cast<UINT>(std::lround(m.height * j.scale));
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> target;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * j.scale, 96.f * j.scale);
    std::wstring name = L"settings-" + W(secs[static_cast<size_t>(j.section)].key) + (j.dark ? L"-dark" : L"-light");
    if (j.scale != 1.f) name += L"-" + std::to_wstring(std::lround(j.scale * 100.f));
    if (j.full) name += L"-full";
    if (j.lang != Lang::En) name += L"-" + W(LangCode(j.lang));
    std::wstring path = dir + L"\\" + name + L".png";
    StepCheck check;
    bool ok = check(m.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp),
                    "WIC CreateBitmap") &&
              check(m.d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &target), "CreateWicBitmapRenderTarget");
    if (ok) {
      target->BeginDraw();
      m.Paint(target.Get());
      ok = check(target->EndDraw(), "EndDraw");
      m.rt = nullptr;
      m.boundRt = nullptr;
      m.brush.Reset();
      m.icon.Reset();
      m.iconTried = false;
    }
    ok = ok && SavePng(m.wic.Get(), bmp.Get(), path, check);
    if (ok) std::printf("render-settings: wrote %ls (%ux%u)\n", path.c_str(), pw, ph);
    else
      std::printf("render-settings: FAILED %ls: %s 0x%08lx\n", path.c_str(), check.failed ? check.failed : "?",
                  static_cast<unsigned long>(check.hr));
    if (ok) ++written;
    else ++failures;
  }
  std::fflush(stdout);
  return failures == 0 && written > 0 ? 0 : 1;
}

}  // namespace snow::win
