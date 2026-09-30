#include "common/config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

#include "common/log.h"
#include "common/paths.h"
#include "common/util.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace po {
namespace {

// Legge un campo se presente e valido; altrimenti lascia il valore di default.
template <class T>
void Get(const json& j, const char* key, T& out) {
  if (!j.is_object()) return;
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  try {
    out = it->get<T>();
  } catch (const std::exception& e) {
    LogWarn("Campo JSON '{}' non valido: {}", key, e.what());
  }
}

void OneOf(std::string& v, std::initializer_list<const char*> allowed) {
  v = ToLowerAscii(Trim(v));
  for (const char* a : allowed)
    if (v == a) return;
  v = *allowed.begin();
}

std::string SanitizeFileName(std::string_view name) {
  std::string s;
  for (char c : name) {
    const bool bad = (unsigned char)c < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
    s.push_back(bad ? '_' : c);
  }
  s = Trim(s);
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
  return s.empty() ? "profile" : s;
}

}  // namespace

// ---------------------------------------------------------------- colori
Color ParseColor(std::string_view s, Color fb) {
  if (!s.empty() && s[0] == '#') s.remove_prefix(1);
  if (s.size() != 6 && s.size() != 8) return fb;
  unsigned v[4] = {0, 0, 0, 255};
  for (size_t i = 0; i < s.size() / 2; ++i) {
    const char* b = s.data() + i * 2;
    const auto r = std::from_chars(b, b + 2, v[i], 16);
    if (r.ec != std::errc{} || r.ptr != b + 2) return fb;
  }
  return {uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3])};
}

std::string ColorToHex(Color c) {
  if (c.a == 255) return std::format("#{:02X}{:02X}{:02X}", c.r, c.g, c.b);
  return std::format("#{:02X}{:02X}{:02X}{:02X}", c.r, c.g, c.b, c.a);
}

// ---------------------------------------------------------------- JSON
void to_json(json& j, const Color& c) { j = ColorToHex(c); }
void from_json(const json& j, Color& c) {
  if (j.is_string()) c = ParseColor(j.get<std::string>(), c);
}

void to_json(json& j, const GameEntry& g) {
  j = json{{"process", g.process}};
  if (!g.window.empty()) j["window"] = g.window;
}
void from_json(const json& j, GameEntry& g) {
  if (j.is_string()) {
    g.process = j.get<std::string>();
    return;
  }
  Get(j, "process", g.process);
  Get(j, "window", g.window);
}

void to_json(json& j, const ShowFlags& s) {
  j = json{{"fps", s.fps},         {"graph", s.graph},     {"frametime", s.frametime},
           {"percentiles", s.percentiles}, {"stutter", s.stutter}, {"cpu", s.cpu},
           {"gpu", s.gpu},         {"ram", s.ram},         {"battery", s.battery}};
}
void from_json(const json& j, ShowFlags& s) {
  Get(j, "fps", s.fps);
  Get(j, "graph", s.graph);
  Get(j, "frametime", s.frametime);
  Get(j, "percentiles", s.percentiles);
  Get(j, "stutter", s.stutter);
  Get(j, "cpu", s.cpu);
  Get(j, "gpu", s.gpu);
  Get(j, "ram", s.ram);
  Get(j, "battery", s.battery);
}

void to_json(json& j, const ElementColors& c) {
  j = json{{"fps", c.fps}, {"cpu", c.cpu},   {"gpu", c.gpu},          {"ram", c.ram},
           {"battery", c.battery}, {"text", c.text}, {"background", c.background}};
}
void from_json(const json& j, ElementColors& c) {
  Get(j, "fps", c.fps);
  Get(j, "cpu", c.cpu);
  Get(j, "gpu", c.gpu);
  Get(j, "ram", c.ram);
  Get(j, "battery", c.battery);
  Get(j, "text", c.text);
  Get(j, "background", c.background);
}

int LayoutFormatWidth(std::string_view id) {
  for (const auto& f : kLayoutFormats)
    if (id == f.id) return f.width;
  return 1920;
}

std::string ClosestLayoutFormat(double aspect) {
  const LayoutFormat* best = &kLayoutFormats[0];
  for (const auto& f : kLayoutFormats)
    if (std::abs(double(f.width) / kLayoutRefH - aspect) < std::abs(double(best->width) / kLayoutRefH - aspect))
      best = &f;
  return best->id;
}

std::vector<LayoutItem> ScaleLayout(const std::vector<LayoutItem>& items, int fromWidth, int toWidth) {
  // Un blocco nel terzo sinistro resta alla stessa distanza dal bordo sinistro, uno nel terzo destro alla
  // stessa distanza dal bordo destro; quelli al centro restano centrati (in proporzione).
  std::vector<LayoutItem> out = items;
  for (auto& it : out) {
    if (it.x < fromWidth / 3) continue;
    if (it.x > fromWidth * 2 / 3)
      it.x = std::max(0, toWidth - (fromWidth - it.x));
    else
      it.x = int(std::lround(double(it.x) * toWidth / fromWidth));
  }
  return out;
}

bool Profile::Field(std::string_view element, std::string_view key) const {
  const std::string k = std::string(element) + "." + std::string(key);
  if (auto it = fields.find(k); it != fields.end()) return it->second;
  for (const auto& f : kLayoutFields)
    if (element == f.element && key == f.key) return f.byDefault;
  return true;
}

void Profile::SetField(std::string_view element, std::string_view key, bool on) {
  fields[std::string(element) + "." + std::string(key)] = on;
}

const std::vector<LayoutItem>& Profile::LayoutFor(double aspect, int* canvasWidth) const {
  const std::string* bestId = nullptr;
  const std::vector<LayoutItem>* best = nullptr;
  for (const auto& [id, items] : layouts) {
    const double a = double(LayoutFormatWidth(id)) / kLayoutRefH;
    if (!best || std::abs(a - aspect) < std::abs(double(LayoutFormatWidth(*bestId)) / kLayoutRefH - aspect)) {
      best = &items;
      bestId = &id;
    }
  }
  static const std::vector<LayoutItem> kFallback = DefaultLayoutItems();
  if (canvasWidth) *canvasWidth = bestId ? LayoutFormatWidth(*bestId) : 1920;
  return best ? *best : kFallback;
}

std::vector<LayoutItem> DefaultLayoutItems() {
  // Colonna in alto a sinistra, come il layout verticale.
  // Distanze sufficienti perché gli sfondi dei blocchi non si sovrappongano.
  return {{"fps", 20, 20, 160},   {"graph", 20, 70, 120}, {"frametime", 20, 110, 100}, {"cpu", 20, 150, 100},
          {"gpu", 20, 190, 100}, {"ram", 20, 230, 100},  {"battery", 20, 270, 100}};
}

void to_json(json& j, const LayoutItem& i) {
  j = json{{"element", i.element}, {"x", i.x}, {"y", i.y}, {"scale", i.scale}};
}
void from_json(const json& j, LayoutItem& i) {
  Get(j, "element", i.element);
  Get(j, "x", i.x);
  Get(j, "y", i.y);
  Get(j, "scale", i.scale);
}

void to_json(json& j, const RtssLayer& l) {
  j = json{{"text", l.text},           {"x", l.x},           {"y", l.y},       {"extentX", l.extentX},
           {"extentY", l.extentY},     {"origin", l.origin}, {"size", l.size}, {"color", l.color}};
  if (!l.name.empty()) j["name"] = l.name;
  if (!l.colorSpec.empty()) j["colorSpec"] = l.colorSpec;
  if (!l.bgColor.empty()) j["bgColor"] = l.bgColor;
  if (!l.visibility.empty()) j["visibility"] = l.visibility;
  if (l.marginTop) j["marginTop"] = l.marginTop;
  if (l.marginBottom) j["marginBottom"] = l.marginBottom;
}
void from_json(const json& j, RtssLayer& l) {
  Get(j, "text", l.text);
  Get(j, "x", l.x);
  Get(j, "y", l.y);
  Get(j, "extentX", l.extentX);
  Get(j, "extentY", l.extentY);
  Get(j, "origin", l.origin);
  Get(j, "size", l.size);
  Get(j, "color", l.color);
  Get(j, "name", l.name);
  Get(j, "colorSpec", l.colorSpec);
  Get(j, "bgColor", l.bgColor);
  Get(j, "visibility", l.visibility);
  Get(j, "marginTop", l.marginTop);
  Get(j, "marginBottom", l.marginBottom);
}
void to_json(json& j, const RtssSourceDef& s) {
  j = json{{"name", s.name},         {"units", s.units}, {"format", s.format}, {"formula", s.formula},
           {"provider", s.provider}, {"id", s.id},       {"reading", s.reading}};
}
void from_json(const json& j, RtssSourceDef& s) {
  Get(j, "name", s.name);
  Get(j, "units", s.units);
  Get(j, "format", s.format);
  Get(j, "formula", s.formula);
  Get(j, "provider", s.provider);
  Get(j, "id", s.id);
  Get(j, "reading", s.reading);
}
void to_json(json& j, const RtssSource& s) { j = json{{"metric", s.metric}, {"decimals", s.decimals}}; }
void from_json(const json& j, RtssSource& s) {
  Get(j, "metric", s.metric);
  Get(j, "decimals", s.decimals);
}
void to_json(json& j, const RtssLayout& l) {
  j = json{{"name", l.name}, {"fontFace", l.fontFace}, {"fontWeight", l.fontWeight}, {"layers", l.layers},
           {"sources", l.sources}};
  if (l.advanced) {
    j["advanced"] = true;
    j["fontHeight"] = l.fontHeight;
    j["zoomRatio"] = l.zoomRatio;
    j["image"] = l.image;
    j["defs"] = l.defs;
  }
}
void from_json(const json& j, RtssLayout& l) {
  Get(j, "name", l.name);
  Get(j, "fontFace", l.fontFace);
  Get(j, "fontWeight", l.fontWeight);
  Get(j, "layers", l.layers);
  Get(j, "sources", l.sources);
  Get(j, "advanced", l.advanced);
  Get(j, "fontHeight", l.fontHeight);
  Get(j, "zoomRatio", l.zoomRatio);
  Get(j, "image", l.image);
  Get(j, "defs", l.defs);
}

void to_json(json& j, const SensorPick& s) {
  j = json{{"id", s.id}, {"label", s.label}, {"unit", s.unit}, {"group", s.group}};
}
void from_json(const json& j, SensorPick& s) {
  Get(j, "id", s.id);
  Get(j, "label", s.label);
  Get(j, "unit", s.unit);
  Get(j, "group", s.group);
}

void to_json(json& j, const Profile& p) {
  j = json{{"name", p.name},
           {"match", p.match},
           {"style", p.style},
           {"layout", p.layout},
           {"layouts", p.layouts},
           {"fields", p.fields},
           {"sensors", p.sensors},
           {"rtss", p.rtss},
           {"position", p.position},
           {"x", p.x},
           {"y", p.y},
           {"margin", p.margin},
           {"fontFamily", p.fontFamily},
           {"fontSize", p.fontSize},
           {"bold", p.bold},
           {"autoScale", p.autoScale},
           {"colors", p.colors},
           {"show", p.show},
           {"fpsRedBelow", p.fpsRedBelow},
           {"fpsYellowBelow", p.fpsYellowBelow},
           {"bgOpacity", p.bgOpacity}};
}
void from_json(const json& j, Profile& p) {
  Get(j, "name", p.name);
  Get(j, "match", p.match);
  Get(j, "style", p.style);
  Get(j, "layout", p.layout);
  Get(j, "layouts", p.layouts);
  Get(j, "fields", p.fields);
  Get(j, "sensors", p.sensors);
  Get(j, "rtss", p.rtss);
  if (p.layouts.empty() && j.contains("items")) {  // formato precedente: un solo layout 16:9
    std::vector<LayoutItem> old;
    Get(j, "items", old);
    p.layouts[kDefaultLayoutFormat] = old;
  }
  Get(j, "position", p.position);
  Get(j, "x", p.x);
  Get(j, "y", p.y);
  Get(j, "margin", p.margin);
  Get(j, "fontFamily", p.fontFamily);
  Get(j, "fontSize", p.fontSize);
  Get(j, "bold", p.bold);
  Get(j, "autoScale", p.autoScale);
  Get(j, "colors", p.colors);
  Get(j, "show", p.show);
  Get(j, "fpsRedBelow", p.fpsRedBelow);
  Get(j, "fpsYellowBelow", p.fpsYellowBelow);
  Get(j, "bgOpacity", p.bgOpacity);
  Sanitize(p);
}

void to_json(json& j, const GlobalConfig& c) {
  j = json{{"refreshMs", c.refreshMs},
           {"graphSeconds", c.graphSeconds},
           {"hotkeyToggle", c.hotkeyToggle},
           {"hotkeyCycleProfile", c.hotkeyCycleProfile},
           {"hotkeyFpsOnly", c.hotkeyFpsOnly},
           {"fpsOnly", c.fpsOnly},
           {"outOfGame", c.outOfGame},
           {"autostart", c.autostart},
           {"autoLearnGames", c.autoLearnGames},
           {"forcedProfile", c.forcedProfile},
           {"desktopProfile", c.desktopProfile}};
}
void from_json(const json& j, GlobalConfig& c) {
  Get(j, "refreshMs", c.refreshMs);
  Get(j, "graphSeconds", c.graphSeconds);
  Get(j, "hotkeyToggle", c.hotkeyToggle);
  Get(j, "hotkeyCycleProfile", c.hotkeyCycleProfile);
  Get(j, "hotkeyFpsOnly", c.hotkeyFpsOnly);
  Get(j, "fpsOnly", c.fpsOnly);
  Get(j, "outOfGame", c.outOfGame);
  Get(j, "autostart", c.autostart);
  Get(j, "autoLearnGames", c.autoLearnGames);
  Get(j, "forcedProfile", c.forcedProfile);
  Get(j, "desktopProfile", c.desktopProfile);
  Sanitize(c);
}

void to_json(json& j, const GameList& g) {
  j = json{{"known", g.known}, {"learned", g.learned}, {"exclude", g.exclude}};
}
void from_json(const json& j, GameList& g) {
  Get(j, "known", g.known);
  Get(j, "learned", g.learned);
  Get(j, "exclude", g.exclude);
  auto fix = [](std::vector<GameEntry>& v) {
    for (auto& e : v) e.process = ToLowerAscii(Trim(e.process));
    std::erase_if(v, [](const GameEntry& e) { return e.process.empty(); });
  };
  fix(g.known);
  fix(g.learned);
  for (auto& e : g.exclude) e = ToLowerAscii(Trim(e));
}

void Sanitize(Profile& p) {
  p.name = Trim(p.name);
  if (p.name.empty()) p.name = "profile";
  {
    std::vector<SensorPick> unique;
    for (auto& sp : p.sensors) {
      sp.label = Trim(sp.label);
      if (sp.id.empty() || std::any_of(unique.begin(), unique.end(), [&](const SensorPick& u) { return u.id == sp.id; }))
        continue;
      unique.push_back(sp);
    }
    p.sensors = std::move(unique);
  }
  p.style = "steam";  // unico stile: quello della barra Steam
  OneOf(p.layout, {"bar", "vertical", "free", "rtss"});
  if (p.layout == "rtss" && p.rtss.layers.empty()) p.layout = "bar";  // nessun preset importato
  // Layout libero: solo formati noti, 16:9 sempre presente, un blocco per ogni elemento,
  // coordinate dentro la tela del formato.
  std::erase_if(p.layouts, [](const auto& kv) {
    return std::none_of(std::begin(kLayoutFormats), std::end(kLayoutFormats),
                        [&](const LayoutFormat& f) { return kv.first == f.id; });
  });
  if (!p.layouts.contains(kDefaultLayoutFormat)) p.layouts[kDefaultLayoutFormat] = DefaultLayoutItems();
  for (auto& [fmt, current] : p.layouts) {
    const int width = LayoutFormatWidth(fmt);
    const auto defaults = ScaleLayout(DefaultLayoutItems(), 1920, width);
    std::vector<LayoutItem> items;
    for (const char* el : kLayoutElements) {
      auto it = std::find_if(current.begin(), current.end(), [&](const LayoutItem& i) { return i.element == el; });
      LayoutItem li = it != current.end() ? *it
                                          : *std::find_if(defaults.begin(), defaults.end(),
                                                          [&](const LayoutItem& i) { return i.element == el; });
      li.x = std::clamp(li.x, 0, width - 1);
      li.y = std::clamp(li.y, 0, kLayoutRefH - 1);
      li.scale = std::clamp(li.scale, 40, 400);
      items.push_back(li);
    }
    // Un blocco per ogni sensore scelto: quelli nuovi vanno in colonna sotto gli elementi fissi.
    int nextY = 310;
    for (const auto& li : items) nextY = std::max(nextY, li.y + 40);
    for (const auto& sp : p.sensors) {
      const std::string el = SensorElement(sp.id);
      auto it = std::find_if(current.begin(), current.end(), [&](const LayoutItem& i) { return i.element == el; });
      LayoutItem li = it != current.end() ? *it : LayoutItem{el, 20, std::min(nextY, kLayoutRefH - 40), 100};
      if (it == current.end()) nextY += 40;
      li.x = std::clamp(li.x, 0, width - 1);
      li.y = std::clamp(li.y, 0, kLayoutRefH - 1);
      li.scale = std::clamp(li.scale, 40, 400);
      items.push_back(li);
    }
    current = std::move(items);
  }
  OneOf(p.position, {"top-left", "top-right", "bottom-left", "bottom-right", "custom"});
  p.fontFamily = Trim(p.fontFamily);
  if (p.fontFamily.empty()) p.fontFamily = "sans";
  p.fontSize = std::clamp(p.fontSize, 6.0f, 96.0f);
  p.bgOpacity = std::clamp(p.bgOpacity, 0, 100);
  p.margin = std::clamp(p.margin, 0, 1000);
  p.fpsRedBelow = std::clamp(p.fpsRedBelow, 0, 1000);
  p.fpsYellowBelow = std::clamp(p.fpsYellowBelow, p.fpsRedBelow, 1000);
  for (auto& m : p.match) {
    m.process = ToLowerAscii(Trim(m.process));
    m.window = Trim(m.window);
  }
  std::erase_if(p.match, [](const GameEntry& e) { return e.process.empty(); });
}

void Sanitize(GlobalConfig& c) {
  c.refreshMs = std::clamp(c.refreshMs, 100, 5000);
  c.graphSeconds = std::clamp(c.graphSeconds, 10, 300);
  OneOf(c.outOfGame, {"hide", "compact"});
  c.forcedProfile = Trim(c.forcedProfile);
}

GameList DefaultGameList() {
  GameList g;
  g.known = {
      {"perfoverlaytest.exe", ""},  // finestra di prova DirectX 11/12
      {"cs2.exe", ""},
      {"dota2.exe", ""},
      {"eldenring.exe", ""},
      {"cyberpunk2077.exe", ""},
      {"rdr2.exe", ""},
      {"witcher3.exe", ""},
      {"bg3.exe", ""},
      {"bg3_dx11.exe", ""},
      {"doometernalx64vk.exe", ""},
      {"rocketleague.exe", ""},
      {"minecraft.windows.exe", ""},
      {"javaw.exe", "Minecraft"},
  };
  g.exclude = {
      // browser
      "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe", "brave.exe", "vivaldi.exe", "msedgewebview2.exe",
      // editor / office
      "code.exe", "devenv.exe", "notepad.exe", "notepad++.exe", "sublime_text.exe", "idea64.exe", "winword.exe",
      "excel.exe", "powerpnt.exe", "acrobat.exe",
      // player / streaming / chat
      "vlc.exe", "mpc-hc64.exe", "mpv.exe", "potplayermini64.exe", "obs64.exe", "discord.exe", "spotify.exe",
      "ms-teams.exe", "zoom.exe",
      // launcher e shell
      "steam.exe", "steamwebhelper.exe", "epicgameslauncher.exe", "galaxyclient.exe", "battle.net.exe",
      "eadesktop.exe", "ubisoftconnect.exe", "xboxpcapp.exe", "explorer.exe", "applicationframehost.exe",
      "searchhost.exe", "startmenuexperiencehost.exe", "shellexperiencehost.exe", "taskmgr.exe",
      "perfoverlay.exe", "perfoverlay-settings.exe", "perfoverlaysupreme.exe",
      "perfoverlaysupreme-settings.exe"};
  return g;
}

// ---------------------------------------------------------------- file
std::optional<json> ReadJsonFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  auto j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
  if (j.is_discarded()) {
    LogWarn("JSON non valido: {}", ToUtf8(p.wstring()));
    return std::nullopt;
  }
  return j;
}

bool WriteJsonFile(const fs::path& p, const json& j) {
  auto tmp = p;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      LogError("Impossibile scrivere {}", ToUtf8(tmp.wstring()));
      return false;
    }
    f << j.dump(2);
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);  // MSVC: MoveFileEx con REPLACE_EXISTING
  if (ec) {
    LogError("Rinomina di {} fallita: {}", ToUtf8(p.wstring()), ec.message());
    return false;
  }
  return true;
}

GlobalConfig LoadConfig() {
  GlobalConfig c;
  if (auto j = ReadJsonFile(ConfigPath())) {
    try {
      c = j->get<GlobalConfig>();
    } catch (const std::exception& e) {
      LogWarn("config.json non leggibile, uso i default: {}", e.what());
    }
  } else if (!fs::exists(ConfigPath())) {
    SaveConfig(c);
  }
  return c;
}

bool SaveConfig(const GlobalConfig& c) { return WriteJsonFile(ConfigPath(), json(c)); }

GameList LoadGames() {
  if (auto j = ReadJsonFile(GamesPath())) {
    try {
      return j->get<GameList>();
    } catch (const std::exception& e) {
      LogWarn("games.json non leggibile, uso la lista predefinita: {}", e.what());
      return DefaultGameList();
    }
  }
  auto g = DefaultGameList();
  if (!fs::exists(GamesPath())) SaveGames(g);
  return g;
}

bool SaveGames(const GameList& g) { return WriteJsonFile(GamesPath(), json(g)); }

// ---------------------------------------------------------------- profili
fs::path ProfilePath(const std::string& name) {
  return ProfilesDir() / (ToWide(SanitizeFileName(name)) + L".json");
}

std::vector<Profile> LoadProfiles() {
  std::vector<Profile> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(ProfilesDir(), ec)) {
    if (!e.is_regular_file() || e.path().extension() != L".json") continue;
    auto j = ReadJsonFile(e.path());
    if (!j) continue;
    try {
      Profile p = j->get<Profile>();
      if (!j->contains("name")) p.name = ToUtf8(e.path().stem().wstring());
      if (FindProfile(out, p.name)) {
        LogWarn("Profilo duplicato '{}' in {}, ignorato", p.name, ToUtf8(e.path().wstring()));
        continue;
      }
      out.push_back(std::move(p));
    } catch (const std::exception& ex) {
      LogWarn("Profilo {} non valido: {}", ToUtf8(e.path().wstring()), ex.what());
    }
  }
  if (!FindProfile(out, "default")) {
    Profile d;
    SaveProfile(d);
    out.push_back(d);
  }
  std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) {
    const bool ad = IEquals(a.name, "default"), bd = IEquals(b.name, "default");
    if (ad != bd) return ad;
    return ToLowerAscii(a.name) < ToLowerAscii(b.name);
  });
  return out;
}

bool SaveProfile(const Profile& p) { return WriteJsonFile(ProfilePath(p.name), json(p)); }

bool DeleteProfileFile(const std::string& name) {
  std::error_code ec;
  return fs::remove(ProfilePath(name), ec);
}

const Profile* FindProfile(const std::vector<Profile>& all, std::string_view name) {
  for (const auto& p : all)
    if (IEquals(p.name, name)) return &p;
  return nullptr;
}

bool MatchesEntry(const GameEntry& e, std::string_view exeLower, std::string_view title) {
  if (e.process != "*" && e.process != exeLower) return false;
  return e.window.empty() || IContains(title, e.window);
}

const Profile& SelectProfile(const std::vector<Profile>& all, std::string_view exeLower, std::string_view title,
                             std::string_view forced) {
  if (!forced.empty())
    if (const Profile* p = FindProfile(all, forced)) return *p;
  if (!exeLower.empty() || !title.empty()) {
    for (const auto& p : all)
      for (const auto& m : p.match)
        if (MatchesEntry(m, exeLower, title)) return p;
  }
  const Profile* d = FindProfile(all, "default");
  return d ? *d : all.front();
}

// ---------------------------------------------------------------- export/import
json ExportProfile(const Profile& p) { return json{{"perfoverlay", "profile"}, {"version", 1}, {"profile", p}}; }

json ExportAll(const GlobalConfig& c, const GameList& g, const std::vector<Profile>& profiles) {
  return json{{"perfoverlay", "full"}, {"version", 1}, {"config", c}, {"games", g}, {"profiles", profiles}};
}

json ExportConfig(const GlobalConfig& c, const GameList& g) {
  return json{{"perfoverlay", "config"}, {"version", 1}, {"config", c}, {"games", g}};
}

std::optional<ImportBundle> ParseImport(const json& j) {
  if (!j.is_object()) return std::nullopt;
  ImportBundle b;
  try {
    if (j.contains("perfoverlay")) {
      if (j.contains("profile")) b.profiles.push_back(j.at("profile").get<Profile>());
      if (j.contains("profiles")) b.profiles = j.at("profiles").get<std::vector<Profile>>();
      if (j.contains("config")) b.config = j.at("config").get<GlobalConfig>();
      if (j.contains("games")) b.games = j.at("games").get<GameList>();
    } else if (j.contains("style") || j.contains("show") || j.contains("colors")) {
      b.profiles.push_back(j.get<Profile>());  // file di profilo grezzo
    } else if (j.contains("refreshMs") || j.contains("hotkeyToggle")) {
      b.config = j.get<GlobalConfig>();  // config.json grezzo
    } else {
      return std::nullopt;
    }
  } catch (const std::exception& e) {
    LogWarn("Import non valido: {}", e.what());
    return std::nullopt;
  }
  if (b.profiles.empty() && !b.config && !b.games) return std::nullopt;
  return b;
}

}  // namespace po
