#include "settings/settings_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/branding.h"
#include "common/darkmode.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/resources.h"
#include "common/rtss_preset.h"
#include "common/util.h"
#include "common/winutil.h"
#include "settings/gallery.h"
#include "settings/games_editor.h"
#include "settings/layout_editor.h"
#include "settings/sensor_browser.h"
#include "settings/theme.h"
#include "common/i18n.h"

namespace po {
namespace {

// ------------------------------------------------------------------ ID controlli
enum : int {
  IDC_LIST = 1000,
  IDC_NEW,
  IDC_DELETE,
  IDC_EXPORT_ONE,
  IDC_IMPORT_ONE,
  IDC_EXPORT_ALL,
  IDC_IMPORT_ALL,
  IDC_BACKUP_CFG,
  IDC_RESTORE_CFG,
  IDC_NAME,
  IDC_MATCH,
  IDC_LAYOUT,
  IDC_POSITION,
  IDC_X,
  IDC_Y,
  IDC_FONT,
  IDC_FONTSIZE,
  IDC_BOLD,
  IDC_OPACITY,
  IDC_FPSRED,
  IDC_FPSYELLOW,
  IDC_AUTOSCALE,
  IDC_MARGIN,
  IDC_COLOR_BTN = 1100,     // + indice colore
  IDC_COLOR_SWATCH = 1120,  // + indice colore
  IDC_SHOW = 1140,          // + indice elemento
  IDC_REFRESH = 1200,
  IDC_HK_TOGGLE,
  IDC_HK_CYCLE,
  IDC_HK_FPSONLY,
  IDC_OUTOFGAME,
  IDC_FORCED,
  IDC_FPSONLY,
  IDC_AUTOLEARN,
  IDC_AUTOSTART,
  IDC_EDIT_GAMES = 1300,
  IDC_OPEN_FOLDER,
  IDC_SAVE,
  IDC_STATUS,
  IDC_LAUNCH,
  IDC_EDITOR,
  IDC_IMPORT_RTSS,
  IDC_SENSORS,
  IDC_STORE,
  IDC_LANG,
  IDC_HK_BTN = 1260,  // + indice hotkey (stesso ordine di IDC_HK_TOGGLE..)
};

constexpr const char* kLayouts[] = {"bar", "vertical", "free", "rtss"};
const wchar_t* const kLayoutNames[] = {T(L"Barra orizzontale"), T(L"Verticale"), T(L"Libero (editor)"),
                                           T(L"Preset RTSS (importato)")};
constexpr const char* kPositions[] = {"top-left", "top-right", "bottom-left", "bottom-right", "custom"};
const wchar_t* const kPositionNames[] = {T(L"In alto a sinistra"), T(L"In alto a destra"), T(L"In basso a sinistra"),
                                             T(L"In basso a destra"), T(L"Personalizzata (X/Y)")};
constexpr const char* kOutOfGame[] = {"hide", "compact"};
// La lingua si mostra nel proprio nome, così si ritrova anche senza capire quella attiva.
constexpr const char* kLangCodes[] = {"it", "en"};
constexpr const wchar_t* kLangNames[] = {L"Italiano", L"English"};
const wchar_t* const kOutOfGameNames[] = {T(L"Nascondi"), T(L"Profilo selezionato sul desktop")};

constexpr int kColorCount = 7;
const wchar_t* const kColorNames[kColorCount] = {L"FPS", L"CPU", L"GPU", L"RAM", T(L"Batteria"), T(L"Testo"), T(L"Sfondo")};
constexpr int kShowCount = 9;
const wchar_t* const kShowNames[kShowCount] = {L"FPS",   T(L"Mini-grafico FPS"), L"Frame time",
                                                   L"P95 / P99", L"Stutter %",        L"CPU",
                                                   L"GPU",   L"RAM",              T(L"Batteria (solo portatili)")};

Color& ColorRef(Profile& p, int i) {
  auto& c = p.colors;
  Color* all[kColorCount] = {&c.fps, &c.cpu, &c.gpu, &c.ram, &c.battery, &c.text, &c.background};
  return *all[i];
}

bool& ShowRef(Profile& p, int i) {
  auto& s = p.show;
  bool* all[kShowCount] = {&s.fps, &s.graph, &s.frametime, &s.percentiles, &s.stutter,
                           &s.cpu, &s.gpu,   &s.ram,       &s.battery};
  return *all[i];
}

template <size_t N>
int IndexOf(const char* const (&arr)[N], const std::string& v) {
  for (size_t i = 0; i < N; ++i)
    if (v == arr[i]) return int(i);
  return 0;
}

// ------------------------------------------------------------------ stato
struct State {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr;
  HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSection = nullptr;
  HBRUSH brBg = nullptr, brPanel = nullptr, brInput = nullptr, brInputOff = nullptr;
  HICON icon = nullptr;
  int dpi = 96;
  GlobalConfig cfg;
  GameList games;
  std::vector<Profile> profiles;
  int cur = -1;
  std::vector<std::string> removed;  // file di profilo da cancellare al salvataggio
  COLORREF custom[16] = {};
  std::wstring status;
  COLORREF statusColor = ui::col::Sub;
  bool monitorRunning = false;
  int focusedEdit = 0;
};
State g;

constexpr COLORREF kInputOff = RGB(32, 32, 38);  // campo disattivato
constexpr int kClientW = 1000, kClientH = 806;
constexpr int kRowH = 28;
constexpr UINT_PTR kTimerMonitor = 1;

// Elementi disegnati direttamente sulla finestra (coordinate a 96 DPI).
struct PanelItem {
  RECT r;
  const wchar_t* title;
};
struct LabelItem {
  RECT r;
  const wchar_t* text;
};
struct FrameItem {
  int id;
  RECT r;
};
std::vector<PanelItem> g_panels;
std::vector<LabelItem> g_labels;
std::vector<FrameItem> g_frames;

int S(int v) { return MulDiv(v, g.dpi, 96); }
RECT SR(const RECT& r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }
HWND Item(int id) { return GetDlgItem(g.wnd, id); }

// ------------------------------------------------------------------ helper controlli
HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g.wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
  return c;
}
void Panel(const wchar_t* title, int x, int y, int w, int h) { g_panels.push_back({{x, y, x + w, y + h}, title}); }
void Label(const wchar_t* t, int x, int y, int w) { g_labels.push_back({{x, y, x + w, y + kRowH}, t}); }
// Campo di testo senza bordo: la cornice arrotondata la disegna la finestra (g_frames).
void Edit(int id, int x, int y, int w, const wchar_t* cue = nullptr) {
  g_frames.push_back({id, {x, y, x + w, y + kRowH}});
  HWND e = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x + 9, y + 6, w - 18, kRowH - 11, id);
  if (cue) SendMessageW(e, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
}
void Button(int id, const wchar_t* t, int x, int y, int w, int h = 30, COLORREF bg = ui::col::Bg,
            bool primary = false) {
  ui::MakeButton(Make(L"BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, id), bg, primary);
}
void Check(int id, const wchar_t* t, int x, int y, int w) { Make(ui::kCheckClass, t, WS_TABSTOP, x, y, w, 24, id); }
template <size_t N>
void Combo(int id, int x, int y, int w, const wchar_t* const (&items)[N], bool editable = false) {
  HWND c = Make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST), x, y + 1,
                w, 240, id);
  ApplyDarkControlTheme(c, L"DarkMode_CFD");
  SendMessageW(c, CB_SETITEMHEIGHT, WPARAM(-1), S(kRowH - 8));  // campo alto quanto le altre righe
  SendMessageW(c, CB_SETITEMHEIGHT, 0, S(22));
  for (const wchar_t* it : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it));
}

std::wstring GetText(int id) {
  HWND c = Item(id);
  std::wstring s(size_t(GetWindowTextLengthW(c)) + 1, L'\0');
  s.resize(size_t(GetWindowTextW(c, s.data(), int(s.size()))));
  return s;
}
std::string GetUtf8(int id) { return Trim(ToUtf8(GetText(id))); }
void SetText(int id, const std::wstring& s) { SetWindowTextW(Item(id), s.c_str()); }
void SetUtf8(int id, const std::string& s) { SetText(id, ToWide(s)); }
int GetInt(int id, int fallback) {
  const auto s = GetUtf8(id);
  if (s.empty()) return fallback;
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}
void SetInt(int id, int v) { SetText(id, std::to_wstring(v)); }
bool GetCheck(int id) { return Button_GetCheck(Item(id)) == BST_CHECKED; }
void SetCheck(int id, bool v) { Button_SetCheck(Item(id), v ? BST_CHECKED : BST_UNCHECKED); }
int GetSel(int id) { return std::max(0, ComboBox_GetCurSel(Item(id))); }
void SetSel(int id, int i) { ComboBox_SetCurSel(Item(id), i); }

// Riga di stato in basso: grigia per le informazioni, verde/rossa per l'esito del salvataggio.
RECT StatusRect() { return SR({386, 756, 814, 788}); }
void Status(const std::wstring& s, COLORREF color = ui::col::Sub) {
  g.status = s;
  g.statusColor = color;
  const RECT r = StatusRect();
  InvalidateRect(g.wnd, &r, FALSE);
}

// Abilita/disabilita un campo e ridisegna la sua cornice.
void EnableField(int id, bool on) {
  EnableWindow(Item(id), on);
  for (const auto& f : g_frames)
    if (f.id == id) {
      RECT r = SR(f.r);
      InvalidateRect(g.wnd, &r, FALSE);
    }
}

// ------------------------------------------------------------------ profili ↔ UI
std::string MatchToText(const std::vector<GameEntry>& m) {
  std::string s;
  for (const auto& e : m) {
    if (!s.empty()) s += "; ";
    s += e.process;
    if (!e.window.empty()) s += "|" + e.window;
  }
  return s;
}

std::vector<GameEntry> TextToMatch(const std::string& text) {
  std::vector<GameEntry> out;
  for (const auto& part : Split(text, ';')) {
    const auto fields = Split(part, '|');
    GameEntry e{ToLowerAscii(Trim(fields[0])), fields.size() > 1 ? Trim(fields[1]) : ""};
    if (!e.process.empty()) out.push_back(e);
  }
  return out;
}

bool IsDefault(const Profile& p) { return IEquals(p.name, "default"); }

std::string UniqueName(const std::string& base) {
  if (!FindProfile(g.profiles, base)) return base;
  for (int i = 2;; ++i) {
    auto n = std::format("{} ({})", base, i);
    if (!FindProfile(g.profiles, n)) return n;
  }
}

void UpdateSwatch(int i) { InvalidateRect(Item(IDC_COLOR_BTN + i), nullptr, FALSE); }

void ProfileToUi() {
  if (g.cur < 0) return;
  const Profile& p = g.profiles[size_t(g.cur)];
  SetUtf8(IDC_NAME, p.name);
  SetUtf8(IDC_MATCH, MatchToText(p.match));
  SetSel(IDC_LAYOUT, IndexOf(kLayouts, p.layout));
  SetSel(IDC_POSITION, IndexOf(kPositions, p.position));
  SetInt(IDC_X, p.x);
  SetInt(IDC_Y, p.y);
  SetUtf8(IDC_FONT, p.fontFamily);
  SetText(IDC_FONTSIZE, std::format(L"{:g}", p.fontSize));
  SetCheck(IDC_BOLD, p.bold);
  SetCheck(IDC_AUTOSCALE, p.autoScale);
  SetInt(IDC_OPACITY, p.bgOpacity);
  SetInt(IDC_MARGIN, p.margin);
  SetInt(IDC_FPSRED, p.fpsRedBelow);
  SetInt(IDC_FPSYELLOW, p.fpsYellowBelow);
  for (int i = 0; i < kShowCount; ++i) SetCheck(IDC_SHOW + i, ShowRef(g.profiles[size_t(g.cur)], i));
  for (int i = 0; i < kColorCount; ++i) UpdateSwatch(i);
  const bool def = IsDefault(p);
  EnableField(IDC_NAME, !def);
  EnableField(IDC_MATCH, !def);
  EnableWindow(Item(IDC_DELETE), !def);
  const bool custom = p.position == "custom";
  EnableField(IDC_X, custom);
  EnableField(IDC_Y, custom);
}

// Legge i controlli nel profilo corrente. false se il nome non è valido.
bool UiToProfile() {
  if (g.cur < 0) return true;
  Profile& p = g.profiles[size_t(g.cur)];
  if (!IsDefault(p)) {
    const std::string name = GetUtf8(IDC_NAME);
    if (name.empty() || IEquals(name, "default")) {
      MessageBoxW(g.wnd, T(L"Nome profilo non valido."), kAppName, MB_ICONWARNING);
      return false;
    }
    if (const Profile* other = FindProfile(g.profiles, name); other && other != &p) {
      MessageBoxW(g.wnd, T(L"Esiste già un profilo con questo nome."), kAppName, MB_ICONWARNING);
      return false;
    }
    if (name != p.name) {
      g.removed.push_back(p.name);
      p.name = name;
    }
    p.match = TextToMatch(GetUtf8(IDC_MATCH));
  }
  p.layout = kLayouts[GetSel(IDC_LAYOUT)];
  p.position = kPositions[GetSel(IDC_POSITION)];
  p.x = GetInt(IDC_X, p.x);
  p.y = GetInt(IDC_Y, p.y);
  p.fontFamily = GetUtf8(IDC_FONT);
  try {
    p.fontSize = std::stof(GetUtf8(IDC_FONTSIZE));
  } catch (...) {
  }
  p.bold = GetCheck(IDC_BOLD);
  p.autoScale = GetCheck(IDC_AUTOSCALE);
  p.bgOpacity = GetInt(IDC_OPACITY, p.bgOpacity);
  p.margin = GetInt(IDC_MARGIN, p.margin);
  p.fpsRedBelow = GetInt(IDC_FPSRED, p.fpsRedBelow);
  p.fpsYellowBelow = GetInt(IDC_FPSYELLOW, p.fpsYellowBelow);
  for (int i = 0; i < kShowCount; ++i) ShowRef(p, i) = GetCheck(IDC_SHOW + i);
  Sanitize(p);
  return true;
}

void RefreshForcedCombo() {
  HWND c = Item(IDC_FORCED);
  ComboBox_ResetContent(c);
  ComboBox_AddString(c, T(L"(automatico)"));
  int sel = 0;
  for (size_t i = 0; i < g.profiles.size(); ++i) {
    ComboBox_AddString(c, ToWide(g.profiles[i].name).c_str());
    if (IEquals(g.profiles[i].name, g.cfg.forcedProfile)) sel = int(i) + 1;
  }
  ComboBox_SetCurSel(c, sel);
}

void RefreshList() {
  HWND l = Item(IDC_LIST);
  ListBox_ResetContent(l);
  for (const auto& p : g.profiles) {
    // "nome	dettaglio": il dettaglio viene disegnato a destra, in grigio.
    std::wstring label = ToWide(p.name);
    if (IsDefault(p))
      label += L"	fallback";
    else if (!p.match.empty())
      label += std::format(L"	{} {}", p.match.size(), p.match.size() == 1 ? T(L"gioco") : T(L"giochi"));
    ListBox_AddString(l, label.c_str());
  }
  ListBox_SetCurSel(l, g.cur);
  RefreshForcedCombo();
}

void GlobalToUi() {
  SetInt(IDC_REFRESH, g.cfg.refreshMs);
  SetUtf8(IDC_HK_TOGGLE, g.cfg.hotkeyToggle);
  SetUtf8(IDC_HK_CYCLE, g.cfg.hotkeyCycleProfile);
  SetUtf8(IDC_HK_FPSONLY, g.cfg.hotkeyFpsOnly);
  SetSel(IDC_OUTOFGAME, IndexOf(kOutOfGame, g.cfg.outOfGame));
  SetSel(IDC_LANG, CurrentLang() == Lang::En ? 1 : 0);
  SetCheck(IDC_FPSONLY, g.cfg.fpsOnly);
  SetCheck(IDC_AUTOLEARN, g.cfg.autoLearnGames);
  SetCheck(IDC_AUTOSTART, g.cfg.autostart);
  RefreshForcedCombo();
}

bool UiToGlobal() {
  g.cfg.refreshMs = GetInt(IDC_REFRESH, g.cfg.refreshMs);
  const std::pair<int, std::string*> hks[] = {{IDC_HK_TOGGLE, &g.cfg.hotkeyToggle},
                                              {IDC_HK_CYCLE, &g.cfg.hotkeyCycleProfile},
                                              {IDC_HK_FPSONLY, &g.cfg.hotkeyFpsOnly}};
  for (const auto& [id, dst] : hks) {
    const std::string v = GetUtf8(id);
    unsigned mods, vk;
    if (!v.empty() && !ParseHotkey(v, mods, vk)) {
      MessageBoxW(g.wnd, (T(L"Hotkey non valida: ") + ToWide(v) + T(L"\nEsempi: Ctrl+Shift+O, Alt+F10")).c_str(),
                  kAppName, MB_ICONWARNING);
      return false;
    }
    *dst = v;
  }
  g.cfg.outOfGame = kOutOfGame[GetSel(IDC_OUTOFGAME)];
  const int forced = GetSel(IDC_FORCED);
  g.cfg.forcedProfile = forced > 0 && size_t(forced) <= g.profiles.size() ? g.profiles[size_t(forced - 1)].name : "";
  g.cfg.fpsOnly = GetCheck(IDC_FPSONLY);
  g.cfg.autoLearnGames = GetCheck(IDC_AUTOLEARN);
  g.cfg.autostart = GetCheck(IDC_AUTOSTART);
  Sanitize(g.cfg);
  return true;
}

// ------------------------------------------------------------------ salvataggio
bool SaveAll();

// Salva tutto nella nuova lingua e riapre le impostazioni, che vengono ricostruite con i nuovi testi.
void OnChangeLanguage() {
  const int sel = GetSel(IDC_LANG);
  const std::string code = kLangCodes[sel >= 0 && sel < 2 ? sel : 0];
  if (code == LangCode(CurrentLang())) return;
  if (!UiToProfile() || !UiToGlobal()) {
    SetSel(IDC_LANG, CurrentLang() == Lang::En ? 1 : 0);
    return;
  }
  g.cfg.language = code;
  SetLang(LangFromCode(code));
  if (!SaveAll()) return;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  ShellExecuteW(nullptr, L"open", exe, L"--settings", nullptr, SW_SHOWNORMAL);
  DestroyWindow(g.wnd);
}

bool SaveAll() {
  // Sul desktop compare il profilo selezionato qui.
  if (g.cur >= 0 && size_t(g.cur) < g.profiles.size()) g.cfg.desktopProfile = g.profiles[size_t(g.cur)].name;
  bool ok = SaveConfig(g.cfg) && SaveGames(g.games);
  for (const auto& p : g.profiles) ok = SaveProfile(p) && ok;
  for (const auto& old : g.removed) {
    // Cancella il file solo se nessun profilo attuale lo usa (es. rinomina solo di maiuscole).
    bool inUse = false;
    for (const auto& p : g.profiles) inUse = inUse || ProfilePath(p.name) == ProfilePath(old);
    if (!inUse) DeleteProfileFile(old);
  }
  g.removed.clear();
  // L'avvio con Windows lo applica PerfOverlay.exe (elevato) quando ricarica la config.
  const bool notified = NotifyMonitorReload();
  if (!ok)
    Status(T(L"Errore durante il salvataggio: vedi overlay.log"), ui::col::Bad);
  else
    Status(notified ? T(L"Salvato. Overlay aggiornato.") : T(L"Salvato. (l'overlay non è in esecuzione)"), ui::col::Good);
  LogInfo("Impostazioni salvate ({} profili)", g.profiles.size());
  return ok;
}

// ------------------------------------------------------------------ file picker
std::filesystem::path PickFile(bool save, const std::wstring& suggested) {
  wchar_t buf[MAX_PATH] = {};
  wcsncpy_s(buf, suggested.c_str(), _TRUNCATE);
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = g.wnd;
  ofn.lpstrFilter = T(L"Profili PerfOverlay (*.json)\0*.json\0Tutti i file\0*.*\0");
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"json";
  ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
  return ok ? std::filesystem::path(buf) : std::filesystem::path();
}

void Export(const nlohmann::json& j, const std::wstring& suggested) {
  const auto path = PickFile(true, suggested);
  if (path.empty()) return;
  if (WriteJsonFile(path, j)) {
    Status(T(L"Esportato: ") + path.filename().wstring());
    LogInfo("Esportato {}", ToUtf8(path.wstring()));
  } else {
    MessageBoxW(g.wnd, T(L"Esportazione non riuscita."), kAppName, MB_ICONERROR);
  }
}

// Aggiunge un profilo importato chiedendo cosa fare in caso di conflitto. false = annullato.
bool MergeProfile(Profile p) {
  Sanitize(p);
  for (auto& existing : g.profiles) {
    if (!IEquals(existing.name, p.name)) continue;
    const std::wstring msg = T(L"Il profilo \"") + ToWide(p.name) +
                             T(L"\" esiste già.\n\nSì = sostituisci\nNo = importa con un nuovo nome\nAnnulla = salta");
    switch (MessageBoxW(g.wnd, msg.c_str(), T(L"Conflitto profilo"), MB_YESNOCANCEL | MB_ICONQUESTION)) {
      case IDYES:
        p.name = existing.name;
        existing = p;
        return true;
      case IDNO:
        if (IsDefault(p)) p.match.clear();
        p.name = UniqueName(IsDefault(p) ? std::string(TU("default importato")) : p.name);
        g.profiles.push_back(p);
        return true;
      default:
        return false;
    }
  }
  g.profiles.push_back(p);
  return true;
}

void Import(bool acceptGlobals) {
  const auto path = PickFile(false, L"");
  if (path.empty()) return;
  const auto j = ReadJsonFile(path);
  const auto bundle = j ? ParseImport(*j) : std::nullopt;
  if (!bundle) {
    MessageBoxW(g.wnd, T(L"File non riconosciuto come profilo o backup PerfOverlay."), kAppName, MB_ICONWARNING);
    return;
  }
  if (!UiToProfile()) return;
  int imported = 0;
  for (const auto& p : bundle->profiles) imported += MergeProfile(p) ? 1 : 0;

  bool globals = false;
  if ((bundle->config || bundle->games) && acceptGlobals &&
      MessageBoxW(g.wnd, T(L"Il file contiene anche impostazioni globali e lista giochi.\nSostituire quelle attuali?"),
                  kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
    if (bundle->config) g.cfg = *bundle->config;
    if (bundle->games) g.games = *bundle->games;
    globals = true;
  } else if ((bundle->config || bundle->games) && !acceptGlobals) {
    Status(T(L"Nota: il file contiene impostazioni globali, usa \"Importa tutto\" o \"Ripristina impostazioni\"."));
  }
  if (!imported && !globals) return;

  // Riordina (default in testa) e resta sul profilo selezionato.
  const std::string curName = g.cur >= 0 ? g.profiles[size_t(g.cur)].name : "default";
  std::stable_sort(g.profiles.begin(), g.profiles.end(), [](const Profile& a, const Profile& b) {
    return IsDefault(a) && !IsDefault(b);
  });
  g.cur = 0;
  for (size_t i = 0; i < g.profiles.size(); ++i)
    if (g.profiles[i].name == curName) g.cur = int(i);
  RefreshList();
  ProfileToUi();
  GlobalToUi();
  SaveAll();
  Status(TF(L"Importati {} profili{} da {}", imported, globals ? T(L" + impostazioni globali") : L"",
                     path.filename().wstring()));
  LogInfo("Import da {}: {} profili, globali={}", ToUtf8(path.wstring()), imported, globals);
}

// ------------------------------------------------------------------ preset RTSS
// Importa un layout dell'OverlayEditor di RivaTuner nel profilo selezionato.
// Un profilo senza giochi associati non si vede mai in gioco (vale "default"): propone di forzarlo
// per tutti i giochi, così le modifiche appena fatte compaiono subito.
void OfferUseEverywhere(const Profile& p) {
  if (IsDefault(p) || !p.match.empty() || IEquals(g.cfg.forcedProfile, p.name)) return;
  const std::wstring q = TF(
      L"Il profilo \"{}\" non è associato a nessun gioco, quindi in gioco si vede il profilo \"default\".\n\n"
      L"Vuoi usare \"{}\" per tutti i giochi?\n(Impostazioni globali > Profilo forzato)",
      ToWide(p.name), ToWide(p.name));
  if (MessageBoxW(g.wnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
    g.cfg.forcedProfile = p.name;
    RefreshForcedCombo();
  }
}

void OnImportRtss() {
  if (!UiToProfile() || !UiToGlobal()) return;
  wchar_t buf[MAX_PATH] = {};
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = g.wnd;
  ofn.lpstrFilter = T(L"Preset RTSS OverlayEditor (*.ovx;*.ovl)\0*.ovx;*.ovl\0Tutti i file\0*.*\0");
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) return;

  const auto r = ImportRtssPreset(buf);
  if (!r.ok) {
    MessageBoxW(g.wnd, (T(L"Preset non importato: ") + ToWide(r.error)).c_str(), kAppName, MB_ICONWARNING);
    return;
  }
  Profile& p = g.profiles[size_t(g.cur)];
  p.rtss = r.layout;
  p.layout = "rtss";
  ProfileToUi();
  OfferUseEverywhere(p);
  SaveAll();
  std::wstring msg = TF(L"Preset \"{}\" applicato al profilo \"{}\": {} layer.", ToWide(r.layout.name),
                                 ToWide(p.name), r.layout.layers.size());
  if (!r.unsupported.empty()) {
    msg += T(L" Dati non disponibili (mostrano N/A): ");
    for (size_t i = 0; i < r.unsupported.size(); ++i) msg += (i ? L", " : L"") + ToWide(r.unsupported[i]);
  }
  Status(msg, r.unsupported.empty() ? ui::col::Good : ui::col::Sub);
}

// ------------------------------------------------------------------ azioni
void OnSelectProfile() {
  const int sel = ListBox_GetCurSel(Item(IDC_LIST));
  if (sel < 0 || sel == g.cur) return;
  if (!UiToProfile()) {
    ListBox_SetCurSel(Item(IDC_LIST), g.cur);
    return;
  }
  g.cur = sel;
  RefreshList();
  ProfileToUi();
  // Il profilo selezionato è anche quello mostrato sul desktop: aggiorna subito l'overlay.
  if (g.cur >= 0 && size_t(g.cur) < g.profiles.size()) {
    GlobalConfig saved = LoadConfig();
    saved.desktopProfile = g.profiles[size_t(g.cur)].name;
    g.cfg.desktopProfile = saved.desktopProfile;
    if (SaveConfig(saved)) NotifyMonitorReload();
  }
}

void OnNewProfile() {
  if (!UiToProfile()) return;
  Profile p = g.profiles[size_t(g.cur)];  // parte dalle impostazioni del profilo corrente
  p.name = UniqueName(TU("Nuovo profilo"));
  p.match.clear();
  g.profiles.push_back(p);
  g.cur = int(g.profiles.size()) - 1;
  RefreshList();
  ProfileToUi();
  SetFocus(Item(IDC_NAME));
  Edit_SetSel(Item(IDC_NAME), 0, -1);
}

void OnDeleteProfile() {
  if (g.cur <= 0 || IsDefault(g.profiles[size_t(g.cur)])) return;
  const auto& p = g.profiles[size_t(g.cur)];
  if (MessageBoxW(g.wnd, (T(L"Eliminare il profilo \"") + ToWide(p.name) + L"\"?").c_str(), kAppName,
                  MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  g.removed.push_back(p.name);
  if (IEquals(g.cfg.forcedProfile, p.name)) g.cfg.forcedProfile.clear();
  g.profiles.erase(g.profiles.begin() + g.cur);
  g.cur = 0;
  RefreshList();
  ProfileToUi();
  Status(T(L"Profilo eliminato (premi Salva per confermare)."));
}

void OnPickColor(int i) {
  if (g.cur < 0) return;
  Color& c = ColorRef(g.profiles[size_t(g.cur)], i);
  CHOOSECOLORW cc{sizeof(cc)};
  cc.hwndOwner = g.wnd;
  cc.rgbResult = RGB(c.r, c.g, c.b);
  cc.lpCustColors = g.custom;
  cc.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (!ChooseColorW(&cc)) return;
  c.r = GetRValue(cc.rgbResult);
  c.g = GetGValue(cc.rgbResult);
  c.b = GetBValue(cc.rgbResult);
  UpdateSwatch(i);
}

void OnSave() {
  if (!UiToProfile() || !UiToGlobal()) return;
  RefreshList();
  SaveAll();
}

// ------------------------------------------------------------------ cattura hotkey
// Campo hotkey in sola lettura: con "Cambia" (o un clic sul campo) si preme la combinazione.
// Esc annulla, Canc/Backspace da soli tolgono la hotkey.
constexpr int kHotkeyIds[] = {IDC_HK_TOGGLE, IDC_HK_CYCLE, IDC_HK_FPSONLY};
const wchar_t* const kHotkeyNames[] = {T(L"Mostra / nascondi"), T(L"Cambia profilo"), T(L"Solo FPS")};
int g_capturing = 0;          // id del campo in acquisizione, 0 = nessuno
std::wstring g_captureBefore;  // valore da ripristinare se si annulla

int HotkeyIndex(int editId) {
  for (int i = 0; i < 3; ++i)
    if (kHotkeyIds[i] == editId) return i;
  return -1;
}

void InvalidateFrame(int id) {
  for (const auto& f : g_frames)
    if (f.id == id) {
      RECT r = SR(f.r);
      InvalidateRect(g.wnd, &r, FALSE);
    }
}

// Nome del tasto nello stesso formato letto da ParseHotkey; vuoto se non supportato.
std::wstring KeyName(UINT vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, wchar_t(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return std::format(L"F{}", vk - VK_F1 + 1);
  switch (vk) {
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_PAUSE: return L"Pause";
    case VK_SCROLL: return L"Scroll";
  }
  return {};
}

std::wstring ModifierPrefix() {
  std::wstring s;
  if (GetKeyState(VK_CONTROL) < 0) s += L"Ctrl+";
  if (GetKeyState(VK_SHIFT) < 0) s += L"Shift+";
  if (GetKeyState(VK_MENU) < 0) s += L"Alt+";
  if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) s += L"Win+";
  return s;
}

void EndCapture(bool commit, const std::wstring& value = {}) {
  if (!g_capturing) return;
  const int id = g_capturing;
  g_capturing = 0;
  SetText(id, commit ? value : g_captureBefore);
  SetWindowTextW(Item(id + (IDC_HK_BTN - IDC_HK_TOGGLE)), T(L"Cambia"));
  InvalidateRect(Item(id + (IDC_HK_BTN - IDC_HK_TOGGLE)), nullptr, FALSE);
  NotifyMonitorSuspendHotkeys(false);
  InvalidateFrame(id);
}

void StartCapture(int id) {
  if (g_capturing == id) return;
  EndCapture(false);
  NotifyMonitorSuspendHotkeys(true);  // altrimenti PerfOverlay.exe "ruberebbe" le sue combinazioni
  g_capturing = id;
  g_captureBefore = GetText(id);
  SetText(id, T(L"Premi i tasti..."));
  SetWindowTextW(Item(id + (IDC_HK_BTN - IDC_HK_TOGGLE)), T(L"Annulla"));
  InvalidateRect(Item(id + (IDC_HK_BTN - IDC_HK_TOGGLE)), nullptr, FALSE);
  SetFocus(Item(id));
  InvalidateFrame(id);
  Status(T(L"Premi la nuova combinazione, es. Ctrl+Shift+O.  Esc = annulla, Canc = nessuna hotkey."),
         ui::col::Accent);
}

// Valida e applica la combinazione; false se va rifiutata (resta in acquisizione).
bool AcceptCombo(const std::wstring& combo) {
  const std::string text = ToUtf8(combo);
  unsigned mods = 0, vk = 0;
  if (!ParseHotkey(text, mods, vk)) return false;
  for (int other : kHotkeyIds) {
    if (other == g_capturing) continue;
    const std::string cur = GetUtf8(other);
    if (!cur.empty() && IEquals(cur, text)) {
      Status(combo + T(L" è già usata per \"") + kHotkeyNames[HotkeyIndex(other)] + T(L"\". Premi un'altra combinazione."),
             ui::col::Bad);
      return false;
    }
  }
  // Prova a registrarla: se fallisce, la usa già un altro programma.
  constexpr int kProbeId = 0xBFFF;
  if (!RegisterHotKey(g.wnd, kProbeId, mods | MOD_NOREPEAT, vk)) {
    Status(combo + T(L" è già usata da un altro programma. Premi un'altra combinazione."), ui::col::Bad);
    return false;
  }
  UnregisterHotKey(g.wnd, kProbeId);
  const std::wstring name = kHotkeyNames[HotkeyIndex(g_capturing)];
  EndCapture(true, combo);
  Status(T(L"Hotkey \"") + name + L"\" = " + combo + T(L".  Premi \"Salva e applica\" per attivarla."), ui::col::Good);
  return true;
}

LRESULT CALLBACK HotkeyEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  const int id = GetDlgCtrlID(h);
  const bool capturing = g_capturing == id;
  switch (msg) {
    case WM_LBUTTONDOWN:
      StartCapture(id);
      return 0;
    case WM_GETDLGCODE:
      if (capturing) return DLGC_WANTALLKEYS;  // anche Tab, Invio ed Esc arrivano qui
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (!capturing) {
        if (wp == VK_RETURN || wp == VK_SPACE) StartCapture(id);
        break;
      }
      const UINT vk = UINT(wp);
      const std::wstring mods = ModifierPrefix();
      if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        SetText(id, mods + L"...");  // anteprima mentre si tengono premuti i modificatori
        return 0;
      }
      if (vk == VK_ESCAPE && mods.empty()) {
        EndCapture(false);
        Status(T(L"Modifica hotkey annullata."));
        return 0;
      }
      if ((vk == VK_BACK || vk == VK_DELETE) && mods.empty()) {
        EndCapture(true, L"");
        Status(T(L"Hotkey rimossa.  Premi \"Salva e applica\" per confermare."), ui::col::Good);
        return 0;
      }
      const std::wstring key = KeyName(vk);
      if (key.empty()) {
        Status(T(L"Tasto non supportato: usa lettere, numeri, F1-F24, Home, End, Ins, Canc, PagSu, PagGiù."),
               ui::col::Bad);
        return 0;
      }
      const bool standalone = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL;
      if (mods.empty() && !standalone) {
        Status(T(L"Aggiungi almeno Ctrl, Shift o Alt: da sola \"") + key + T(L"\" scatterebbe mentre scrivi o giochi."),
               ui::col::Bad);
        return 0;
      }
      AcceptCombo(mods + key);
      return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (capturing) {
        const std::wstring mods = ModifierPrefix();
        SetText(id, mods.empty() ? T(L"Premi i tasti...") : mods + L"...");
        return 0;
      }
      break;
    case WM_CHAR:
    case WM_SYSCHAR:
      if (capturing) return 0;
      break;
    case WM_KILLFOCUS:
      if (capturing) {
        // Il clic su "Annulla" toglie il focus: lo gestisce il pulsante.
        const HWND next = reinterpret_cast<HWND>(wp);
        if (next != Item(id + (IDC_HK_BTN - IDC_HK_TOGGLE))) EndCapture(false);
      }
      break;
    case WM_SETFOCUS: {
      const LRESULT r = DefSubclassProc(h, msg, wp, lp);
      HideCaret(h);  // campo in sola lettura: niente cursore lampeggiante
      return r;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, HotkeyEditProc, 0);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

void HotkeyField(int id, int x, int y) {
  Edit(id, x, y, 106);
  HWND e = Item(id);
  SendMessageW(e, EM_SETREADONLY, TRUE, 0);
  SetWindowSubclass(e, HotkeyEditProc, 0, 0);
  Button(id + (IDC_HK_BTN - IDC_HK_TOGGLE), T(L"Cambia"), x + 112, y, 58, kRowH, ui::col::Panel);
}

// ------------------------------------------------------------------ costruzione UI
void BuildUi() {
  // Colonna 1: profili
  Panel(T(L"PROFILI"), 20, 104, 240, 240);
  HWND list = Make(L"LISTBOX", L"",
                   WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                   28, 112, 224, 224, IDC_LIST);
  ApplyDarkControlTheme(list, L"DarkMode_Explorer");
  const std::pair<int, const wchar_t*> btns[] = {
      {IDC_NEW, T(L"Nuovo")},                    {IDC_DELETE, T(L"Elimina")},
      {IDC_EXPORT_ONE, T(L"Esporta profilo")},   {IDC_IMPORT_ONE, T(L"Importa profilo")},
      {IDC_EXPORT_ALL, T(L"Esporta tutto")},     {IDC_IMPORT_ALL, T(L"Importa tutto")},
      {IDC_BACKUP_CFG, T(L"Backup config")},     {IDC_RESTORE_CFG, T(L"Ripristina config")}};
  for (size_t i = 0; i < std::size(btns); ++i)
    Button(btns[i].first, btns[i].second, 20 + int(i % 2) * 125, 356 + int(i / 2) * 36, 115);
  Button(IDC_IMPORT_RTSS, T(L"Importa RTSS..."), 20, 500, 115);
  Button(IDC_STORE, T(L"Store overlay..."), 145, 500, 115, 30, ui::col::Bg, true);

  // Colonna 2: profilo selezionato
  Panel(T(L"PROFILO SELEZIONATO"), 280, 104, 400, 440);
  constexpr int lx = 296, lw = 130, cx = 430, cw = 234;
  auto row = [](int i) { return 118 + i * 34; };
  Label(T(L"Nome"), lx, row(0), lw);
  Edit(IDC_NAME, cx, row(0), cw);
  Label(T(L"Giochi"), lx, row(1), lw);
  Edit(IDC_MATCH, cx, row(1), cw, T(L"gioco.exe; altro.exe|Titolo"));
  Label(L"Layout", lx, row(2), lw);
  Combo(IDC_LAYOUT, cx, row(2), 150, kLayoutNames);
  Button(IDC_EDITOR, L"Editor...", cx + 156, row(2), cw - 156, kRowH, ui::col::Panel);
  Label(T(L"Posizione"), lx, row(3), lw);
  Combo(IDC_POSITION, cx, row(3), cw, kPositionNames);
  Label(T(L"X / Y (px a 1080p)"), lx, row(4), lw);
  Edit(IDC_X, cx, row(4), 112, L"X");
  Edit(IDC_Y, cx + 122, row(4), 112, L"Y");
  Label(L"Font", lx, row(5), lw);
  static const wchar_t* const kFonts[] = {L"sans", L"mono", L"Segoe UI", L"Consolas", L"Bahnschrift"};
  Combo(IDC_FONT, cx, row(5), cw, kFonts, true);
  Label(T(L"Dimensione (1080p)"), lx, row(6), lw);
  Edit(IDC_FONTSIZE, cx, row(6), 70);
  Check(IDC_BOLD, T(L"Grassetto"), cx + 84, row(6) + 2, 150);
  Label(T(L"Opacità sfondo %"), lx, row(7), lw);
  Edit(IDC_OPACITY, cx, row(7), 70);
  Check(IDC_AUTOSCALE, T(L"Scala con risoluzione"), cx + 84, row(7) + 2, 160);
  Label(T(L"Margine bordo (px)"), lx, row(8), lw);
  Edit(IDC_MARGIN, cx, row(8), 70);
  Label(T(L"FPS rosso sotto"), lx, row(9), lw);
  Edit(IDC_FPSRED, cx, row(9), 70);
  Label(T(L"FPS giallo sotto"), lx, row(10), lw);
  Edit(IDC_FPSYELLOW, cx, row(10), 70);
  Button(IDC_SENSORS, T(L"Sensori di sistema (temperature, ventole, dischi...)"), lx, row(11) + 6, cx + cw - lx,
         32, ui::col::Panel);

  // Colonna 3: elementi visibili e colori
  Panel(T(L"ELEMENTI VISIBILI"), 700, 104, 280, 248);
  for (int i = 0; i < kShowCount; ++i) Check(IDC_SHOW + i, kShowNames[i], 716, 116 + i * 26, 250);
  Panel(T(L"COLORI"), 700, 390, 280, 154);
  for (int i = 0; i < kColorCount; ++i)
    Button(IDC_COLOR_BTN + i, kColorNames[i], 712 + (i % 2) * 134, 402 + (i / 2) * 34, 122, 28, ui::col::Panel);

  // Riga in basso: impostazioni globali
  Panel(T(L"IMPOSTAZIONI GLOBALI"), 20, 584, 960, 156);
  auto grow = [](int i) { return 598 + i * 34; };
  Label(L"Refresh (ms)", 36, grow(0), 120);
  Edit(IDC_REFRESH, 160, grow(0), 80);
  Label(T(L"Fuori dal gioco"), 36, grow(1), 120);
  Combo(IDC_OUTOFGAME, 160, grow(1), 156, kOutOfGameNames);
  Label(T(L"Profilo forzato"), 36, grow(2), 120);
  Make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 160, grow(2) + 1, 156, 240, IDC_FORCED);
  ApplyDarkControlTheme(Item(IDC_FORCED), L"DarkMode_CFD");
  SendMessageW(Item(IDC_FORCED), CB_SETITEMHEIGHT, WPARAM(-1), S(kRowH - 8));
  SendMessageW(Item(IDC_FORCED), CB_SETITEMHEIGHT, 0, S(22));
  Label(L"Lingua / Language", 36, grow(3), 120);
  Combo(IDC_LANG, 160, grow(3), 156, kLangNames);
  Label(T(L"Mostra / nascondi"), 344, grow(0), 136);
  HotkeyField(IDC_HK_TOGGLE, 484, grow(0));
  Label(T(L"Cambia profilo"), 344, grow(1), 136);
  HotkeyField(IDC_HK_CYCLE, 484, grow(1));
  Label(T(L"Solo FPS"), 344, grow(2), 136);
  HotkeyField(IDC_HK_FPSONLY, 484, grow(2));
  Check(IDC_FPSONLY, T(L"Modalità solo FPS"), 664, grow(0) + 2, 300);
  Check(IDC_AUTOLEARN, T(L"Rileva nuovi giochi automaticamente"), 664, grow(1) + 2, 300);
  Check(IDC_AUTOSTART, T(L"Avvia PerfOverlay Supreme con Windows"), 664, grow(2) + 2, 300);

  // Barra inferiore
  Button(IDC_EDIT_GAMES, T(L"Lista giochi..."), 20, 756, 176, 32);
  Button(IDC_OPEN_FOLDER, T(L"Apri cartella dati"), 204, 756, 168, 32);
  Button(IDC_SAVE, T(L"Salva e applica"), 830, 756, 150, 32, ui::col::Bg, true);

  // Intestazione: avvio dell'overlay se non è in esecuzione
  Button(IDC_LAUNCH, T(L"Avvia overlay"), 850, 26, 130, 30);
}

// ------------------------------------------------------------------ disegno
void DrawTextAt(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextW(dc, t.c_str(), int(t.size()), &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

void Paint(HDC dc, const RECT& client) {
  FillRect(dc, &client, g.brBg);
  SetBkMode(dc, TRANSPARENT);

  // Intestazione (come Decky Manager e SteamImporter): icona, titolo d'accento, sottotitolo.
  DrawIconEx(dc, S(20), S(18), g.icon, S(44), S(44), 0, nullptr, DI_NORMAL);
  DrawTextAt(dc, kAppTitleUpper, SR({76, 14, 600, 46}), g.fontTitle, ui::col::Accent, DT_LEFT | DT_TOP);
  DrawTextAt(dc, T(L"Overlay prestazioni per i giochi  ·  profili per gioco  ·  export / import"),
             SR({77, 46, 700, 68}), g.font, ui::col::Sub, DT_LEFT | DT_TOP);
  const int right = g.monitorRunning ? 980 : 838;
  DrawTextAt(dc, g.monitorRunning ? T(L"●  Overlay in esecuzione") : T(L"●  Overlay non avviato"), SR({600, 26, right, 56}),
             g.fontBold, g.monitorRunning ? ui::col::Good : ui::col::Bad, DT_RIGHT | DT_VCENTER);

  for (const auto& p : g_panels) {
    const RECT r = SR(p.r);
    DrawTextAt(dc, p.title, {r.left + S(4), r.top - S(22), r.right, r.top - S(4)}, g.fontSection, ui::col::Sub,
               DT_LEFT | DT_BOTTOM);
    ui::FillRound(dc, r, S(8), ui::col::Panel, ui::col::Line);
  }
  for (const auto& l : g_labels) DrawTextAt(dc, l.text, SR(l.r), g.font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  for (const auto& f : g_frames) {
    const bool on = IsWindowEnabled(Item(f.id));
    const COLORREF border = g.focusedEdit == f.id || g_capturing == f.id ? ui::col::Accent : ui::col::Line;
    ui::FillRound(dc, SR(f.r), S(6), on ? ui::col::Input : kInputOff, on ? border : RGB(44, 44, 52));
  }
  DrawTextAt(dc, g.status, StatusRect(), g.font, g.statusColor, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

void DrawProfileItem(const DRAWITEMSTRUCT* d) {
  if (d->itemID == UINT(-1)) return;
  HDC dc = d->hDC;
  const RECT& r = d->rcItem;
  FillRect(dc, &r, g.brPanel);
  const bool sel = d->itemState & ODS_SELECTED;
  if (sel) {
    RECT b = r;
    InflateRect(&b, -S(2), -S(2));
    ui::FillRound(dc, b, S(6), ui::Blend(ui::col::Panel, ui::col::Accent, 0.20f),
                  ui::Blend(ui::col::Panel, ui::col::Accent, 0.55f));
  }
  wchar_t buf[256];
  SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, reinterpret_cast<LPARAM>(buf));
  const std::wstring text = buf;
  const auto tab = text.find(L'\t');
  SetBkMode(dc, TRANSPARENT);
  RECT tr{r.left + S(12), r.top, r.right - S(12), r.bottom};
  if (tab != std::wstring::npos)
    DrawTextAt(dc, text.substr(tab + 1), tr, g.font, sel ? ui::col::AccentHover : ui::col::Sub,
               DT_RIGHT | DT_VCENTER);
  tr.right -= S(78);
  DrawTextAt(dc, text.substr(0, tab), tr, sel ? g.fontBold : g.font, ui::col::Text,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

void UpdateMonitorState() {
  const bool running = FindWindowW(kMonitorWndClass, nullptr) != nullptr;
  if (running == g.monitorRunning && IsWindowVisible(Item(IDC_LAUNCH)) == !running) return;
  g.monitorRunning = running;
  ShowWindow(Item(IDC_LAUNCH), running ? SW_HIDE : SW_SHOW);
  const RECT r = SR({560, 0, kClientW, 80});
  InvalidateRect(g.wnd, &r, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      Paint(mem, rc);
      BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
             ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_TIMER:
      if (wp == kTimerMonitor) UpdateMonitorState();
      return 0;
    case WM_MEASUREITEM: {
      auto* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
      if (m->CtlType == ODT_LISTBOX) m->itemHeight = UINT(S(32));
      return TRUE;
    }
    case WM_DRAWITEM: {
      auto* d = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
      if (d->CtlType == ODT_LISTBOX) {
        DrawProfileItem(d);
      } else if (d->CtlType == ODT_BUTTON) {
        const int ci = int(d->CtlID) - IDC_COLOR_BTN;
        if (ci >= 0 && ci < kColorCount && g.cur >= 0) {
          const Color c = ColorRef(g.profiles[size_t(g.cur)], ci);
          const COLORREF sw = RGB(c.r, c.g, c.b);
          ui::DrawButton(d, &sw);
        } else {
          ui::DrawButton(d);
        }
      }
      return TRUE;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, ui::col::Text);
      SetBkColor(dc, ui::col::Input);
      return reinterpret_cast<LRESULT>(g.brInput);
    }
    case WM_CTLCOLORSTATIC: {  // EDIT disattivati o in sola lettura (hotkey)
      HDC dc = reinterpret_cast<HDC>(wp);
      const HWND ctl = reinterpret_cast<HWND>(lp);
      if (IsWindowEnabled(ctl)) {
        SetTextColor(dc, g_capturing == GetDlgCtrlID(ctl) ? ui::col::AccentHover : ui::col::Text);
        SetBkColor(dc, ui::col::Input);
        return reinterpret_cast<LRESULT>(g.brInput);
      }
      SetTextColor(dc, ui::col::Disabled);
      SetBkColor(dc, kInputOff);
      return reinterpret_cast<LRESULT>(g.brInputOff);
    }
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, ui::col::Text);
      if (reinterpret_cast<HWND>(lp) == Item(IDC_LIST)) {
        SetBkColor(dc, ui::col::Panel);
        return reinterpret_cast<LRESULT>(g.brPanel);
      }
      SetBkColor(dc, ui::col::Input);  // tendina delle combobox
      return reinterpret_cast<LRESULT>(g.brInput);
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      const bool isEdit = std::any_of(g_frames.begin(), g_frames.end(), [id](const FrameItem& f) { return f.id == id; });
      if (isEdit && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        g.focusedEdit = code == EN_SETFOCUS ? id : 0;
        for (const auto& f : g_frames)
          if (f.id == id) {
            RECT r = SR(f.r);
            InvalidateRect(hwnd, &r, FALSE);
          }
        return 0;
      }
      if (id == IDC_LIST && code == LBN_SELCHANGE) {
        OnSelectProfile();
      } else if (id == IDC_LANG && code == CBN_SELCHANGE) {
        OnChangeLanguage();
      } else if (id == IDC_POSITION && code == CBN_SELCHANGE) {
        const bool custom = GetSel(IDC_POSITION) == 4;
        EnableField(IDC_X, custom);
        EnableField(IDC_Y, custom);
      } else if (code == BN_CLICKED && id >= IDC_HK_BTN && id < IDC_HK_BTN + 3) {
        const int edit = kHotkeyIds[id - IDC_HK_BTN];
        if (g_capturing == edit) {
          EndCapture(false);
          Status(T(L"Modifica hotkey annullata."));
        } else {
          StartCapture(edit);
        }
      } else if (code == BN_CLICKED) {
        if (id >= IDC_COLOR_BTN && id < IDC_COLOR_BTN + kColorCount) OnPickColor(id - IDC_COLOR_BTN);
        switch (id) {
          case IDC_IMPORT_RTSS: OnImportRtss(); break;
          case IDC_EDITOR:
            if (UiToProfile() && UiToGlobal()) {
              Profile& p = g.profiles[size_t(g.cur)];
              if (p.layout == "rtss" && p.rtss.advanced) {
                MessageBoxW(g.wnd,
                            T(L"Questo è un preset RTSS avanzato (immagini, grafici e formule, es. TroyMetrics): "
                            L"viene mostrato così com'è stato disegnato nell'OverlayEditor di RTSS.\n\n"
                            L"Qui puoi cambiarne posizione (Posizione, X / Y) e grandezza (Dimensione)."),
                            kAppName, MB_ICONINFORMATION);
                break;
              }
              if (RunLayoutEditor(g.wnd, g.inst, g.font, p)) {  // imposta da solo il layout (libero o RTSS)
                ProfileToUi();
                OfferUseEverywhere(p);
                SaveAll();
                Status(TF(L"{} di \"{}\" salvato e applicato.",
                                   p.layout == "rtss" ? L"Preset RTSS" : T(L"Layout libero"), ToWide(p.name)),
                       ui::col::Good);
              }
            }
            break;
          case IDC_STORE:
            if (UiToProfile() && UiToGlobal()) {
              Profile& p = g.profiles[size_t(g.cur)];
              if (RunGallery(g.wnd, g.inst, g.font, p)) {
                ProfileToUi();
                OfferUseEverywhere(p);
                SaveAll();
                Status(T(L"Overlay applicato al profilo \"") + ToWide(p.name) + T(L"\" e salvato."), ui::col::Good);
              }
            }
            break;
          case IDC_SENSORS:
            if (UiToProfile() && UiToGlobal()) {
              Profile& p = g.profiles[size_t(g.cur)];
              if (RunSensorBrowser(g.wnd, g.inst, g.font, p)) {
                Sanitize(p);
                OfferUseEverywhere(p);
                SaveAll();
                Status(TF(L"{} {} nell'overlay di \"{}\": salvato e applicato.", p.sensors.size(),
                                   p.sensors.size() == 1 ? T(L"sensore") : T(L"sensori"), ToWide(p.name)),
                       ui::col::Good);
              }
            }
            break;
          case IDC_NEW: OnNewProfile(); break;
          case IDC_DELETE: OnDeleteProfile(); break;
          case IDC_EXPORT_ONE:
            if (UiToProfile()) Export(ExportProfile(g.profiles[size_t(g.cur)]), ToWide(g.profiles[size_t(g.cur)].name));
            break;
          case IDC_IMPORT_ONE: Import(false); break;
          case IDC_EXPORT_ALL:
            if (UiToProfile() && UiToGlobal()) Export(ExportAll(g.cfg, g.games, g.profiles), L"PerfOverlaySupreme-backup");
            break;
          case IDC_IMPORT_ALL: Import(true); break;
          case IDC_BACKUP_CFG:
            if (UiToGlobal()) Export(ExportConfig(g.cfg, g.games), L"PerfOverlaySupreme-config");
            break;
          case IDC_RESTORE_CFG: Import(true); break;
          case IDC_EDIT_GAMES:
            g.games = LoadGames();  // include quelli appresi dall'overlay nel frattempo
            if (RunGamesEditor(hwnd, g.inst, g.font, g.games)) {
              const bool saved = SaveGames(g.games);
              NotifyMonitorReload();
              Status(saved ? TF(L"Lista giochi salvata: {} giochi, {} esclusi.",
                                         g.games.known.size() + g.games.learned.size(), g.games.exclude.size())
                           : std::wstring(T(L"Errore nel salvataggio di games.json")),
                     saved ? ui::col::Good : ui::col::Bad);
            }
            break;
          case IDC_OPEN_FOLDER:
            ShellExecuteW(hwnd, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
          case IDC_LAUNCH:
            ShellExecuteW(hwnd, L"open", (ExeDir() / kMonitorExe).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            SetTimer(hwnd, kTimerMonitor, 500, nullptr);  // controllo rapido subito dopo l'avvio
            break;
          case IDC_SAVE:
            EndCapture(false);
            g.games = LoadGames();  // include eventuali modifiche manuali a games.json
            OnSave();
            break;
        }
      }
      return 0;
    }
    case WM_DESTROY:
      EndCapture(false);  // riattiva le hotkey del monitor se si chiude durante la cattura
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HFONT MakeFont(int tenthsOfPoint, int weight) {
  return CreateFontW(-MulDiv(tenthsOfPoint, g.dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

int RunSettingsWindow(HINSTANCE inst, int show) {
  g.inst = inst;
  g.dpi = int(GetDpiForSystem());
  g.cfg = LoadConfig();
  g.games = LoadGames();
  g.profiles = LoadProfiles();
  g.cur = 0;

  EnableDarkModeForApp();
  ui::Init(inst);
  g.font = MakeFont(95, FW_NORMAL);
  g.fontBold = MakeFont(95, FW_SEMIBOLD);
  g.fontTitle = MakeFont(160, FW_BOLD);
  g.fontSection = MakeFont(80, FW_BOLD);
  g.brBg = CreateSolidBrush(ui::col::Bg);
  g.brPanel = CreateSolidBrush(ui::col::Panel);
  g.brInput = CreateSolidBrush(ui::col::Input);
  g.brInputOff = CreateSolidBrush(kInputOff);
  g.icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(44), S(44), 0));

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  wc.hbrBackground = g.brBg;
  wc.lpszClassName = L"PerfOverlaySupremeSettings";
  RegisterClassExW(&wc);

  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  RECT r{0, 0, S(kClientW), S(kClientH)};
  AdjustWindowRect(&r, style, FALSE);
  g.wnd = CreateWindowExW(0, wc.lpszClassName, T(L"PerfOverlay Supreme - Impostazioni"), style, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!g.wnd) return 1;
  ApplyDarkTitleBar(g.wnd, ui::col::Bg);

  BuildUi();
  RefreshList();
  ProfileToUi();
  GlobalToUi();
  UpdateMonitorState();
  SetTimer(g.wnd, kTimerMonitor, 1500, nullptr);
  Status(T(L"Il profilo \"default\" vale per tutti i giochi senza un profilo dedicato."));
  ShowWindow(g.wnd, show);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(g.wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (HGDIOBJ o : std::initializer_list<HGDIOBJ>{g.font, g.fontBold, g.fontTitle, g.fontSection, g.brBg, g.brPanel,
                                                  g.brInput, g.brInputOff})
    DeleteObject(o);
  if (g.icon) DestroyIcon(g.icon);
  ui::Shutdown();
  return 0;
}

}  // namespace po
