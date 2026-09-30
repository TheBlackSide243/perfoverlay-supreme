#pragma once
#include <cstdint>
#include <map>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace po {

struct Color {
  uint8_t r = 255, g = 255, b = 255, a = 255;
};
Color ParseColor(std::string_view hex, Color fallback);
std::string ColorToHex(Color c);

// Firma di un gioco: nome processo (minuscolo, "*" = qualsiasi) + sottostringa opzionale del titolo finestra.
struct GameEntry {
  std::string process;
  std::string window;
};

struct ShowFlags {
  bool fps = true, graph = true, frametime = true, percentiles = false, stutter = false;
  bool cpu = true, gpu = true, ram = true, battery = true;
};

struct ElementColors {
  Color fps{0xF2, 0x6D, 0x6D};
  Color cpu{0xD8, 0xCB, 0x62};
  Color gpu{0x6F, 0xD6, 0x6F};
  Color ram{0xB4, 0x9D, 0xF0};
  Color battery{0x6C, 0xBC, 0xF2};
  Color text{0xF2, 0xF2, 0xF2};
  Color background{0x10, 0x10, 0x14};
};

// Blocco del layout libero (stile OverlayEditor di RTSS): posizione in pixel su una tela alta
// 1080 px e larga secondo il formato, riportata in proporzione sull'area del gioco.
struct LayoutItem {
  std::string element;  // fps | graph | frametime | cpu | gpu | ram | battery
  int x = 0, y = 0;
  int scale = 100;      // % della dimensione font del profilo
};
inline constexpr int kLayoutRefH = 1080;
inline constexpr const char* kLayoutElements[] = {"fps", "graph", "frametime", "cpu", "gpu", "ram", "battery"};

// Formati della tela: ogni profilo può avere un layout diverso per formato; l'overlay usa quello
// più vicino al formato del gioco.
struct LayoutFormat {
  const char* id;
  int width;  // larghezza della tela a 1080 px di altezza
  const wchar_t* label;
};
inline constexpr LayoutFormat kLayoutFormats[] = {
    {"16:9", 1920, L"16:9  (1920 x 1080)"},  {"16:10", 1728, L"16:10  (1728 x 1080)"},
    {"21:9", 2560, L"21:9  (2560 x 1080)"},  {"32:9", 3840, L"32:9  (3840 x 1080)"},
    {"4:3", 1440, L"4:3  (1440 x 1080)"}};
inline constexpr const char* kDefaultLayoutFormat = "16:9";
int LayoutFormatWidth(std::string_view id);
std::string ClosestLayoutFormat(double aspect);  // tra tutti i formati noti
std::vector<LayoutItem> DefaultLayoutItems();    // disposizione 16:9 predefinita
std::vector<LayoutItem> ScaleLayout(const std::vector<LayoutItem>& items, int fromWidth, int toWidth);

// Campi (spunte) di ogni elemento: quali dati mostrare dentro il blocco.
struct LayoutField {
  const char* element;
  const char* key;
  const wchar_t* label;
  bool byDefault;
};
inline constexpr LayoutField kLayoutFields[] = {
    {"fps", "value", L"Framerate", true},
    {"fps", "minmax", L"Minimo / massimo  (↓ ↑)", true},
    {"fps", "low1", L"1% low", false},
    {"frametime", "value", L"Frame time (ms)", true},
    {"frametime", "p95", L"P95", false},
    {"frametime", "p99", L"P99", false},
    {"frametime", "stutter", L"Stutter %", false},
    {"cpu", "usage", L"Utilizzo totale", true},
    {"cpu", "topcore", L"Core più carico", true},
    {"cpu", "temp", L"Temperatura", true},
    {"cpu", "freq", L"Frequenza media", true},
    {"cpu", "maxfreq", L"Frequenza massima  (↑)", true},
    {"gpu", "usage", L"Utilizzo", true},
    {"gpu", "temp", L"Temperatura", true},
    {"gpu", "clock", L"Frequenza", true},
    {"gpu", "vram", L"VRAM usata / totale", true},
    {"ram", "used", L"Usata / totale", true},
    {"ram", "speed", L"Velocità", true},
    {"battery", "percent", L"Percentuale", true},
    {"battery", "state", L"Stato (in carica / a batteria)", true},
};
inline constexpr int kMaxFieldsPerElement = 5;

// Preset importato dall'OverlayEditor di RivaTuner (.ovx/.ovl): layer di testo posizionati su una
// griglia a caratteri (valori negativi = celle di testo, positivi = pixel), come in RTSS.
struct RtssLayer {
  std::string text;      // con macro %Nome sorgente%, %Time12%, ...
  int x = 0, y = 0;      // posizione
  int extentX = 0, extentY = 0;
  int origin = 0;        // allineamento nel riquadro: 0..8 (riga*3 + colonna)
  int size = 100;        // % della dimensione del font
  Color color{255, 255, 255};
  // Solo preset avanzati (motore completo):
  std::string name;
  std::string colorSpec;   // TextColor originale, anche con soglie "c1,min,max,c2,...(sorgente,flag)"
  std::string bgColor;     // BgndColor
  std::string visibility;  // VisibilitySource: layer mostrato solo se la sorgente è diversa da 0
  int marginTop = 0, marginBottom = 0;
};
struct RtssSource {
  std::string metric;    // dato di PerfOverlay collegato (vuoto = non supportato → "N/A")
  int decimals = 0;
};
// Sorgente dati di un preset avanzato, come definita nell'OverlayEditor.
struct RtssSourceDef {
  std::string name, units, format, formula;
  std::string provider;  // HAL | HwInfo | PresentMon | MSI Afterburner
  std::string id;        // ID del dato (HAL / PresentMon), "Timer", "Stub"
  std::string reading;   // HwInfo: ReadingName
};
struct RtssLayout {
  std::string name;      // nome del file importato
  std::string fontFace = "Unispace";
  int fontWeight = 400;
  std::vector<RtssLayer> layers;
  std::map<std::string, RtssSource> sources;  // nome sorgente RTSS → dato
  // Preset avanzati (ipertesto, formule, immagini, grafici: es. TroyMetrics) → motore completo.
  bool advanced = false;
  int fontHeight = 0;    // [Master] FontHeight
  int zoomRatio = 1;     // [Master] ZoomRatio
  std::string image;     // EmbeddedImage, copiata in %APPDATA%\PerfOverlay Supreme\presets
  std::vector<RtssSourceDef> defs;
};
void to_json(nlohmann::json& j, const RtssSourceDef& s);
void from_json(const nlohmann::json& j, RtssSourceDef& s);
void to_json(nlohmann::json& j, const RtssLayer& l);
void from_json(const nlohmann::json& j, RtssLayer& l);
void to_json(nlohmann::json& j, const RtssSource& s);
void from_json(const nlohmann::json& j, RtssSource& s);
void to_json(nlohmann::json& j, const RtssLayout& l);
void from_json(const nlohmann::json& j, RtssLayout& l);

// Sensore scelto nella finestra "Sensori di sistema" da mostrare nell'overlay. Nel layout libero è
// un blocco a sé ("sensor:<id>"); nomi e unità restano salvati anche se il sensore sparisce.
struct SensorPick {
  std::string id;
  std::string label;  // etichetta nell'overlay
  std::string unit;
  std::string group;  // hardware di provenienza (solo per mostrarlo nelle impostazioni)
};
inline constexpr std::string_view kSensorElementPrefix = "sensor:";
inline bool IsSensorElement(std::string_view el) { return el.starts_with(kSensorElementPrefix); }
inline std::string SensorElement(const std::string& id) { return std::string(kSensorElementPrefix) + id; }
void to_json(nlohmann::json& j, const SensorPick& s);
void from_json(const nlohmann::json& j, SensorPick& s);

struct Profile {
  std::string name = "default";
  std::vector<GameEntry> match;       // giochi a cui si applica (vuoto per "default")
  std::string style = "steam";        // steam | minimal | neon
  std::string layout = "bar";         // bar | vertical | free | rtss
  RtssLayout rtss;                    // solo per layout "rtss"
  std::map<std::string, std::vector<LayoutItem>> layouts;  // layout "free": formato → blocchi
  std::string position = "top-left";  // top-left | top-right | bottom-left | bottom-right | custom
  int x = 0, y = 0;                   // offset per "custom", relativo all'area client del gioco
  int margin = 6;
  std::string fontFamily = "sans";    // mono | sans | <nome font installato>
  float fontSize = 15.0f;             // px a 1080p se autoScale, altrimenti px a 96 DPI
  bool bold = false;
  bool autoScale = true;              // dimensiona in base alla risoluzione del gioco
  static constexpr int kReferenceHeight = 1080;
  ElementColors colors;
  ShowFlags show;
  int fpsRedBelow = 30;
  int fpsYellowBelow = 60;
  int bgOpacity = 55;                 // 0..100

  std::map<std::string, bool> fields;  // "cpu.temp" → mostrato; assente = valore predefinito
  std::vector<SensorPick> sensors;     // sensori extra mostrati dopo gli elementi fissi

  // Campo di un elemento acceso/spento (per P95/P99/stutter vale insieme a show.percentiles/stutter).
  bool Field(std::string_view element, std::string_view key) const;
  void SetField(std::string_view element, std::string_view key, bool on);

  // Layout libero da usare per un gioco con questo rapporto larghezza/altezza: il formato più
  // vicino tra quelli disegnati dall'utente (16:9 esiste sempre).
  const std::vector<LayoutItem>& LayoutFor(double aspect, int* canvasWidth) const;
};

struct GlobalConfig {
  int refreshMs = 500;
  int graphSeconds = 60;
  std::string hotkeyToggle = "Ctrl+Shift+O";
  std::string hotkeyCycleProfile = "Ctrl+Shift+P";
  std::string hotkeyFpsOnly = "Ctrl+Shift+F";
  bool fpsOnly = false;
  std::string outOfGame = "hide";     // hide | compact
  bool autostart = false;
  bool autoLearnGames = true;
  std::string forcedProfile;          // vuoto = selezione automatica
  std::string desktopProfile;         // profilo mostrato sul desktop (quello selezionato nelle impostazioni)
  std::string language;               // "it" / "en"; vuoto = lingua di Windows
};

struct GameList {
  std::vector<GameEntry> known;
  std::vector<GameEntry> learned;     // aggiunti dall'euristica
  std::vector<std::string> exclude;   // mai considerati giochi
};

void to_json(nlohmann::json& j, const Color& c);
void from_json(const nlohmann::json& j, Color& c);
void to_json(nlohmann::json& j, const GameEntry& g);
void from_json(const nlohmann::json& j, GameEntry& g);
void to_json(nlohmann::json& j, const ShowFlags& s);
void from_json(const nlohmann::json& j, ShowFlags& s);
void to_json(nlohmann::json& j, const ElementColors& c);
void from_json(const nlohmann::json& j, ElementColors& c);
void to_json(nlohmann::json& j, const LayoutItem& i);
void from_json(const nlohmann::json& j, LayoutItem& i);
void to_json(nlohmann::json& j, const Profile& p);
void from_json(const nlohmann::json& j, Profile& p);
void to_json(nlohmann::json& j, const GlobalConfig& c);
void from_json(const nlohmann::json& j, GlobalConfig& c);
void to_json(nlohmann::json& j, const GameList& g);
void from_json(const nlohmann::json& j, GameList& g);

void Sanitize(Profile& p);
void Sanitize(GlobalConfig& c);
GameList DefaultGameList();

std::optional<nlohmann::json> ReadJsonFile(const std::filesystem::path& p);
bool WriteJsonFile(const std::filesystem::path& p, const nlohmann::json& j);

GlobalConfig LoadConfig();
bool SaveConfig(const GlobalConfig& c);
GameList LoadGames();
bool SaveGames(const GameList& g);

// Profili: un file per profilo in %APPDATA%\PerfOverlay\profiles\<Nome>.json.
// Il risultato contiene sempre "default" in prima posizione.
std::vector<Profile> LoadProfiles();
bool SaveProfile(const Profile& p);
bool DeleteProfileFile(const std::string& name);
std::filesystem::path ProfilePath(const std::string& name);
const Profile* FindProfile(const std::vector<Profile>& all, std::string_view name);
bool MatchesEntry(const GameEntry& e, std::string_view exeLower, std::string_view title);
const Profile& SelectProfile(const std::vector<Profile>& all, std::string_view exeLower, std::string_view title,
                             std::string_view forced);

// Esportazione / importazione
nlohmann::json ExportProfile(const Profile& p);
nlohmann::json ExportAll(const GlobalConfig& c, const GameList& g, const std::vector<Profile>& profiles);
nlohmann::json ExportConfig(const GlobalConfig& c, const GameList& g);
struct ImportBundle {
  std::vector<Profile> profiles;
  std::optional<GlobalConfig> config;
  std::optional<GameList> games;
};
std::optional<ImportBundle> ParseImport(const nlohmann::json& j);

}  // namespace po
