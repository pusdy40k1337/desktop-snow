#include "settings.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include "theme.h"

namespace snow {

namespace fs = std::filesystem;
using json::Value;

static float clampNum(float v, float lo, float hi, float def) {
  if (!std::isfinite(v)) return def;
  return std::clamp(v, lo, hi);
}
static int clampInt(int v, int lo, int hi) { return std::clamp(v, lo, hi); }
// path::string() can throw on Windows for characters outside the ANSI code page.
static std::string PathUtf8(const fs::path& p) {
  auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
static std::string oneOf(const std::string& v, std::initializer_list<const char*> allowed, const char* def) {
  for (const char* a : allowed)
    if (v == a) return v;
  return def;
}

void Sanitize(Settings& s) {
  s.intensity = clampInt(s.intensity, 25, 200);
  s.fpsCap = clampInt(s.fpsCap, 20, 240);
  s.language = oneOf(s.language, {"auto", "en", "ru", "uk"}, "auto");
  s.reducedMotion = oneOf(s.reducedMotion, {"auto", "on", "off"}, "auto");
  s.effect = oneOf(s.effect, {"snow", "confetti", "none"}, "snow");
  bool themeOk = false;
  for (const auto& id : ThemeIds()) themeOk = themeOk || id == s.theme;
  if (!themeOk) s.theme = "classic";
  if (s.hotkeyToggle.size() > 40) s.hotkeyToggle = "Ctrl+Alt+S";
  if (s.hotkeyDecorate.size() > 40) s.hotkeyDecorate = "Ctrl+Alt+D";
  if (s.decorColor < 0) s.decorColor = -1;
  else s.decorColor = std::clamp(s.decorColor, 0, 63);
  s.paletteLeft = std::clamp(s.paletteLeft, -20000, 20000);
  s.paletteTop = std::clamp(s.paletteTop, -20000, 20000);
  {
    std::vector<std::string> mons;
    for (const auto& id : s.disabledMonitors)
      if (!id.empty() && id.size() <= 64 && mons.size() < 16 && std::find(mons.begin(), mons.end(), id) == mons.end())
        mons.push_back(id);
    s.disabledMonitors = std::move(mons);
  }

  SnowSettings& n = s.snow;
  n.density = clampInt(n.density, 0, 100);
  n.windSpeed = clampNum(n.windSpeed, -50, 50, 5);
  n.flakeMin = clampNum(n.flakeMin, 1, 16, 3);
  n.flakeMax = clampNum(n.flakeMax, 1, 16, 7);
  if (n.flakeMax < n.flakeMin) std::swap(n.flakeMin, n.flakeMax);
  n.fallSpeed = clampNum(n.fallSpeed, 20, 300, 90);
  n.sway = clampNum(n.sway, 0, 100, 35);
  Color c;
  if (!ParseHexColor(n.color, c)) n.color = "#ffffff";

  PileSettings& p = s.piles;
  p.maxHeight = clampNum(p.maxHeight, 6, 120, 34);
  p.growth = clampNum(p.growth, 0.1f, 8, 1);
  p.taskbarMaxHeight = clampNum(p.taskbarMaxHeight, 4, 120, 40);
  if (!(p.cornerRadius < 0)) p.cornerRadius = clampNum(p.cornerRadius, 0, 40, -1);
  else p.cornerRadius = -1;

  ScatterSettings& sc = s.scatter;
  sc.speedOn = clampNum(sc.speedOn, 150, 20000, 900);
  sc.hysteresis = clampNum(sc.hysteresis, 0.2f, 0.95f, 0.67f);
  sc.accelOn = clampNum(sc.accelOn, 1000, 1e6f, 22000);
  sc.regrowDelay = clampNum(sc.regrowDelay, 0, 30, 1.5f);
  sc.inherit = clampNum(sc.inherit, 0, 1.5f, 0.55f);

  s.sync.mode = oneOf(s.sync.mode, {"latch", "vsync"}, "latch");
  s.sync.predictor = oneOf(s.sync.predictor, {"linear", "stepped"}, "stepped");
  s.sync.latchMarginMs = clampNum(s.sync.latchMarginMs, 0, 8, 1.5f);
}

void ResetToDefaults(Settings& s) {
  bool startup = s.launchAtStartup;
  s = Settings{};
  s.launchAtStartup = startup;
}

void Sanitize(DecorState& d) {
  if (d.trees.size() > static_cast<size_t>(kMaxTrees)) d.trees.resize(static_cast<size_t>(kMaxTrees));
  if (d.snowmen.size() > static_cast<size_t>(kMaxSnowmen)) d.snowmen.resize(static_cast<size_t>(kMaxSnowmen));
  uint32_t maxId = 0;
  for (auto& t : d.trees) {
    t.nx = clampNum(t.nx, 0, 1, 0.5f);
    t.ny = clampNum(t.ny, 0, 1, 1);
    t.scale = clampNum(t.scale, 0.4f, 2.5f, 1);
    t.grow = 1.f;
    if (t.ornaments.size() > static_cast<size_t>(kMaxOrnamentsPerTree))
      t.ornaments.resize(static_cast<size_t>(kMaxOrnamentsPerTree));
    for (auto& o : t.ornaments) {
      o.u = clampNum(o.u, 0, 1, 0.5f);
      o.v = clampNum(o.v, 0, 1, 0.5f);
      o.color = std::clamp(o.color, 0, 63);
      if (o.kind != OrnamentKind::Bauble && o.kind != OrnamentKind::CandyCane) o.kind = OrnamentKind::Bauble;
    }
    maxId = std::max(maxId, t.id);
  }
  for (auto& s : d.snowmen) {
    s.relX = clampNum(s.relX, 0, 1, 0.5f);
    s.scale = clampNum(s.scale, 0.4f, 2.5f, 1);
    s.hat = std::clamp(s.hat, 0, 3);
    s.scarf = std::clamp(s.scarf, 0, 63);
    s.hatColor = std::clamp(s.hatColor, 0, 63);
    if (s.free) {
      s.nx = clampNum(s.nx, 0, 1, 0.5f);
      s.ny = clampNum(s.ny, 0, 1, 1);
    }
    maxId = std::max(maxId, s.id);
  }
  if (d.free.size() > static_cast<size_t>(kMaxFreeDecor))
    d.free.resize(static_cast<size_t>(kMaxFreeDecor));
  for (auto& f : d.free) {
    f.nx = clampNum(f.nx, 0, 1, 0.5f);
    f.ny = clampNum(f.ny, 0, 1, 0.5f);
    f.scale = clampNum(f.scale, 0.4f, 2.5f, 1);
    f.color = std::clamp(f.color, 0, 63);
    f.variant = std::clamp(f.variant, 1, 3);
    if (static_cast<int>(f.kind) < 0 || static_cast<int>(f.kind) > static_cast<int>(FreeKind::Scarf))
      f.kind = FreeKind::Bauble;
    maxId = std::max(maxId, f.id);
  }
  // Ids must be unique and non-zero.
  uint32_t next = std::max(d.nextId, maxId + 1);
  std::vector<uint32_t> seen;
  auto fix = [&](uint32_t& id) {
    if (id == 0 || std::find(seen.begin(), seen.end(), id) != seen.end()) id = next++;
    seen.push_back(id);
  };
  for (auto& t : d.trees) fix(t.id);
  for (auto& s : d.snowmen) fix(s.id);
  for (auto& f : d.free) fix(f.id);
  d.nextId = std::max<uint32_t>(next, 1);
}

// ---------------------------------------------------------------- to JSON

static Value anchorToJson(const WindowAnchor& a) {
  Value v = Value::object();
  v.set("taskbar", a.taskbar);
  v.set("hwnd", static_cast<double>(a.hwnd));
  v.set("process", a.process);
  v.set("class", a.className);
  v.set("title", a.title);
  return v;
}

Value ToJson(const AppState& st) {
  const Settings& s = st.settings;
  Value root = Value::object();
  root.set("version", kStateVersion);
  root.set("app", "DesktopSnow");

  Value set = Value::object();
  set.set("enabled", s.enabled);
  set.set("effect", s.effect);
  set.set("intensity", s.intensity);
  set.set("theme", s.theme);
  set.set("fpsCap", s.fpsCap);
  set.set("language", s.language);
  set.set("launchAtStartup", s.launchAtStartup);
  set.set("debugOverlay", s.debugOverlay);
  set.set("reducedMotion", s.reducedMotion);
  set.set("batterySaver", s.batterySaver);
  set.set("pauseFullscreen", s.pauseFullscreen);
  set.set("pauseMaximized", s.pauseMaximized);
  set.set("pauseOnBattery", s.pauseOnBattery);
  {
    Value mons = Value::array();
    for (const auto& id : s.disabledMonitors) mons.push(id);
    set.set("disabledMonitors", mons);
  }
  set.set("hotkeyToggle", s.hotkeyToggle);
  set.set("hotkeyDecorate", s.hotkeyDecorate);
  set.set("decorColor", s.decorColor);
  set.set("paletteLeft", s.paletteLeft);
  set.set("paletteTop", s.paletteTop);

  Value snow = Value::object();
  snow.set("density", s.snow.density);
  snow.set("windSpeed", s.snow.windSpeed);
  Value size = Value::array();
  size.push(s.snow.flakeMin);
  size.push(s.snow.flakeMax);
  snow.set("flakeSize", size);
  snow.set("fallSpeed", s.snow.fallSpeed);
  snow.set("sway", s.snow.sway);
  snow.set("color", s.snow.color);
  set.set("snow", snow);

  Value piles = Value::object();
  piles.set("enabled", s.piles.enabled);
  piles.set("maxHeight", s.piles.maxHeight);
  piles.set("growth", s.piles.growth);
  piles.set("onTaskbar", s.piles.onTaskbar);
  piles.set("taskbarMaxHeight", s.piles.taskbarMaxHeight);
  piles.set("cornerRadius", s.piles.cornerRadius);
  set.set("piles", piles);

  Value sc = Value::object();
  sc.set("speedOn", s.scatter.speedOn);
  sc.set("hysteresis", s.scatter.hysteresis);
  sc.set("accelOn", s.scatter.accelOn);
  sc.set("regrowDelay", s.scatter.regrowDelay);
  sc.set("inherit", s.scatter.inherit);
  set.set("scatter", sc);

  Value sy = Value::object();
  sy.set("mode", s.sync.mode);
  sy.set("predict", s.sync.predict);
  sy.set("predictor", s.sync.predictor);
  sy.set("latchMarginMs", s.sync.latchMarginMs);
  set.set("sync", sy);

  Value ex = Value::object();
  ex.set("windGusts", s.extras.windGusts);
  ex.set("cursorSweep", s.extras.cursorSweep);
  ex.set("melt", s.extras.melt);
  ex.set("sparkle", s.extras.sparkle);
  ex.set("dayNightTint", s.extras.dayNightTint);
  ex.set("santa", s.extras.santa);
  ex.set("train", s.extras.train);
  ex.set("trainKnocks", s.extras.trainKnocksSnowmen);
  ex.set("snowballs", s.extras.snowballs);
  ex.set("yeti", s.extras.yeti);
  set.set("extras", ex);
  root.set("settings", set);

  Value decor = Value::object();
  Value trees = Value::array();
  for (const auto& t : st.decor.trees) {
    Value tv = Value::object();
    tv.set("id", t.id);
    tv.set("monitor", t.monitor);
    tv.set("x", t.nx);
    tv.set("y", t.ny);
    tv.set("scale", t.scale);
    tv.set("star", t.star);
    tv.set("lights", t.lights);
    Value orns = Value::array();
    for (const auto& o : t.ornaments) {
      Value ov = Value::object();
      ov.set("kind", o.kind == OrnamentKind::CandyCane ? "candycane" : "bauble");
      ov.set("u", o.u);
      ov.set("v", o.v);
      ov.set("color", o.color);
      orns.push(ov);
    }
    tv.set("ornaments", orns);
    trees.push(tv);
  }
  decor.set("trees", trees);
  Value men = Value::array();
  for (const auto& m : st.decor.snowmen) {
    Value mv = Value::object();
    mv.set("id", m.id);
    mv.set("anchor", anchorToJson(m.anchor));
    mv.set("relX", m.relX);
    mv.set("scale", m.scale);
    mv.set("hat", m.hat);
    mv.set("scarf", m.scarf);
    mv.set("hatColor", m.hatColor);
    mv.set("free", m.free);
    mv.set("monitor", m.monitor);
    mv.set("x", m.nx);
    mv.set("y", m.ny);
    men.push(mv);
  }
  decor.set("snowmen", men);
  Value free = Value::array();
  for (const auto& f : st.decor.free) {
    Value fv = Value::object();
    fv.set("id", f.id);
    const char* kind = "bauble";
    switch (f.kind) {
      case FreeKind::CandyCane: kind = "candycane"; break;
      case FreeKind::Star: kind = "star"; break;
      case FreeKind::Hat: kind = "hat"; break;
      case FreeKind::Scarf: kind = "scarf"; break;
      default: break;
    }
    fv.set("kind", std::string(kind));
    fv.set("monitor", f.monitor);
    fv.set("x", f.nx);
    fv.set("y", f.ny);
    fv.set("scale", f.scale);
    fv.set("color", f.color);
    fv.set("variant", f.variant);
    free.push(fv);
  }
  decor.set("free", free);
  decor.set("nextId", st.decor.nextId);
  root.set("decorations", decor);
  return root;
}

// -------------------------------------------------------------- from JSON

static void rd(const Value& v, const char* k, bool& out) {
  if (v[k].isBool()) out = v[k].asBool();
}
static void rd(const Value& v, const char* k, int& out) {
  if (v[k].isNumber()) out = v[k].asInt(out);
}
static void rd(const Value& v, const char* k, float& out) {
  if (v[k].isNumber()) out = static_cast<float>(v[k].asNumber(out));
}
static void rd(const Value& v, const char* k, std::string& out) {
  if (v[k].isString()) out = v[k].asString();
}

void FromJson(const Value& root, AppState& st) {
  const Value& set = root["settings"];
  Settings& s = st.settings;
  if (set.isObject()) {
    rd(set, "enabled", s.enabled);
    rd(set, "effect", s.effect);
    rd(set, "intensity", s.intensity);
    rd(set, "theme", s.theme);
    rd(set, "fpsCap", s.fpsCap);
    rd(set, "language", s.language);
    rd(set, "launchAtStartup", s.launchAtStartup);
    rd(set, "debugOverlay", s.debugOverlay);
    rd(set, "reducedMotion", s.reducedMotion);
    rd(set, "batterySaver", s.batterySaver);
    rd(set, "pauseFullscreen", s.pauseFullscreen);
    rd(set, "pauseMaximized", s.pauseMaximized);
    rd(set, "pauseOnBattery", s.pauseOnBattery);
    if (set["disabledMonitors"].isArray()) {
      s.disabledMonitors.clear();
      const Value& mons = set["disabledMonitors"];
      for (size_t i = 0; i < mons.size(); ++i)
        if (mons.at(i).isString()) s.disabledMonitors.push_back(mons.at(i).asString());
    }
    rd(set, "hotkeyToggle", s.hotkeyToggle);
    rd(set, "hotkeyDecorate", s.hotkeyDecorate);
    rd(set, "decorColor", s.decorColor);
    rd(set, "paletteLeft", s.paletteLeft);
    rd(set, "paletteTop", s.paletteTop);
    const Value& snow = set["snow"];
    rd(snow, "density", s.snow.density);
    rd(snow, "windSpeed", s.snow.windSpeed);
    if (snow["flakeSize"].isArray() && snow["flakeSize"].size() == 2) {
      s.snow.flakeMin = static_cast<float>(snow["flakeSize"].at(0).asNumber(s.snow.flakeMin));
      s.snow.flakeMax = static_cast<float>(snow["flakeSize"].at(1).asNumber(s.snow.flakeMax));
    }
    rd(snow, "fallSpeed", s.snow.fallSpeed);
    rd(snow, "sway", s.snow.sway);
    rd(snow, "color", s.snow.color);
    const Value& piles = set["piles"];
    rd(piles, "enabled", s.piles.enabled);
    rd(piles, "maxHeight", s.piles.maxHeight);
    rd(piles, "growth", s.piles.growth);
    rd(piles, "onTaskbar", s.piles.onTaskbar);
    rd(piles, "taskbarMaxHeight", s.piles.taskbarMaxHeight);
    rd(piles, "cornerRadius", s.piles.cornerRadius);
    const Value& sc = set["scatter"];
    rd(sc, "speedOn", s.scatter.speedOn);
    rd(sc, "hysteresis", s.scatter.hysteresis);
    rd(sc, "accelOn", s.scatter.accelOn);
    rd(sc, "regrowDelay", s.scatter.regrowDelay);
    rd(sc, "inherit", s.scatter.inherit);
    const Value& sy = set["sync"];
    rd(sy, "mode", s.sync.mode);
    rd(sy, "predict", s.sync.predict);
    rd(sy, "predictor", s.sync.predictor);
    rd(sy, "latchMarginMs", s.sync.latchMarginMs);
    const Value& ex = set["extras"];
    rd(ex, "windGusts", s.extras.windGusts);
    rd(ex, "cursorSweep", s.extras.cursorSweep);
    rd(ex, "melt", s.extras.melt);
    rd(ex, "sparkle", s.extras.sparkle);
    rd(ex, "dayNightTint", s.extras.dayNightTint);
    rd(ex, "santa", s.extras.santa);
    rd(ex, "train", s.extras.train);
    rd(ex, "trainKnocks", s.extras.trainKnocksSnowmen);
    rd(ex, "snowballs", s.extras.snowballs);
    rd(ex, "yeti", s.extras.yeti);
  }

  const Value& decor = root["decorations"];
  if (decor.isObject()) {
    DecorState d;
    for (const Value& tv : decor["trees"].elements()) {
      if (!tv.isObject()) continue;
      TreeDecor t;
      t.id = static_cast<uint32_t>(std::max(0, tv["id"].asInt(0)));
      rd(tv, "monitor", t.monitor);
      rd(tv, "x", t.nx);
      rd(tv, "y", t.ny);
      rd(tv, "scale", t.scale);
      rd(tv, "star", t.star);
      rd(tv, "lights", t.lights);
      for (const Value& ov : tv["ornaments"].elements()) {
        if (!ov.isObject()) continue;
        Ornament o;
        o.kind = ov["kind"].asString() == "candycane" ? OrnamentKind::CandyCane : OrnamentKind::Bauble;
        rd(ov, "u", o.u);
        rd(ov, "v", o.v);
        rd(ov, "color", o.color);
        t.ornaments.push_back(o);
      }
      d.trees.push_back(std::move(t));
    }
    for (const Value& mv : decor["snowmen"].elements()) {
      if (!mv.isObject()) continue;
      SnowmanDecor m;
      m.id = static_cast<uint32_t>(std::max(0, mv["id"].asInt(0)));
      const Value& a = mv["anchor"];
      rd(a, "taskbar", m.anchor.taskbar);
      if (a["hwnd"].isNumber()) m.anchor.hwnd = static_cast<uint64_t>(std::max(0.0, a["hwnd"].asNumber()));
      rd(a, "process", m.anchor.process);
      rd(a, "class", m.anchor.className);
      rd(a, "title", m.anchor.title);
      rd(mv, "relX", m.relX);
      rd(mv, "scale", m.scale);
      rd(mv, "hat", m.hat);
      rd(mv, "scarf", m.scarf);
      rd(mv, "hatColor", m.hatColor);
      rd(mv, "free", m.free);
      rd(mv, "monitor", m.monitor);
      rd(mv, "x", m.nx);
      rd(mv, "y", m.ny);
      d.snowmen.push_back(std::move(m));
    }
    for (const Value& fv : decor["free"].elements()) {
      if (!fv.isObject()) continue;
      FreeDecor f;
      f.id = static_cast<uint32_t>(std::max(0, fv["id"].asInt(0)));
      const std::string kind = fv["kind"].asString();
      if (kind == "candycane") f.kind = FreeKind::CandyCane;
      else if (kind == "star") f.kind = FreeKind::Star;
      else if (kind == "hat") f.kind = FreeKind::Hat;
      else if (kind == "scarf") f.kind = FreeKind::Scarf;
      else f.kind = FreeKind::Bauble;
      rd(fv, "monitor", f.monitor);
      rd(fv, "x", f.nx);
      rd(fv, "y", f.ny);
      rd(fv, "scale", f.scale);
      rd(fv, "color", f.color);
      rd(fv, "variant", f.variant);
      d.free.push_back(std::move(f));
    }
    int next = decor["nextId"].asInt(1);
    d.nextId = static_cast<uint32_t>(std::max(1, next));
    st.decor = std::move(d);
  }
}

bool ImportLegacy(const Value& v, Settings& s) {
  if (!v.isObject()) return false;
  const Value& snow = v["values"]["snow"];
  if (!snow.isObject()) return false;
  rd(snow, "density", s.snow.density);
  rd(snow, "windSpeed", s.snow.windSpeed);
  if (snow["flakeSize"].isArray() && snow["flakeSize"].size() == 2) {
    s.snow.flakeMin = static_cast<float>(snow["flakeSize"].at(0).asNumber(s.snow.flakeMin));
    s.snow.flakeMax = static_cast<float>(snow["flakeSize"].at(1).asNumber(s.snow.flakeMax));
  }
  rd(snow, "fallSpeed", s.snow.fallSpeed);
  rd(snow, "sway", s.snow.sway);
  rd(snow, "color", s.snow.color);
  rd(snow, "accumulate", s.piles.enabled);
  // Legacy defaults (blowSpeed 1200, fps 30) are replaced by the new ones;
  // only values the user actually changed are carried over.
  if (snow["blowSpeed"].isNumber() && snow["blowSpeed"].asInt() != 1200)
    s.scatter.speedOn = static_cast<float>(snow["blowSpeed"].asNumber());
  if (snow["dockMoundHeight"].isNumber()) s.piles.taskbarMaxHeight = static_cast<float>(snow["dockMoundHeight"].asNumber());
  if (snow["fps"].isNumber() && snow["fps"].asInt() != 30) s.fpsCap = snow["fps"].asInt();
  if (v["activeEffect"].isString() && v["activeEffect"].asString() != "snow") s.effect = "none";
  Sanitize(s);
  return true;
}

// ------------------------------------------------------------------- I/O

bool ReadTextFile(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  if (f.bad()) return false;
  out = ss.str();
  return true;
}

bool WriteTextFile(const fs::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.flush();
  return f.good();
}

fs::path StateStore::backupFile() const {
  fs::path b = file_;
  b += ".bak";
  return b;
}

LoadResult StateStore::load(AppState& out, const fs::path& legacyFile) {
  LoadResult r;
  out = AppState();
  auto tryLoad = [&](const fs::path& p, bool& corrupt) {
    corrupt = false;
    std::string text;
    if (!ReadTextFile(p, text)) return false;
    Value v;
    std::string err;
    if (!json::parse(text, v, &err) || !v.isObject()) {
      corrupt = true;
      r.message = PathUtf8(p.filename()) + ": " + (err.empty() ? std::string("not an object") : err);
      return false;
    }
    if (v["version"].asInt(0) > kStateVersion) r.newerVersion = true;
    AppState tmp;
    FromJson(v, tmp);
    out = std::move(tmp);
    return true;
  };

  bool corrupt = false;
  if (tryLoad(file_, corrupt)) {
    r.fromFile = true;
  } else {
    if (corrupt) {
      std::error_code ec;
      fs::path q = file_;
      q += ".corrupt";
      fs::rename(file_, q, ec);
      r.corruptFileQuarantined = !ec;
    }
    bool bcorrupt = false;
    if (tryLoad(backupFile(), bcorrupt)) {
      r.fromBackup = true;
    } else {
      out = AppState();
      if (!legacyFile.empty()) {
        std::string text;
        Value v;
        if (ReadTextFile(legacyFile, text) && json::parse(text, v) && ImportLegacy(v, out.settings))
          r.migratedLegacy = true;
      }
    }
  }
  Sanitize(out.settings);
  Sanitize(out.decor);
  return r;
}

bool StateStore::save(const AppState& st, std::string* error) {
  std::error_code ec;
  if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
  fs::path tmp = file_;
  tmp += ".tmp";
  if (!WriteTextFile(tmp, json::dump(ToJson(st)))) {
    if (error) *error = "cannot write " + PathUtf8(tmp);
    return false;
  }
  if (fs::exists(file_, ec)) {
    // The previous good file becomes the backup used if the main one is ever corrupt.
    fs::rename(file_, backupFile(), ec);
    if (ec) {
      ec.clear();
      fs::copy_file(file_, backupFile(), fs::copy_options::overwrite_existing, ec);
    }
  }
  ec.clear();
  fs::rename(tmp, file_, ec);
  if (ec) {
    if (error) *error = "cannot replace state file: " + ec.message();
    return false;
  }
  return true;
}

}  // namespace snow
