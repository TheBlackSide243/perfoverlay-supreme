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
#include <memory>
#include <optional>
#include <thread>

#include "common/config.h"
#include "common/branding.h"
#include "common/darkmode.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/resources.h"
#include "common/rtss_preset.h"
#include "common/util.h"
#include "common/winutil.h"
#include "settings/color_wheel.h"
#include "settings/gallery.h"
#include "settings/games_editor.h"
#include "settings/layout_editor.h"
#include "settings/overlay_preview.h"
#include "settings/sensor_browser.h"
#include "settings/theme.h"
#include "settings/updater.h"
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
  IDC_FSHINT,
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
  IDC_UPDATE,
  IDC_CHECKUPDATES,
  IDC_ACCENT_RESET,
  IDC_ACCENT_HEX,
  IDC_ACCENT_WHEEL,
  IDC_ACCENT_TO_OVERLAY,
  IDC_PREVIEW,       // anteprima dell'overlay (Aspetto overlay)
  IDC_DEMO_PRIMARY,  // controlli d'esempio della sezione Tema: non fanno niente
  IDC_DEMO_BUTTON,
  IDC_DEMO_CHECK,
  IDC_DEMO_EDIT,
  IDC_ACCENT_BASE = 1400,  // + indice colore predefinito dell'interfaccia
  IDC_NAV_BASE = 1500,     // + indice pagina della barra laterale
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
// Aggiornamenti: 0 = installati dall'overlay da soli, 1 = solo il pulsante nelle impostazioni, 2 = nessun controllo.
const wchar_t* const kUpdateNames[] = {T(L"Automatici"), T(L"Solo avviso"), T(L"Disattivati")};
// Colori d'accento pronti per l'interfaccia (il primo è il predefinito); la ruota colori fa il resto.
struct AccentPreset {
  const wchar_t* name;
  COLORREF color;
};
const AccentPreset kAccentPresets[] = {{T(L"Blu"), ui::col::DefaultAccent}, {T(L"Ciano"), RGB(34, 211, 238)},
                                       {T(L"Verde"), RGB(16, 217, 138)},    {T(L"Rosso"), RGB(255, 90, 106)},
                                       {T(L"Ambra"), RGB(250, 204, 21)},    {T(L"Rosa"), RGB(244, 114, 182)}};
constexpr int kAccentCount = int(std::size(kAccentPresets));

// Pagine della barra laterale. Le icone sono glifi di Segoe Fluent Icons / Segoe MDL2 Assets (font di Windows).
struct PageInfo {
  const wchar_t* name;
  const wchar_t* glyph;
  const wchar_t* subtitle;  // nullptr = descrizione legata al profilo selezionato
};
const PageInfo kPages[] = {
    {T(L"Profili"), L"", T(L"Un profilo per gioco; \"default\" vale per tutti gli altri.")},
    {T(L"Aspetto overlay"), L"", nullptr},
    {T(L"Elementi e sensori"), L"", nullptr},
    {T(L"Hotkey"), L"", T(L"Scorciatoie da tastiera, attive anche durante il gioco.")},
    {T(L"Generali"), L"", T(L"Comportamento dell'overlay, lingua, avvio e aggiornamenti.")},
    {T(L"Tema"), L"", T(L"Colore d'accento di questa finestra.")}};
constexpr int kPageCount = int(std::size(kPages));
constexpr int kSideW = 224;  // larghezza della barra laterale (a 96 DPI)
constexpr COLORREF kSidebar = RGB(20, 20, 23);

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
  std::optional<UpdateInfo> update;  // versione nuova trovata su GitHub
  HWND wnd = nullptr;
  HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSection = nullptr;
  HFONT fontDlg = nullptr;  // per le finestre secondarie, che hanno dimensioni fisse ai DPI di sistema
  HFONT fontIcon = nullptr;  // glifi delle icone della barra laterale
  HFONT fontIconBig = nullptr;  // icona della sezione nell'intestazione
  HBRUSH brBg = nullptr, brPanel = nullptr, brInput = nullptr, brInputOff = nullptr, brSide = nullptr;
  HICON icon = nullptr;
  int dpi = 96;      // scala effettiva dell'interfaccia (DPI del monitor × ingrandimento della finestra)
  int baseDpi = 96;  // DPI del monitor su cui si trova la finestra
  int ox = 0, oy = 0;  // margine per centrare il contenuto quando la finestra ha proporzioni diverse
  std::vector<std::pair<HWND, RECT>> layout;  // controlli e posizione a 96 DPI, per ridisporli
  std::vector<int> layoutPage;                // pagina di ogni controllo di layout (-1 = sempre visibile)
  int page = 0;                               // pagina mostrata
  bool syncingAccent = false;                 // aggiornamento di ruota/codice in corso (niente rimbalzi)
  bool previewQueued = false;                 // kMsgRefreshPreview già in coda
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
constexpr int kClientW = 1000, kClientH = 640;
constexpr int kRowH = 28;
constexpr UINT_PTR kTimerMonitor = 1;
constexpr UINT kMsgUpdateFound = WM_APP + 20;     // lParam = UpdateInfo* (dal thread di controllo)
constexpr UINT kMsgUpdateDownloaded = WM_APP + 21;  // wParam = riuscito, lParam = std::wstring* errore
constexpr UINT kMsgRefreshPreview = WM_APP + 22;    // ridisegna l'anteprima dell'overlay (una volta per raffica)

// Elementi disegnati direttamente sulla finestra (coordinate a 96 DPI).
struct PanelItem {
  RECT r;
  const wchar_t* title;
  int page;
};
struct LabelItem {
  RECT r;
  const wchar_t* text;
  int page;
};
struct FrameItem {
  int id;
  RECT r;
  int page;
};
int g_buildPage = -1;  // pagina a cui appartengono i controlli creati in BuildUi
std::vector<PanelItem> g_panels;
std::vector<LabelItem> g_labels;
std::vector<FrameItem> g_frames;

int S(int v) { return MulDiv(v, g.dpi, 96); }
RECT SR(const RECT& r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }
// Rettangolo di disegno → coordinate della finestra (il disegno è spostato di ox/oy per centrare).
RECT ToClient(RECT r) {
  OffsetRect(&r, g.ox, g.oy);
  return r;
}
HWND Item(int id) { return GetDlgItem(g.wnd, id); }

// ------------------------------------------------------------------ helper controlli
HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, g.ox + S(x), g.oy + S(y), S(w), S(h), g.wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
  g.layout.push_back({c, {x, y, x + w, y + h}});
  g.layoutPage.push_back(g_buildPage);
  return c;
}
void Panel(const wchar_t* title, int x, int y, int w, int h) {
  g_panels.push_back({{x, y, x + w, y + h}, title, g_buildPage});
}
void Label(const wchar_t* t, int x, int y, int w) { g_labels.push_back({{x, y, x + w, y + kRowH}, t, g_buildPage}); }
// Campo di testo senza bordo: la cornice arrotondata la disegna la finestra (g_frames).
void Edit(int id, int x, int y, int w, const wchar_t* cue = nullptr) {
  g_frames.push_back({id, {x, y, x + w, y + kRowH}, g_buildPage});
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
  ui::MakeCombo(c, ui::col::Panel);
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
RECT StatusRect() { return SR({248, 594, 812, 626}); }
void Status(const std::wstring& s, COLORREF color = ui::col::Sub) {
  g.status = s;
  g.statusColor = color;
  const RECT r = ToClient(StatusRect());
  InvalidateRect(g.wnd, &r, FALSE);
}

// Abilita/disabilita un campo e ridisegna la sua cornice.
void EnableField(int id, bool on) {
  EnableWindow(Item(id), on);
  for (const auto& f : g_frames)
    if (f.id == id) {
      RECT r = ToClient(SR(f.r));
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

// L'anteprima dell'overlay si aggiorna dopo che i controlli hanno finito di cambiare (una volta sola
// anche se cambiano in tanti, come quando si seleziona un altro profilo).
void QueuePreview() {
  if (g.previewQueued || !g.wnd) return;
  g.previewQueued = PostMessageW(g.wnd, kMsgRefreshPreview, 0, 0) != 0;
}

void UpdateSwatch(int i) {
  InvalidateRect(Item(IDC_COLOR_BTN + i), nullptr, FALSE);
  QueuePreview();
}

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
  QueuePreview();
}

// Aspetto ed elementi dai controlli al profilo (senza controlli di validità: li fa Sanitize).
void UiToLook(Profile& p) {
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
}

// Anteprima con quello che c'è nei controlli in questo momento, anche se non è ancora salvato.
void UpdatePreview() {
  HWND pv = Item(IDC_PREVIEW);
  if (!pv || g.cur < 0 || size_t(g.cur) >= g.profiles.size()) return;
  Profile p = g.profiles[size_t(g.cur)];
  UiToLook(p);
  Sanitize(p);
  ui::SetOverlayPreview(pv, p);
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
  UiToLook(p);
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
  SetCheck(IDC_FSHINT, g.cfg.fullscreenHint);
  SetCheck(IDC_AUTOSTART, g.cfg.autostart);
  SetSel(IDC_CHECKUPDATES, !g.cfg.checkUpdates ? 2 : g.cfg.autoUpdate ? 0 : 1);
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
  g.cfg.fullscreenHint = GetCheck(IDC_FSHINT);
  g.cfg.autostart = GetCheck(IDC_AUTOSTART);
  const int updates = GetSel(IDC_CHECKUPDATES);
  g.cfg.checkUpdates = updates != 2;
  g.cfg.autoUpdate = updates == 0;
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

// ------------------------------------------------------------------ colore dell'interfaccia
COLORREF CurrentAccent() {
  COLORREF c = ui::col::DefaultAccent;
  ui::ParseHexColor(g.cfg.uiAccent, c);
  return c;
}

// Applica subito il colore a tutta la finestra; resta salvato con "Salva e applica".
// La ruota e il campo del codice si allineano al nuovo colore, tranne quello da cui arriva.
void ApplyAccent(COLORREF c, int fromId = 0) {
  g.cfg.uiAccent = c == ui::col::DefaultAccent ? "" : ui::HexColor(c);
  ui::SetAccent(c);
  g.syncingAccent = true;
  if (fromId != IDC_ACCENT_WHEEL) SendMessageW(Item(IDC_ACCENT_WHEEL), ui::kMsgWheelSetColor, c, 0);
  if (fromId != IDC_ACCENT_HEX) SetUtf8(IDC_ACCENT_HEX, ui::HexColor(c));
  g.syncingAccent = false;
  RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
  Status(T(L"Colore applicato. Premi \"Salva e applica\" per tenerlo."), ui::col::Accent);
}

// Pallino di un colore pronto: anello chiaro se è quello in uso, anello d'accento con il focus da tastiera.
void DrawAccentChip(const DRAWITEMSTRUCT* d, COLORREF color) {
  HDC dc = d->hDC;
  const RECT& r = d->rcItem;
  FillRect(dc, &r, g.brPanel);
  const bool selected = color == CurrentAccent();
  const bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
  const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  const int outer = std::min(r.right - r.left, r.bottom - r.top) / 2;
  if (selected || focus) {
    const RECT ring{cx - outer, cy - outer, cx + outer, cy + outer};
    ui::StrokeRound(dc, ring, outer, focus ? ui::col::Accent : ui::col::Text, float(S(2)));
  }
  const int rad = outer - S(5);
  ui::FillCircle(dc, cx, cy, rad, color, color);
}

// "Usa nell'overlay...": un menu chiede a quale elemento dare il colore dell'interfaccia (quello che si
// vede, cioè già schiarito se era troppo scuro). Vale per il profilo selezionato, come gli altri colori.
void OnAccentToOverlay() {
  if (g.cur < 0) return;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, T(L"Dai questo colore a:"));
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  constexpr int kTargets = kColorCount - 1;  // tutti tranne lo sfondo
  for (int i = 0; i < kTargets; ++i) AppendMenuW(menu, MF_STRING, UINT_PTR(i + 1), kColorNames[i]);
  RECT r;
  GetWindowRect(Item(IDC_ACCENT_TO_OVERLAY), &r);
  const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left,
                                  r.bottom + 2, 0, g.wnd, nullptr);
  DestroyMenu(menu);
  if (pick < 1 || pick > kTargets) return;
  Profile& p = g.profiles[size_t(g.cur)];
  Color& c = ColorRef(p, pick - 1);
  c.r = GetRValue(ui::col::Accent);
  c.g = GetGValue(ui::col::Accent);
  c.b = GetBValue(ui::col::Accent);
  UpdateSwatch(pick - 1);
  Status(TF(L"Colore di \"{}\" nell'overlay cambiato per il profilo \"{}\". Premi \"Salva e applica\" per tenerlo.",
            kColorNames[pick - 1], ToWide(p.name)),
         ui::col::Accent);
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
      RECT r = ToClient(SR(f.r));
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
// Barra laterale a sinistra (sezioni), a destra una sezione alla volta divisa in riquadri.
// Coordinate a 96 DPI; ogni controllo appartiene alla pagina impostata in g_buildPage.
void BuildUi() {
  // Sempre visibili: sezioni, aggiornamento, avvio overlay, salvataggio.
  g_buildPage = -1;
  for (int i = 0; i < kPageCount; ++i) Button(IDC_NAV_BASE + i, kPages[i].name, 12, 76 + i * 42, 200, 38, kSidebar);
  Button(IDC_UPDATE, L"", 16, 506, 192, 34, kSidebar, true);
  ShowWindow(Item(IDC_UPDATE), SW_HIDE);
  Button(IDC_LAUNCH, T(L"Avvia overlay"), 16, 550, 192, 32, kSidebar);
  Button(IDC_SAVE, T(L"Salva e applica"), 826, 592, 150, 34, ui::col::Bg, true);

  // 0 · Profili
  g_buildPage = 0;
  Panel(T(L"Profili"), 248, 116, 260, 410);
  HWND list = Make(L"LISTBOX", L"",
                   WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                   256, 124, 244, 340, IDC_LIST);
  ApplyDarkControlTheme(list, L"DarkMode_Explorer");
  Button(IDC_NEW, T(L"Nuovo"), 256, 478, 118, 32, ui::col::Panel);
  Button(IDC_DELETE, T(L"Elimina"), 382, 478, 118, 32, ui::col::Panel);
  Panel(T(L"Profilo selezionato"), 528, 116, 448, 80);
  Label(T(L"Nome"), 544, 128, 110);
  Edit(IDC_NAME, 660, 128, 300);
  Label(T(L"Giochi"), 544, 162, 110);
  Edit(IDC_MATCH, 660, 162, 300, T(L"gioco.exe; altro.exe|Titolo"));
  Panel(T(L"Overlay pronti e preset RTSS"), 528, 240, 448, 64);
  Button(IDC_STORE, T(L"Store overlay..."), 544, 256, 200, 32, ui::col::Panel, true);
  Button(IDC_IMPORT_RTSS, T(L"Importa RTSS..."), 756, 256, 204, 32, ui::col::Panel);
  Panel(T(L"Backup e condivisione"), 528, 348, 448, 134);
  const std::pair<int, const wchar_t*> btns[] = {
      {IDC_EXPORT_ONE, T(L"Esporta profilo")}, {IDC_IMPORT_ONE, T(L"Importa profilo")},
      {IDC_EXPORT_ALL, T(L"Esporta tutto")},   {IDC_IMPORT_ALL, T(L"Importa tutto")},
      {IDC_BACKUP_CFG, T(L"Backup config")},   {IDC_RESTORE_CFG, T(L"Ripristina config")}};
  for (size_t i = 0; i < std::size(btns); ++i)
    Button(btns[i].first, btns[i].second, 544 + int(i % 2) * 212, 364 + int(i / 2) * 38, 204, 32, ui::col::Panel);

  // 1 · Aspetto overlay (profilo selezionato)
  g_buildPage = 1;
  constexpr int lx = 264, lw = 146, cx = 414;
  auto row = [](int i) { return 128 + i * 34; };
  Panel(T(L"Layout e posizione"), 248, 116, 728, 148);
  Label(L"Layout", lx, row(0), lw);
  Combo(IDC_LAYOUT, cx, row(0), 220, kLayoutNames);
  Button(IDC_EDITOR, L"Editor...", cx + 228, row(0), 110, kRowH, ui::col::Panel);
  Label(T(L"Posizione"), lx, row(1), lw);
  Combo(IDC_POSITION, cx, row(1), 220, kPositionNames);
  Label(T(L"X / Y (px a 1080p)"), lx, row(2), lw);
  Edit(IDC_X, cx, row(2), 105, L"X");
  Edit(IDC_Y, cx + 115, row(2), 105, L"Y");
  Label(T(L"Margine bordo (px)"), lx, row(3), lw);
  Edit(IDC_MARGIN, cx, row(3), 80);
  auto row2 = [](int i) { return 324 + i * 34; };
  Panel(T(L"Testo, sfondo e soglie FPS"), 248, 312, 728, 114);
  Label(L"Font", lx, row2(0), lw);
  static const wchar_t* const kFonts[] = {L"sans", L"mono", L"Segoe UI", L"Consolas", L"Bahnschrift"};
  Combo(IDC_FONT, cx, row2(0), 220, kFonts, true);
  Label(T(L"Dimensione (1080p)"), lx, row2(1), lw);
  Edit(IDC_FONTSIZE, cx, row2(1), 80);
  Check(IDC_BOLD, T(L"Grassetto"), cx + 96, row2(1) + 2, 170);
  Label(T(L"Opacità sfondo %"), lx, row2(2), lw);
  Edit(IDC_OPACITY, cx, row2(2), 80);
  Check(IDC_AUTOSCALE, T(L"Scala con risoluzione"), cx + 96, row2(2) + 2, 176);
  Label(T(L"FPS rosso sotto"), 690, row2(0), 140);
  Edit(IDC_FPSRED, 836, row2(0), 80);
  Label(T(L"FPS giallo sotto"), 690, row2(1), 140);
  Edit(IDC_FPSYELLOW, 836, row2(1), 80);
  Panel(T(L"Anteprima"), 248, 474, 728, 104);
  Make(ui::kOverlayPreviewClass, L"", 0, 256, 482, 712, 88, IDC_PREVIEW);

  // 2 · Elementi e sensori (profilo selezionato)
  g_buildPage = 2;
  Panel(T(L"Elementi visibili"), 248, 116, 350, 266);
  for (int i = 0; i < kShowCount; ++i) Check(IDC_SHOW + i, kShowNames[i], 264, 126 + i * 28, 320);
  Panel(T(L"Colori overlay"), 618, 116, 358, 160);
  for (int i = 0; i < kColorCount; ++i)
    Button(IDC_COLOR_BTN + i, kColorNames[i], 634 + (i % 2) * 166, 128 + (i / 2) * 36, 154, 30, ui::col::Panel);
  Panel(T(L"Sensori di sistema"), 618, 320, 358, 64);
  Button(IDC_SENSORS, T(L"Scegli i sensori..."), 634, 336, 326, 32, ui::col::Panel);

  // 3 · Hotkey
  g_buildPage = 3;
  Panel(T(L"Scorciatoie da tastiera"), 248, 116, 728, 182);
  Label(T(L"Mostra / nascondi"), 264, row(0), 200);
  HotkeyField(IDC_HK_TOGGLE, 470, row(0));
  Label(T(L"Cambia profilo"), 264, row(1), 200);
  HotkeyField(IDC_HK_CYCLE, 470, row(1));
  Label(T(L"Solo FPS"), 264, row(2), 200);
  HotkeyField(IDC_HK_FPSONLY, 470, row(2));
  Check(IDC_FPSONLY, T(L"Modalità solo FPS"), 264, row(3) + 2, 300);
  Label(T(L"Premi \"Cambia\" e poi la combinazione: Esc annulla, Canc la rimuove."), 264, row(4), 690);

  // 4 · Generali
  g_buildPage = 4;
  constexpr int gx = 420;
  Panel(L"Overlay", 248, 116, 728, 148);
  Label(L"Refresh (ms)", lx, row(0), 150);
  Edit(IDC_REFRESH, gx, row(0), 80);
  Label(T(L"Fuori dal gioco"), lx, row(1), 150);
  Combo(IDC_OUTOFGAME, gx, row(1), 260, kOutOfGameNames);
  Label(T(L"Profilo forzato"), lx, row(2), 150);
  Make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, gx, row(2) + 1, 260, 240, IDC_FORCED);
  ApplyDarkControlTheme(Item(IDC_FORCED), L"DarkMode_CFD");
  SendMessageW(Item(IDC_FORCED), CB_SETITEMHEIGHT, WPARAM(-1), S(kRowH - 8));
  SendMessageW(Item(IDC_FORCED), CB_SETITEMHEIGHT, 0, S(22));
  ui::MakeCombo(Item(IDC_FORCED), ui::col::Panel);
  Label(L"Lingua / Language", lx, row(3), 150);
  Combo(IDC_LANG, gx, row(3), 260, kLangNames);
  Panel(T(L"Giochi e avvio"), 248, 312, 728, 114);
  Check(IDC_AUTOLEARN, T(L"Rileva nuovi giochi automaticamente"), lx, row2(0) + 2, 440);
  Check(IDC_FSHINT, T(L"Avvisa se un gioco usa il fullscreen esclusivo"), lx, row2(1) + 2, 440);
  Check(IDC_AUTOSTART, T(L"Avvia PerfOverlay Supreme con Windows"), lx, row2(2) + 2, 440);
  Button(IDC_EDIT_GAMES, T(L"Lista giochi..."), 760, row2(0), 200, 32, ui::col::Panel);
  Button(IDC_OPEN_FOLDER, T(L"Apri cartella dati"), 760, row2(0) + 40, 200, 32, ui::col::Panel);
  Panel(T(L"Aggiornamenti"), 248, 474, 728, 52);
  Label(T(L"Installazione"), lx, 486, 150);
  Combo(IDC_CHECKUPDATES, gx, 486, 260, kUpdateNames);

  // 5 · Tema
  g_buildPage = 5;
  Panel(T(L"Colore dell'interfaccia"), 248, 116, 728, 300);
  Make(ui::kColorWheelClass, L"", WS_TABSTOP, 272, 132, 268, 268, IDC_ACCENT_WHEEL);
  Label(T(L"Colori pronti"), 572, 136, 300);
  for (int i = 0; i < kAccentCount; ++i)
    Button(IDC_ACCENT_BASE + i, kAccentPresets[i].name, 572 + i * 40, 168, 34, 34, ui::col::Panel);
  Label(T(L"Codice"), 572, 222, 80);
  Edit(IDC_ACCENT_HEX, 652, 222, 120, L"#RRGGBB");
  SendMessageW(Item(IDC_ACCENT_HEX), EM_LIMITTEXT, 7, 0);
  Button(IDC_ACCENT_RESET, T(L"Predefinito"), 572, 266, 140, 32, ui::col::Panel);
  Button(IDC_ACCENT_TO_OVERLAY, T(L"Usa nell'overlay..."), 722, 266, 190, 32, ui::col::Panel);
  Label(T(L"Gira l'anello per la tonalità, trascina nel quadrato"), 572, 318, 390);
  Label(T(L"per chiarezza e intensità. Da tastiera: frecce,"), 572, 340, 390);
  Label(T(L"PagSu e PagGiù. Il colore si applica subito."), 572, 362, 390);
  // Controlli d'esempio: mostrano il colore scelto su pulsanti, caselle e campi. Si possono provare
  // col mouse ma non fanno niente e restano fuori dal giro del Tab.
  Panel(T(L"Anteprima"), 248, 464, 728, 64);
  Button(IDC_DEMO_PRIMARY, T(L"Pulsante principale"), 264, 480, 180, 32, ui::col::Panel, true);
  Button(IDC_DEMO_BUTTON, T(L"Pulsante"), 456, 480, 130, 32, ui::col::Panel);
  Check(IDC_DEMO_CHECK, T(L"Casella"), 606, 484, 110);
  SetCheck(IDC_DEMO_CHECK, true);
  Edit(IDC_DEMO_EDIT, 728, 482, 150);
  SetText(IDC_DEMO_EDIT, T(L"Campo di testo"));
  for (int id : {IDC_DEMO_PRIMARY, IDC_DEMO_BUTTON, IDC_DEMO_CHECK, IDC_DEMO_EDIT})
    SetWindowLongW(Item(id), GWL_STYLE, GetWindowLongW(Item(id), GWL_STYLE) & ~WS_TABSTOP);
  g_buildPage = -1;
}

// Mostra una sezione: i controlli delle altre pagine si nascondono (e escono dall'ordine del Tab).
void ShowPage(int p) {
  if (p < 0 || p >= kPageCount) return;
  g.page = p;
  const HWND focus = GetFocus();
  for (size_t i = 0; i < g.layout.size(); ++i) {
    const int pg = g.layoutPage[i];
    if (pg < 0) continue;
    const HWND c = g.layout[i].first;
    if (pg != p && (c == focus || IsChild(c, focus))) SetFocus(Item(IDC_NAV_BASE + p));
    ShowWindow(c, pg == p ? SW_SHOWNA : SW_HIDE);
  }
  for (int i = 0; i < kPageCount; ++i) InvalidateRect(Item(IDC_NAV_BASE + i), nullptr, FALSE);
  InvalidateRect(g.wnd, nullptr, FALSE);
  if (p == 1) QueuePreview();
}

// ------------------------------------------------------------------ disegno
void DrawTextAt(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextW(dc, t.c_str(), int(t.size()), &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

std::wstring StatusPillText() { return g.monitorRunning ? T(L"Overlay in esecuzione") : T(L"Overlay non avviato"); }

// Badge dello stato in fondo alla barra laterale.
RECT StatusPillRect() { return SR({16, 596, 208, 626}); }

// Il pulsante "Aggiorna a vX" sta in fondo alla barra laterale; compare solo se c'è una versione nuova.
void PlaceUpdateButton() {
  HWND b = Item(IDC_UPDATE);
  if (!b) return;
  if (!g.update) {
    ShowWindow(b, SW_HIDE);
    return;
  }
  SetWindowTextW(b, TF(L"Aggiorna a v{}", ToWide(g.update->version)).c_str());
  ShowWindow(b, SW_SHOW);
}

// Controllo in background all'apertura: se c'è una versione nuova compare il pulsante.
void StartUpdateCheck() {
  if (!g.cfg.checkUpdates) return;
  std::thread([wnd = g.wnd] {
    std::wstring error;
    auto u = CheckForUpdate(error);
    if (!u) return;
    auto* info = new UpdateInfo(std::move(*u));
    if (!PostMessageW(wnd, kMsgUpdateFound, 0, reinterpret_cast<LPARAM>(info))) delete info;
  }).detach();
}

void OnUpdateClicked() {
  if (!g.update) return;
  const UpdateInfo& u = *g.update;
  std::wstring msg = TF(L"È disponibile PerfOverlay Supreme v{} (in uso: v{}).", ToWide(u.version), ToWide(AppVersion()));
  if (!u.notes.empty()) msg += L"\n\n" + ToWide(u.notes);
  msg += T(L"\n\nScaricarla e installarla ora? Impostazioni e overlay si riavviano da soli.");
  if (MessageBoxW(g.wnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
  if (UiToProfile() && UiToGlobal()) SaveAll();  // le modifiche in corso non vanno perse
  EnableWindow(Item(IDC_UPDATE), FALSE);
  Status(TF(L"Scaricamento della versione v{}...", ToWide(u.version)), ui::col::Accent);
  std::thread([wnd = g.wnd, u] {
    std::wstring error;
    const bool ok = DownloadUpdate(u, error);
    auto* err = new std::wstring(error);
    if (!PostMessageW(wnd, kMsgUpdateDownloaded, ok, reinterpret_cast<LPARAM>(err))) delete err;
  }).detach();
}

// Exe scaricato e verificato: chiude l'overlay, sostituisce l'exe e riapre tutto con la nuova versione.
void OnUpdateDownloaded(bool ok, std::wstring error) {
  if (ok) {
    HWND monitor = FindWindowW(kMonitorWndClass, nullptr);
    const bool monitorWasRunning = monitor != nullptr;
    if (monitor) {
      PostMessageW(monitor, kMsgQuit, 0, 0);
      for (int i = 0; i < 40 && FindWindowW(kMonitorWndClass, nullptr); ++i) Sleep(125);
    }
    std::wstring installError;
    if (InstallUpdate(installError)) {
      const auto exe = ExePath();
      ShellExecuteW(nullptr, L"open", exe.c_str(), L"--settings", nullptr, SW_SHOWNORMAL);
      if (monitorWasRunning) ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      DestroyWindow(g.wnd);
      return;
    }
    if (monitorWasRunning) ShellExecuteW(nullptr, L"open", ExePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ok = false;
    error = installError;
  }
  EnableWindow(Item(IDC_UPDATE), TRUE);
  Status(T(L"Aggiornamento non riuscito: ") + error, ui::col::Bad);
  if (g.update && !g.update->pageUrl.empty() &&
      MessageBoxW(g.wnd, (error + T(L"\n\nAprire la pagina della release?")).c_str(), kAppName,
                  MB_YESNO | MB_ICONWARNING) == IDYES)
    ShellExecuteW(nullptr, L"open", ToWide(g.update->pageUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Paint(HDC dc, const RECT& client) {
  FillRect(dc, &client, g.brBg);
  // Barra laterale a tutta altezza, con una riga di separazione.
  const RECT side{0, 0, g.ox + S(kSideW), client.bottom};
  FillRect(dc, &side, g.brSide);
  HBRUSH line = CreateSolidBrush(ui::col::Line);
  const RECT sep{side.right - 1, 0, side.right, client.bottom};
  FillRect(dc, &sep, line);
  DeleteObject(line);
  SetBkMode(dc, TRANSPARENT);
  SetViewportOrgEx(dc, g.ox, g.oy, nullptr);  // contenuto centrato nella finestra

  // Marchio: icona, nome con "Supreme" nel colore d'accento, versione.
  DrawIconEx(dc, S(18), S(20), g.icon, S(32), S(32), 0, nullptr, DI_NORMAL);
  SelectObject(dc, g.fontSection);
  SIZE nw{};
  GetTextExtentPoint32W(dc, L"PerfOverlay ", 12, &nw);
  DrawTextAt(dc, L"PerfOverlay ", SR({60, 16, 220, 38}), g.fontSection, ui::col::Text, DT_LEFT | DT_BOTTOM);
  DrawTextAt(dc, L"Supreme", {S(60) + nw.cx, S(16), S(220), S(38)}, g.fontSection, ui::col::Accent, DT_LEFT | DT_BOTTOM);
  DrawTextAt(dc, L"v" + ToWide(AppVersion()), SR({60, 38, 220, 56}), g.font, ui::col::Sub, DT_LEFT | DT_TOP);

  // Stato dell'overlay: badge arrotondato con pallino, verde se in esecuzione.
  const std::wstring state = StatusPillText();
  const COLORREF stateColor = g.monitorRunning ? ui::col::Good : ui::col::Bad;
  const RECT pill = StatusPillRect();
  ui::FillRound(dc, pill, S(15), ui::Blend(kSidebar, stateColor, 0.12f), ui::Blend(kSidebar, stateColor, 0.45f));
  ui::FillCircle(dc, pill.left + S(17), (pill.top + pill.bottom) / 2, S(4), stateColor, stateColor);
  DrawTextAt(dc, state, {pill.left + S(29), pill.top, pill.right - S(10), pill.bottom}, g.fontBold, stateColor,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

  // Intestazione della sezione.
  // Icona nel suo quadrato tinto del colore d'accento, titolo, descrizione; a destra la traccia degli FPS.
  const PageInfo& pg = kPages[g.page];
  ui::FillRound(dc, SR({248, 20, 290, 62}), S(11), ui::Blend(ui::col::Bg, ui::col::Accent, 0.15f),
                ui::Blend(ui::col::Bg, ui::col::Accent, 0.42f));
  DrawTextAt(dc, pg.glyph, SR({248, 20, 290, 62}), g.fontIconBig, ui::col::Accent, DT_CENTER | DT_VCENTER);
  ui::DrawTrace(dc, SR({770, 18, 966, 62}), ui::col::Accent, g.dpi / 96.0f);
  DrawTextAt(dc, pg.name, SR({304, 14, 760, 44}), g.fontTitle, ui::col::Text, DT_LEFT | DT_VCENTER);
  std::wstring sub = pg.subtitle ? pg.subtitle : L"";
  if (!pg.subtitle && g.cur >= 0 && size_t(g.cur) < g.profiles.size())
    sub = TF(L"Valgono per il profilo \"{}\": lo scegli nella sezione Profili.", ToWide(g.profiles[size_t(g.cur)].name));
  DrawTextAt(dc, sub, SR({305, 44, 760, 66}), g.font, ui::col::Sub, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

  for (const auto& p : g_panels) {
    if (p.page != g.page) continue;
    const RECT r = SR(p.r);
    DrawTextAt(dc, p.title, {r.left + S(4), r.top - S(24), r.right, r.top - S(4)}, g.fontSection, ui::col::Text,
               DT_LEFT | DT_BOTTOM);
    ui::FillRoundLit(dc, r, S(12), ui::col::Panel, RGB(78, 78, 90), ui::col::Line);
  }
  for (const auto& l : g_labels)
    if (l.page == g.page) DrawTextAt(dc, l.text, SR(l.r), g.font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  for (const auto& f : g_frames) {
    if (f.page != g.page) continue;
    const bool on = IsWindowEnabled(Item(f.id));
    const bool active = on && (g.focusedEdit == f.id || g_capturing == f.id);
    ui::FillRound(dc, SR(f.r), S(8), on ? ui::col::Input : kInputOff,
                  !on ? RGB(44, 44, 52) : active ? ui::col::Accent : ui::col::Field, active ? float(S(2)) : 1.0f);
  }
  DrawTextAt(dc, g.status, StatusRect(), g.font, g.statusColor, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  SetViewportOrgEx(dc, 0, 0, nullptr);  // la copia del buffer sullo schermo usa coordinate della finestra
}

// Voce della barra laterale: icona e nome; la sezione aperta ha fondo chiaro e barretta d'accento.
void DrawNavItem(const DRAWITEMSTRUCT* d, int i) {
  HDC dc = d->hDC;
  const RECT& r = d->rcItem;
  FillRect(dc, &r, g.brSide);
  const bool sel = i == g.page;
  const bool hover = ui::IsHovered(d->hwndItem);
  const bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
  if (sel)  // sezione aperta: fondo e bordo tinti del colore d'accento
    ui::FillRound(dc, r, S(10), ui::Blend(kSidebar, ui::col::Accent, 0.16f), ui::Blend(kSidebar, ui::col::Accent, 0.36f));
  else if (hover)
    ui::FillRound(dc, r, S(10), RGB(30, 30, 36), RGB(30, 30, 36), 0);
  if (focus) ui::StrokeRound(dc, r, S(10), ui::col::Accent, float(S(2)));
  SetBkMode(dc, TRANSPARENT);
  DrawTextAt(dc, kPages[i].glyph, {r.left + S(14), r.top, r.left + S(40), r.bottom}, g.fontIcon,
             sel ? ui::col::Accent : ui::col::Sub, DT_LEFT | DT_VCENTER);
  DrawTextAt(dc, kPages[i].name, {r.left + S(46), r.top, r.right - S(8), r.bottom}, sel ? g.fontBold : g.font,
             sel ? ui::col::Text : RGB(200, 200, 210), DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
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
  PlaceUpdateButton();
  const RECT r = ToClient(SR({0, 490, kSideW, kClientH}));
  InvalidateRect(g.wnd, &r, FALSE);
}

void Relayout(int clientW, int clientH);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED && g.wnd) Relayout(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_GETMINMAXINFO: {  // non più piccola del 60% (il testo resta leggibile)
      RECT r{0, 0, MulDiv(kClientW, g.baseDpi * 6, 960), MulDiv(kClientH, g.baseDpi * 6, 960)};
      AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, 0, UINT(g.baseDpi));
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
      return 0;
    }
    case WM_DPICHANGED: {  // finestra spostata su un monitor con DPI diversi
      g.baseDpi = HIWORD(wp);
      const auto* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
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
        const int ai = int(d->CtlID) - IDC_ACCENT_BASE;
        const int ni = int(d->CtlID) - IDC_NAV_BASE;
        if (ni >= 0 && ni < kPageCount) {
          DrawNavItem(d, ni);
        } else if (ai >= 0 && ai < kAccentCount) {
          DrawAccentChip(d, kAccentPresets[ai].color);
        } else if (ci >= 0 && ci < kColorCount && g.cur >= 0) {
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
      // Qualsiasi controllo dell'aspetto o degli elementi visibili cambia l'anteprima dell'overlay.
      if (((id >= IDC_LAYOUT && id <= IDC_MARGIN) || (id >= IDC_SHOW && id < IDC_SHOW + kShowCount)) &&
          (code == EN_CHANGE || code == CBN_SELCHANGE || code == CBN_EDITCHANGE || code == BN_CLICKED))
        QueuePreview();
      const bool isEdit = std::any_of(g_frames.begin(), g_frames.end(), [id](const FrameItem& f) { return f.id == id; });
      if (isEdit && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        g.focusedEdit = code == EN_SETFOCUS ? id : 0;
        for (const auto& f : g_frames)
          if (f.id == id) {
            RECT r = ToClient(SR(f.r));
            InvalidateRect(hwnd, &r, FALSE);
          }
        return 0;
      }
      if (id == IDC_ACCENT_WHEEL && code == ui::kColorWheelChanged) {
        ApplyAccent(COLORREF(SendMessageW(Item(IDC_ACCENT_WHEEL), ui::kMsgWheelGetColor, 0, 0)), IDC_ACCENT_WHEEL);
        return 0;
      }
      if (id == IDC_ACCENT_HEX && code == EN_CHANGE) {
        COLORREF c = 0;
        if (!g.syncingAccent && ui::ParseHexColor(GetUtf8(IDC_ACCENT_HEX), c)) ApplyAccent(c, IDC_ACCENT_HEX);
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
        if (id >= IDC_ACCENT_BASE && id < IDC_ACCENT_BASE + kAccentCount) ApplyAccent(kAccentPresets[id - IDC_ACCENT_BASE].color);
        if (id == IDC_ACCENT_RESET) ApplyAccent(ui::col::DefaultAccent);
        if (id == IDC_ACCENT_TO_OVERLAY) OnAccentToOverlay();
        if (id >= IDC_NAV_BASE && id < IDC_NAV_BASE + kPageCount) {
          EndCapture(false);
          ShowPage(id - IDC_NAV_BASE);
        }
        switch (id) {
          case IDC_IMPORT_RTSS: OnImportRtss(); break;
          case IDC_UPDATE: OnUpdateClicked(); break;
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
              if (RunLayoutEditor(g.wnd, g.inst, g.fontDlg, p)) {  // imposta da solo il layout (libero o RTSS)
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
              if (RunGallery(g.wnd, g.inst, g.fontDlg, p)) {
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
              if (RunSensorBrowser(g.wnd, g.inst, g.fontDlg, p)) {
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
            if (RunGamesEditor(hwnd, g.inst, g.fontDlg, g.games)) {
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
            ShellExecuteW(hwnd, L"open", ExePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    case kMsgRefreshPreview:
      g.previewQueued = false;
      UpdatePreview();
      return 0;
    case kMsgUpdateFound: {
      std::unique_ptr<UpdateInfo> u(reinterpret_cast<UpdateInfo*>(lp));
      g.update = std::move(*u);
      PlaceUpdateButton();
      Status(TF(L"È disponibile la versione v{}: premi \"Aggiorna\" in basso a sinistra.", ToWide(g.update->version)),
             ui::col::Accent);
      return 0;
    }
    case kMsgUpdateDownloaded: {
      std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
      OnUpdateDownloaded(wp != 0, *err);
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

// Icone della barra laterale: Segoe Fluent Icons (Windows 11), altrimenti Segoe MDL2 Assets (Windows 10).
HFONT MakeIconFont(int px = 16) {
  static const wchar_t* face = [] {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
      *reinterpret_cast<bool*>(p) = true;
      return 0;
    }, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
  }();
  return CreateFontW(-S(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

// Altezza del campo e delle voci di tutte le combobox, alla scala attuale. Va riapplicata dopo ogni
// cambio di font o di tema del controllo, che riportano il campo all'altezza scelta da Windows.
void ApplyComboHeights() {
  for (const auto& [c, r] : g.layout) {
    wchar_t cls[32];
    if (!GetClassNameW(c, cls, 32) || _wcsicmp(cls, L"ComboBox") != 0) continue;
    SendMessageW(c, CB_SETITEMHEIGHT, WPARAM(-1), S(kRowH - 8));
    SendMessageW(c, CB_SETITEMHEIGHT, 0, S(22));
  }
}

// La finestra si può ridimensionare e ingrandire: tutto (controlli, testo, pannelli) scala con lei
// in proporzione al disegno originale da 1000x806 a 96 DPI, centrato se le proporzioni sono diverse.
void Relayout(int clientW, int clientH) {
  if (clientW <= 0 || clientH <= 0) return;
  const double zoom = std::min(double(clientW) / MulDiv(kClientW, g.baseDpi, 96),
                               double(clientH) / MulDiv(kClientH, g.baseDpi, 96));
  const int dpi = std::max(48, int(std::lround(g.baseDpi * zoom)));
  const bool rescale = dpi != g.dpi || !g.font;
  g.dpi = dpi;
  g.ox = std::max(0, (clientW - S(kClientW)) / 2);
  g.oy = std::max(0, (clientH - S(kClientH)) / 2);
  SetPropW(g.wnd, ui::kScaleProp, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(MulDiv(1000, g.dpi, 96))));

  if (rescale) {
    const HFONT old[] = {g.font, g.fontBold, g.fontTitle, g.fontSection, g.fontIcon, g.fontIconBig};
    g.fontIcon = MakeIconFont();
    g.fontIconBig = MakeIconFont(21);
    g.font = MakeFont(95, FW_NORMAL);
    g.fontBold = MakeFont(95, FW_SEMIBOLD);
    g.fontTitle = MakeFont(160, FW_BOLD);
    g.fontSection = MakeFont(100, FW_SEMIBOLD);
    for (const auto& [c, r] : g.layout) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), FALSE);
    for (HFONT f : old)
      if (f) DeleteObject(f);
    if (g.icon) DestroyIcon(g.icon);
    g.icon = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(32), S(32), 0));
  }

  HDWP dwp = BeginDeferWindowPos(int(g.layout.size()));
  for (const auto& [c, r] : g.layout)
    if (dwp)
      dwp = DeferWindowPos(dwp, c, nullptr, g.ox + S(r.left), g.oy + S(r.top), S(r.right - r.left),
                           S(r.bottom - r.top), SWP_NOZORDER | SWP_NOACTIVATE);
  if (dwp) EndDeferWindowPos(dwp);

  if (rescale) {
    ApplyComboHeights();
    if (HWND list = Item(IDC_LIST)) SendMessageW(list, LB_SETITEMHEIGHT, 0, S(32));
  }
  // Le combobox modificabili (font) selezionano il testo quando cambiano dimensione: lo deselezioniamo.
  const HWND focus = GetFocus();
  for (const auto& [c, r] : g.layout) {
    wchar_t cls[32];
    if ((GetWindowLongW(c, GWL_STYLE) & 0x3) == CBS_DROPDOWN && GetClassNameW(c, cls, 32) &&
        _wcsicmp(cls, L"ComboBox") == 0 && !IsChild(c, focus))
      SendMessageW(c, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
  }
  PlaceUpdateButton();
  RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

}  // namespace

int RunSettingsWindow(HINSTANCE inst, int show) {
  g.inst = inst;
  g.dpi = g.baseDpi = int(GetDpiForSystem());
  g.cfg = LoadConfig();
  if (COLORREF accent; ui::ParseHexColor(g.cfg.uiAccent, accent)) ui::SetAccent(accent);
  g.games = LoadGames();
  g.profiles = LoadProfiles();
  g.cur = 0;

  EnableDarkModeForApp();
  ui::Init(inst);
  ui::RegisterColorWheel(inst);
  ui::RegisterOverlayPreview(inst);
  g.fontIcon = MakeIconFont();
  g.fontIconBig = MakeIconFont(21);
  g.font = MakeFont(95, FW_NORMAL);
  g.fontBold = MakeFont(95, FW_SEMIBOLD);
  g.fontTitle = MakeFont(160, FW_BOLD);
  g.fontSection = MakeFont(100, FW_SEMIBOLD);
  g.fontDlg = MakeFont(95, FW_NORMAL);
  g.brBg = CreateSolidBrush(ui::col::Bg);
  g.brPanel = CreateSolidBrush(ui::col::Panel);
  g.brInput = CreateSolidBrush(ui::col::Input);
  g.brInputOff = CreateSolidBrush(kInputOff);
  g.brSide = CreateSolidBrush(kSidebar);
  g.icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(32), S(32), 0));

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

  // Finestra ridimensionabile e nitida su ogni monitor (DPI per monitor); le finestre secondarie
  // restano come prima, a dimensione fissa.
  constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
  const DPI_AWARENESS_CONTEXT prevDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  g.wnd = CreateWindowExW(0, wc.lpszClassName, T(L"PerfOverlay Supreme - Impostazioni"), style, CW_USEDEFAULT, CW_USEDEFAULT,
                          S(kClientW), S(kClientH), nullptr, nullptr, inst, nullptr);
  if (prevDpi) SetThreadDpiAwarenessContext(prevDpi);
  if (!g.wnd) return 1;
  ApplyDarkTitleBar(g.wnd, ui::col::Bg);

  // Dimensione iniziale: quella del disegno ai DPI del monitor, ridotta se lo schermo è più piccolo
  // (es. portatili 1366x768 o scala 150% su 1080p), centrata nell'area di lavoro.
  g.baseDpi = int(GetDpiForWindow(g.wnd));
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTONEAREST), &mi);
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&frame, style, FALSE, 0, UINT(g.baseDpi));
  const int frameW = frame.right - frame.left, frameH = frame.bottom - frame.top;
  const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
  const int designW = MulDiv(kClientW, g.baseDpi, 96), designH = MulDiv(kClientH, g.baseDpi, 96);
  const double fit = std::min({1.0, (workW * 0.96 - frameW) / designW, (workH * 0.96 - frameH) / designH});
  const int winW = int(designW * fit) + frameW, winH = int(designH * fit) + frameH;
  SetWindowPos(g.wnd, nullptr, mi.rcWork.left + (workW - winW) / 2, mi.rcWork.top + (workH - winH) / 2, winW, winH,
               SWP_NOZORDER | SWP_NOACTIVATE);
  RECT client;
  GetClientRect(g.wnd, &client);
  Relayout(client.right, client.bottom);

  BuildUi();
  RefreshList();
  ProfileToUi();
  GlobalToUi();
  g.syncingAccent = true;  // ruota e codice mostrano il colore in uso, senza riapplicarlo
  SendMessageW(Item(IDC_ACCENT_WHEEL), ui::kMsgWheelSetColor, CurrentAccent(), 0);
  SetUtf8(IDC_ACCENT_HEX, ui::HexColor(CurrentAccent()));
  g.syncingAccent = false;
  ShowPage(0);
  UpdateMonitorState();
  SetTimer(g.wnd, kTimerMonitor, 1500, nullptr);
  StartUpdateCheck();
  ShowWindow(g.wnd, show);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(g.wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (HGDIOBJ o : std::initializer_list<HGDIOBJ>{g.font, g.fontBold, g.fontTitle, g.fontSection, g.fontDlg, g.fontIcon,
                                                  g.fontIconBig,
                                                  g.brBg, g.brPanel, g.brInput, g.brInputOff, g.brSide})
    DeleteObject(o);
  if (g.icon) DestroyIcon(g.icon);
  ui::Shutdown();
  return 0;
}

}  // namespace po
