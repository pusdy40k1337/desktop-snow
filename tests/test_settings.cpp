#include <filesystem>
#include <string>

#include "../src/core/json.h"
#include "../src/core/settings.h"
#include "test.h"

using namespace snow;
namespace fs = std::filesystem;

static fs::path TempDir(const char* name) {
  fs::path d = fs::temp_directory_path() / (std::string("snowtest_") + name);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

TEST(json_roundtrip_unicode_and_numbers) {
  json::Value v = json::Value::object();
  v.set("title", "Сніг — ялинка ☃");
  v.set("n", 3.25);
  v.set("i", 42);
  v.set("neg", -7);
  json::Value arr = json::Value::array();
  arr.push(true);
  arr.push(json::Value());
  arr.push("a\"b\\c\n");
  v.set("arr", arr);
  std::string text = json::dump(v);
  json::Value back;
  std::string err;
  CHECK(json::parse(text, back, &err));
  CHECK(back["title"].asString() == "Сніг — ялинка ☃");
  CHECK_NEAR(back["n"].asNumber(), 3.25, 1e-12);
  CHECK(back["i"].asInt() == 42);
  CHECK(back["neg"].asInt() == -7);
  CHECK(back["arr"].size() == 3);
  CHECK(back["arr"].at(2).asString() == "a\"b\\c\n");
}

TEST(json_rejects_malformed) {
  json::Value v;
  std::string err;
  CHECK(!json::parse("{\"a\": 1,", v, &err));
  CHECK(!err.empty());
  CHECK(!json::parse("{\"a\": tru}", v));
  CHECK(!json::parse("[1, 2] x", v));
  CHECK(!json::parse("", v));
  CHECK(json::parse("\xEF\xBB\xBF{\"bom\": 1}", v));  // BOM tolerated
  CHECK(v["bom"].asInt() == 1);
  CHECK(json::parse("{\"u\": \"\\u0421\\ud83c\\udf84\"}", v));
  CHECK(v["u"].asString() == "С\xF0\x9F\x8E\x84");
}

static AppState Sample() {
  AppState st;
  st.settings.intensity = 150;
  st.settings.theme = "candy";
  st.settings.snow.windSpeed = -12;
  st.settings.scatter.speedOn = 1100;
  st.settings.language = "uk";
  TreeDecor t;
  t.id = 5;
  t.monitor = "\\\\.\\DISPLAY2";
  t.nx = 0.25f;
  t.ny = 0.9f;
  t.star = true;
  t.lights = true;
  t.ornaments.push_back({OrnamentKind::CandyCane, 0.4f, 0.6f, 2});
  st.decor.trees.push_back(t);
  SnowmanDecor m;
  m.id = 9;
  m.anchor.process = "notepad.exe";
  m.anchor.className = "Notepad";
  m.anchor.title = "Новый документ — Блокнот";
  m.anchor.hwnd = 0x1234;
  m.relX = 0.3f;
  m.hat = 2;
  st.decor.snowmen.push_back(m);
  st.decor.nextId = 10;
  return st;
}

TEST(settings_roundtrip_through_file) {
  fs::path dir = TempDir("roundtrip");
  StateStore store(dir / "state.json");
  CHECK(store.save(Sample()));
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.fromFile);
  CHECK(loaded.settings.intensity == 150);
  CHECK(loaded.settings.theme == "candy");
  CHECK(loaded.settings.language == "uk");
  CHECK_NEAR(loaded.settings.snow.windSpeed, -12, 1e-6);
  CHECK_NEAR(loaded.settings.scatter.speedOn, 1100, 1e-6);
  CHECK(loaded.decor.trees.size() == 1);
  CHECK(loaded.decor.trees[0].star && loaded.decor.trees[0].lights);
  CHECK(loaded.decor.trees[0].ornaments.size() == 1);
  CHECK(loaded.decor.trees[0].ornaments[0].kind == OrnamentKind::CandyCane);
  CHECK(loaded.decor.trees[0].monitor == "\\\\.\\DISPLAY2");
  CHECK(loaded.decor.snowmen.size() == 1);
  CHECK(loaded.decor.snowmen[0].anchor.title == "Новый документ — Блокнот");
  CHECK(loaded.decor.snowmen[0].anchor.hwnd == 0x1234);
  CHECK(loaded.decor.snowmen[0].hat == 2);
  CHECK(loaded.decor.nextId == 10);
}

TEST(settings_missing_file_gives_defaults) {
  fs::path dir = TempDir("missing");
  StateStore store(dir / "state.json");
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(!r.fromFile && !r.fromBackup);
  CHECK(loaded.settings.enabled);
  CHECK(loaded.settings.fpsCap == 60);
  CHECK_NEAR(loaded.settings.scatter.speedOn, 900, 1e-6);
}

TEST(settings_corrupt_file_falls_back_to_backup) {
  fs::path dir = TempDir("corrupt");
  StateStore store(dir / "state.json");
  AppState a = Sample();
  CHECK(store.save(a));         // first save
  a.settings.intensity = 75;
  CHECK(store.save(a));         // second save: previous file becomes .bak
  CHECK(fs::exists(store.backupFile()));
  CHECK(WriteTextFile(store.file(), "{\"version\": 2, \"settings\": {\"intens"));  // truncated write
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.corruptFileQuarantined);
  CHECK(r.fromBackup);
  CHECK(loaded.settings.intensity == 150);  // the last good copy
  fs::path q = store.file();
  q += ".corrupt";
  CHECK(fs::exists(q));
}

TEST(settings_corrupt_without_backup_uses_defaults) {
  fs::path dir = TempDir("corrupt2");
  StateStore store(dir / "state.json");
  CHECK(WriteTextFile(store.file(), std::string("\0\0\0garbage", 10)));
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.corruptFileQuarantined);
  CHECK(!r.fromFile && !r.fromBackup);
  CHECK(loaded.settings.intensity == 100);
}

TEST(settings_out_of_range_values_are_clamped) {
  fs::path dir = TempDir("clamp");
  StateStore store(dir / "state.json");
  CHECK(WriteTextFile(store.file(),
                      "{\"version\":2,\"settings\":{\"intensity\":9999,\"fpsCap\":-5,\"theme\":\"nope\","
                      "\"language\":\"de\",\"snow\":{\"flakeSize\":[9,2],\"color\":\"zzz\"},"
                      "\"scatter\":{\"speedOn\":\"fast\"}},"
                      "\"decorations\":{\"trees\":[{},{},{},{},{}],\"snowmen\":[{\"relX\":7,\"hat\":99}]}}"));
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.fromFile);
  CHECK(loaded.settings.intensity == 200);
  CHECK(loaded.settings.fpsCap == 20);
  CHECK(loaded.settings.theme == "classic");
  CHECK(loaded.settings.language == "auto");
  CHECK(loaded.settings.snow.flakeMin == 2 && loaded.settings.snow.flakeMax == 9);
  CHECK(loaded.settings.snow.color == "#ffffff");
  CHECK_NEAR(loaded.settings.scatter.speedOn, 900, 1e-6);  // wrong type ignored
  CHECK(loaded.decor.trees.size() == 3);                    // at most 3 trees
  CHECK(loaded.decor.snowmen[0].relX == 1.f);
  CHECK(loaded.decor.snowmen[0].hat == 3);
  // Ids repaired to be unique and non-zero.
  CHECK(loaded.decor.trees[0].id != 0 && loaded.decor.trees[0].id != loaded.decor.trees[1].id);
}

TEST(settings_newer_version_still_loads) {
  fs::path dir = TempDir("newer");
  StateStore store(dir / "state.json");
  CHECK(WriteTextFile(store.file(), "{\"version\":99,\"settings\":{\"intensity\":60,\"futureThing\":1}}"));
  AppState loaded;
  LoadResult r = store.load(loaded);
  CHECK(r.fromFile && r.newerVersion);
  CHECK(loaded.settings.intensity == 60);
}

TEST(settings_imports_legacy_electron_file) {
  fs::path dir = TempDir("legacy");
  fs::path legacy = dir / "legacy-settings.json";
  CHECK(WriteTextFile(legacy,
                      "{\"activeEffect\":\"snow\",\"paused\":false,\"values\":{\"snow\":{\"density\":70,"
                      "\"windSpeed\":-20,\"flakeSize\":[4,9],\"fallSpeed\":150,\"sway\":10,\"color\":\"#ccddff\","
                      "\"accumulate\":true,\"blowSpeed\":1200,\"snowOverWindows\":true,\"dockMoundHeight\":30,"
                      "\"fps\":30}}}"));
  StateStore store(dir / "state.json");
  AppState loaded;
  LoadResult r = store.load(loaded, legacy);
  CHECK(r.migratedLegacy);
  CHECK(loaded.settings.snow.density == 70);
  CHECK_NEAR(loaded.settings.snow.windSpeed, -20, 1e-6);
  CHECK(loaded.settings.snow.color == "#ccddff");
  CHECK_NEAR(loaded.settings.scatter.speedOn, 900, 1e-6);  // legacy default replaced
  CHECK(loaded.settings.fpsCap == 60);                       // legacy default replaced
  CHECK_NEAR(loaded.settings.piles.taskbarMaxHeight, 30, 1e-6);
}

TEST(settings_save_is_atomic_no_tmp_left) {
  fs::path dir = TempDir("atomic");
  StateStore store(dir / "state.json");
  CHECK(store.save(Sample()));
  fs::path tmp = store.file();
  tmp += ".tmp";
  CHECK(!fs::exists(tmp));
  CHECK(fs::exists(store.file()));
}
