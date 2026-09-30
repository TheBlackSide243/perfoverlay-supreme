#include "settings/games_editor.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/darkmode.h"
#include "common/resources.h"
#include "common/util.h"
#include "settings/theme.h"
#include "common/i18n.h"

namespace po {
namespace fs = std::filesystem;
namespace {

constexpr int kRowH = 28;
enum Kind { kGame, kLearned, kExcluded };
const wchar_t* const kKindNames[] = {T(L"Gioco"), T(L"Appreso in automatico"), T(L"Escluso (mai overlay)")};

enum : int {
  IDC_LIST = 4000,
  IDC_SEARCH,
  IDC_ADD_RUNNING,
  IDC_ADD_INSTALLED,
  IDC_ADD_EXE,
  IDC_MARK_GAME,
  IDC_MARK_EXCLUDED,
  IDC_REMOVE,
  IDC_TITLE,
  IDC_OK,
  IDC_CANCEL,
};

struct Entry {
  std::string process;  // minuscolo, "*" = qualsiasi
  std::string window;   // filtro sul titolo (opzionale)
  Kind kind = kGame;
};

struct Candidate {
  std::string exe;       // minuscolo
  std::wstring name;     // titolo della finestra o nome del gioco
  std::wstring source;   // "Aperto ora", "Steam", "Epic", "GOG"
  std::wstring path;
};

// ------------------------------------------------------------------ utilità comuni alle due finestre
struct Common {
  HINSTANCE inst = nullptr;
  HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr;
  HBRUSH brBg = nullptr, brInput = nullptr;
  int dpi = 96;
};
Common C;

int S(int v) { return MulDiv(v, C.dpi, 96); }
RECT SR(RECT r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }

HWND MakeCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), C.inst, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(C.font), TRUE);
  return c;
}
void MakeBtn(HWND parent, int id, const wchar_t* t, int x, int y, int w, int h, COLORREF bg, bool primary = false) {
  ui::MakeButton(MakeCtl(parent, L"BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, id), bg, primary);
}
void DrawTextAt(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextW(dc, t.c_str(), int(t.size()), &r, fmt | DT_NOPREFIX | DT_END_ELLIPSIS);
}

LRESULT CALLBACK HeaderTextSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  if (msg == WM_NOTIFY) {
    auto* nm = reinterpret_cast<NMHDR*>(lp);
    if (nm->hwndFrom == ListView_GetHeader(h) && nm->code == NM_CUSTOMDRAW) {
      auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
      if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        SetTextColor(cd->hdc, ui::col::Sub);
        return CDRF_DODEFAULT;
      }
    }
  }
  return DefSubclassProc(h, msg, wp, lp);
}

HWND MakeList(HWND parent, int id, RECT r, DWORD extra, std::initializer_list<std::pair<const wchar_t*, int>> cols,
              HIMAGELIST& rowHeight) {
  HWND lv = MakeCtl(parent, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | extra,
                    r.left, r.top, r.right - r.left, r.bottom - r.top, id);
  ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  ApplyDarkControlTheme(lv, L"DarkMode_Explorer");
  ApplyDarkControlTheme(ListView_GetHeader(lv), L"DarkMode_ItemsView");
  ListView_SetBkColor(lv, ui::col::Panel);
  ListView_SetTextBkColor(lv, CLR_NONE);
  ListView_SetTextColor(lv, ui::col::Text);
  rowHeight = ImageList_Create(1, S(24), ILC_COLOR32, 1, 0);
  ListView_SetImageList(lv, rowHeight, LVSIL_SMALL);
  SetWindowSubclass(lv, HeaderTextSubclass, 0, 0);
  int i = 0;
  for (const auto& [name, width] : cols) {
    LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH};
    c.pszText = const_cast<wchar_t*>(name);
    c.cx = S(width);
    ListView_InsertColumn(lv, i++, &c);
  }
  return lv;
}

void SetItemText(HWND lv, int row, int col, const std::wstring& t) {
  ListView_SetItemText(lv, row, col, const_cast<wchar_t*>(t.c_str()));
}

// Ciclo modale comune: disattiva il proprietario finché done diventa true.
void RunModal(HWND wnd, HWND owner, const bool& done) {
  EnableWindow(owner, FALSE);
  ShowWindow(wnd, SW_SHOW);
  MSG msg;
  while (!done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  DestroyWindow(wnd);
}

HWND CreateCentered(const wchar_t* cls, const std::wstring& title, HWND owner, int cw, int ch) {
  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  RECT r{0, 0, S(cw), S(ch)};
  AdjustWindowRect(&r, style, FALSE);
  RECT o;
  GetWindowRect(owner, &o);
  const int w = r.right - r.left, h = r.bottom - r.top;
  int x = o.left + ((o.right - o.left) - w) / 2, y = o.top + ((o.bottom - o.top) - h) / 2;
  MONITORINFO om{sizeof(om)};
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &om)) {
    x = std::clamp(x, int(om.rcWork.left), std::max(int(om.rcWork.left), int(om.rcWork.right) - w));
    y = std::clamp(y, int(om.rcWork.top), std::max(int(om.rcWork.top), int(om.rcWork.bottom) - h));
  }
  HWND wnd = CreateWindowExW(0, cls, title.c_str(), style, x, y, w, h, owner, nullptr, C.inst, nullptr);
  if (wnd) ApplyDarkTitleBar(wnd, ui::col::Bg);
  return wnd;
}

void PaintBuffered(HWND h, void (*paint)(HDC, const RECT&)) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT rc;
  GetClientRect(h, &rc);
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
  HGDIOBJ old = SelectObject(mem, bmp);
  FillRect(mem, &rc, C.brBg);
  SetBkMode(mem, TRANSPARENT);
  paint(mem, rc);
  BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
         mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(h, &ps);
}

LRESULT CtlColor(WPARAM wp, LPARAM lp) {
  HDC dc = reinterpret_cast<HDC>(wp);
  SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? ui::col::Text : ui::col::Disabled);
  SetBkColor(dc, ui::col::Input);
  return reinterpret_cast<LRESULT>(C.brInput);
}

std::wstring GetText(HWND h) {
  wchar_t buf[512];
  GetWindowTextW(h, buf, 512);
  return buf;
}

// ------------------------------------------------------------------ ricerca dei candidati
const std::string& WindowsDirLower() {
  static const std::string dir = [] {
    wchar_t buf[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    return ToLowerAscii(ToUtf8(std::wstring_view(buf, n))) + "\\";
  }();
  return dir;
}

std::vector<Candidate> RunningPrograms() {
  std::vector<Candidate> out;
  std::set<std::string> seen;
  struct Ctx {
    std::vector<Candidate>* out;
    std::set<std::string>* seen;
  } ctx{&out, &seen};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
          return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        wchar_t title[256];
        if (cloaked || GetWindowTextW(h, title, 256) == 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc) return TRUE;
        wchar_t path[MAX_PATH * 2];
        DWORD len = DWORD(std::size(path));
        const bool ok = QueryFullProcessImageNameW(proc, 0, path, &len);
        CloseHandle(proc);
        if (!ok) return TRUE;
        const std::string lower = ToLowerAscii(ToUtf8(std::wstring_view(path, len)));
        if (lower.rfind(WindowsDirLower(), 0) == 0) return TRUE;  // componenti di Windows
        const std::string exe = lower.substr(lower.find_last_of('\\') + 1);
        if (!c->seen->insert(exe).second) return TRUE;
        c->out->push_back({exe, title, T(L"Aperto ora"), path});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return out;
}

bool SkipExe(const std::string& lowerName) {
  static const char* kBad[] = {"unins",   "setup",     "install",  "crash",   "redist",  "vcredist", "dxsetup",
                               "dotnet",  "directx",   "anticheat", "eac_",   "battleye", "be_service", "cefprocess",
                               "helper",  "report",    "updater",  "uploader", "prereq",  "touchup",  "cleanup",
                               "ue4prereq", "subprocess", "webview", "overlay", "server",
                               "dedicated", "procdump", "7za", "7z.exe"};
  for (const char* b : kBad)
    if (lowerName.find(b) != std::string::npos) return true;
  return false;
}

// Eseguibili di un gioco installato (al massimo 4 livelli e 8 file, senza cartelle di redistribuibili).
void ScanGameFolder(const fs::path& root, const std::wstring& name, const std::wstring& source,
                    std::vector<Candidate>& out) {
  static const std::set<std::string> kSkipDirs = {"_commonredist", "redist",   "redistributables", "directx",
                                                  "vcredist",      "support",  "__installer",      "easyanticheat",
                                                  "battleye",      "engine",   "installers",       "prereqs"};
  std::error_code ec;
  int found = 0;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
  for (; it != end && found < 8; it.increment(ec)) {
    if (ec) break;
    const auto& p = it->path();
    if (it->is_directory(ec)) {
      if (it.depth() >= 3 || kSkipDirs.contains(ToLowerAscii(ToUtf8(p.filename().wstring())))) it.disable_recursion_pending();
      continue;
    }
    const std::string fname = ToLowerAscii(ToUtf8(p.filename().wstring()));
    if (p.extension() != L".exe" && p.extension() != L".EXE") continue;
    if (SkipExe(fname) || it->file_size(ec) < 64 * 1024) continue;
    out.push_back({fname, name, source, p.wstring()});
    ++found;
  }
}

std::string ReadFileText(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[1024];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  return buf;
}

void SteamGames(std::vector<Candidate>& out) {
  std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (steam.empty()) return;
  std::replace(steam.begin(), steam.end(), L'/', L'\\');
  std::vector<fs::path> libs = {fs::path(steam)};
  const std::string vdf = ReadFileText(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf");
  static const std::regex kPath("\"path\"\\s+\"([^\"]+)\"");
  for (std::sregex_iterator m(vdf.begin(), vdf.end(), kPath), e; m != e; ++m) {
    std::string p = (*m)[1].str();
    for (size_t i = p.find("\\\\"); i != std::string::npos; i = p.find("\\\\", i + 1)) p.erase(i, 1);
    const fs::path lib = ToWide(p);
    if (std::find(libs.begin(), libs.end(), lib) == libs.end()) libs.push_back(lib);
  }
  static const std::regex kName("\"name\"\\s+\"([^\"]+)\""), kDir("\"installdir\"\\s+\"([^\"]+)\"");
  for (const auto& lib : libs) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(lib / L"steamapps", ec)) {
      const std::wstring fn = e.path().filename().wstring();
      if (fn.rfind(L"appmanifest_", 0) != 0) continue;
      const std::string acf = ReadFileText(e.path());
      std::smatch n, d;
      if (!std::regex_search(acf, n, kName) || !std::regex_search(acf, d, kDir)) continue;
      const std::string name = n[1].str();
      if (IContains(name, "Redistributable") || IContains(name, "Proton") || IContains(name, "Steam Linux Runtime") ||
          IContains(name, "Steamworks"))
        continue;
      ScanGameFolder(lib / L"steamapps" / L"common" / ToWide(d[1].str()), ToWide(name), L"Steam", out);
    }
  }
}

void EpicGames(std::vector<Candidate>& out) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests", ec)) {
    if (e.path().extension() != L".item") continue;
    try {
      const auto j = nlohmann::json::parse(ReadFileText(e.path()));
      const std::wstring name = ToWide(j.value("DisplayName", ""));
      const fs::path dir = ToWide(j.value("InstallLocation", ""));
      const std::string launch = j.value("LaunchExecutable", "");
      if (!launch.empty()) {
        const fs::path exe = dir / ToWide(launch);
        out.push_back({ToLowerAscii(ToUtf8(exe.filename().wstring())), name, L"Epic", exe.wstring()});
      }
      ScanGameFolder(dir, name, L"Epic", out);
    } catch (...) {
    }
  }
}

void GogGames(std::vector<Candidate>& out) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &key) != ERROR_SUCCESS)
    return;
  wchar_t sub[256];
  for (DWORD i = 0; RegEnumKeyW(key, i, sub, 256) == ERROR_SUCCESS; ++i) {
    const std::wstring k = std::wstring(L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + sub;
    const std::wstring exe = RegString(HKEY_LOCAL_MACHINE, k.c_str(), L"exe");
    const std::wstring name = RegString(HKEY_LOCAL_MACHINE, k.c_str(), L"gameName");
    if (!exe.empty()) out.push_back({ToLowerAscii(ToUtf8(fs::path(exe).filename().wstring())), name, L"GOG", exe});
  }
  RegCloseKey(key);
}

std::vector<Candidate> InstalledGames() {
  std::vector<Candidate> out;
  SteamGames(out);
  EpicGames(out);
  GogGames(out);
  // Un exe una volta sola; ordinati per nome del gioco.
  std::vector<Candidate> unique;
  std::set<std::string> seen;
  for (auto& c : out)
    if (seen.insert(c.exe).second) unique.push_back(std::move(c));
  std::stable_sort(unique.begin(), unique.end(), [](const Candidate& a, const Candidate& b) {
    return ToLowerAscii(ToUtf8(a.name)) < ToLowerAscii(ToUtf8(b.name));
  });
  return unique;
}

// ------------------------------------------------------------------ finestra di scelta
constexpr wchar_t kPickerClass[] = L"POGamePicker";
enum : int { IDP_LIST = 4100, IDP_SEARCH, IDP_ADD, IDP_CANCEL };

struct Picker {
  HWND wnd = nullptr, list = nullptr;
  HIMAGELIST rowHeight = nullptr;
  std::wstring title, hint;
  std::vector<Candidate> all;
  std::vector<int> shown;       // indici in all per ogni riga
  std::set<int> checked;        // indici in all
  bool rebuilding = false, searchFocus = false;
  bool ok = false, done = false;
};
Picker* P = nullptr;

void PickerRebuild() {
  P->rebuilding = true;
  SendMessageW(P->list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(P->list);
  P->shown.clear();
  const std::string f = ToUtf8(GetText(GetDlgItem(P->wnd, IDP_SEARCH)));
  for (int i = 0; i < int(P->all.size()); ++i) {
    const auto& c = P->all[size_t(i)];
    if (!f.empty() && !IContains(c.exe, f) && !IContains(ToUtf8(c.name), f)) continue;
    LVITEMW it{LVIF_TEXT};
    it.iItem = int(P->shown.size());
    std::wstring exe = ToWide(c.exe);
    it.pszText = exe.data();
    const int row = ListView_InsertItem(P->list, &it);
    SetItemText(P->list, row, 1, c.name);
    SetItemText(P->list, row, 2, c.source);
    ListView_SetCheckState(P->list, row, P->checked.contains(i));
    P->shown.push_back(i);
  }
  SendMessageW(P->list, WM_SETREDRAW, TRUE, 0);
  P->rebuilding = false;
  InvalidateRect(P->wnd, nullptr, FALSE);
}

void PickerPaint(HDC dc, const RECT&) {
  DrawTextAt(dc, P->title, SR({20, 8, 600, 36}), C.fontTitle, ui::col::Accent, DT_LEFT | DT_TOP | DT_SINGLELINE);
  DrawTextAt(dc, P->hint, SR({21, 40, 560, 80}), C.font, ui::col::Sub, DT_LEFT | DT_TOP | DT_WORDBREAK);
  ui::FillRound(dc, SR({580, 16, 880, 16 + kRowH}), S(6), ui::col::Input, P->searchFocus ? ui::col::Accent : ui::col::Line);
  RECT border = SR({20, 88, 880, 560});
  InflateRect(&border, 1, 1);
  ui::FillRound(dc, border, S(2), ui::col::Line, ui::col::Line, 0);
  DrawTextAt(dc, TF(L"{} selezionati  ·  spunta i programmi da aggiungere (doppio clic = spunta)", P->checked.size()),
             SR({20, 576, 600, 608}), C.font, ui::col::Sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK PickerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT:
      PaintBuffered(h, PickerPaint);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_DRAWITEM:
      ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
      return CtlColor(wp, lp);
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->idFrom != IDP_LIST) return 0;
      if (nm->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
          cd->clrText = cd->iSubItem == 0 ? ui::col::Text : ui::col::Sub;
          SelectObject(cd->nmcd.hdc, cd->iSubItem == 0 ? C.fontBold : C.font);
          return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
      }
      if (nm->code == LVN_ITEMCHANGED && !P->rebuilding) {
        const auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) && lv->iItem >= 0 &&
            lv->iItem < int(P->shown.size())) {
          const int idx = P->shown[size_t(lv->iItem)];
          if (((lv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2)
            P->checked.insert(idx);
          else
            P->checked.erase(idx);
          const RECT r = SR({20, 570, 600, 612});
          InvalidateRect(h, &r, FALSE);
        }
      }
      if (nm->code == NM_DBLCLK) {
        const auto* ia = reinterpret_cast<NMITEMACTIVATE*>(lp);
        if (ia->iItem >= 0) ListView_SetCheckState(P->list, ia->iItem, !ListView_GetCheckState(P->list, ia->iItem));
      }
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      if (id == IDP_SEARCH && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        P->searchFocus = code == EN_SETFOCUS;
        const RECT r = SR({576, 12, 884, 48});
        InvalidateRect(h, &r, FALSE);
      } else if (id == IDP_SEARCH && code == EN_CHANGE) {
        PickerRebuild();
      } else if (id == IDP_ADD && code == BN_CLICKED) {
        P->ok = P->done = true;
      } else if (id == IDP_CANCEL && code == BN_CLICKED) {
        P->done = true;
      }
      return 0;
    }
    case WM_CLOSE:
      P->done = true;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

std::vector<Candidate> RunPicker(HWND owner, const std::wstring& title, const std::wstring& hint,
                                 std::vector<Candidate> all) {
  Picker pk;
  P = &pk;
  pk.title = title;
  pk.hint = hint;
  pk.all = std::move(all);
  pk.wnd = CreateCentered(kPickerClass, T(L"Aggiungi giochi"), owner, 900, 624);
  if (!pk.wnd) return {};
  HWND search = MakeCtl(pk.wnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 589, 22, 282, kRowH - 11, IDP_SEARCH);
  SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(T(L"Cerca")));
  pk.list = MakeList(pk.wnd, IDP_LIST, {20, 88, 880, 560}, 0,
                     {{T(L"Programma"), 280}, {T(L"Gioco / finestra"), 400}, {T(L"Origine"), 140}}, pk.rowHeight);
  ListView_SetExtendedListViewStyle(pk.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
  MakeBtn(pk.wnd, IDP_CANCEL, T(L"Annulla"), 640, 576, 104, 32, ui::col::Bg);
  MakeBtn(pk.wnd, IDP_ADD, T(L"Aggiungi"), 756, 576, 124, 32, ui::col::Bg, true);
  PickerRebuild();
  SetFocus(pk.list);
  RunModal(pk.wnd, owner, pk.done);
  if (pk.rowHeight) ImageList_Destroy(pk.rowHeight);
  std::vector<Candidate> out;
  if (pk.ok)
    for (int i : pk.checked) out.push_back(pk.all[size_t(i)]);
  P = nullptr;
  return out;
}

// ------------------------------------------------------------------ finestra principale
constexpr wchar_t kEditorClass[] = L"POGamesEditor";

struct Editor {
  HWND wnd = nullptr, list = nullptr;
  HIMAGELIST rowHeight = nullptr;
  std::vector<Entry> entries;
  std::vector<int> shown;  // indici in entries per ogni riga
  int focusedEdit = 0;
  bool updatingTitle = false;
  std::wstring status;
  bool ok = false, done = false;
};
Editor* G = nullptr;

HWND Item(int id) { return GetDlgItem(G->wnd, id); }

std::vector<int> SelectedEntries() {
  std::vector<int> v;
  for (int r = ListView_GetNextItem(G->list, -1, LVNI_SELECTED); r >= 0; r = ListView_GetNextItem(G->list, r, LVNI_SELECTED))
    if (r < int(G->shown.size())) v.push_back(G->shown[size_t(r)]);
  return v;
}

void Rebuild(const std::set<std::string>& select = {}) {
  std::stable_sort(G->entries.begin(), G->entries.end(), [](const Entry& a, const Entry& b) {
    return a.kind != b.kind ? a.kind < b.kind : a.process < b.process;
  });
  SendMessageW(G->list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(G->list);
  G->shown.clear();
  const std::string f = ToUtf8(GetText(Item(IDC_SEARCH)));
  for (int i = 0; i < int(G->entries.size()); ++i) {
    const Entry& e = G->entries[size_t(i)];
    if (!f.empty() && !IContains(e.process, f) && !IContains(e.window, f)) continue;
    LVITEMW it{LVIF_TEXT};
    it.iItem = int(G->shown.size());
    std::wstring p = ToWide(e.process == "*" ? TU("* (qualsiasi programma)") : e.process);
    it.pszText = p.data();
    const int row = ListView_InsertItem(G->list, &it);
    SetItemText(G->list, row, 1, ToWide(e.window));
    SetItemText(G->list, row, 2, kKindNames[e.kind]);
    if (select.contains(e.process)) ListView_SetItemState(G->list, row, LVIS_SELECTED, LVIS_SELECTED);
    G->shown.push_back(i);
  }
  SendMessageW(G->list, WM_SETREDRAW, TRUE, 0);
  if (const int first = ListView_GetNextItem(G->list, -1, LVNI_SELECTED); first >= 0)
    ListView_EnsureVisible(G->list, first, FALSE);
  InvalidateRect(G->wnd, nullptr, FALSE);
}

void UpdateTitleField() {
  const auto sel = SelectedEntries();
  const bool one = sel.size() == 1 && G->entries[size_t(sel[0])].kind != kExcluded;
  G->updatingTitle = true;
  SetWindowTextW(Item(IDC_TITLE), one ? ToWide(G->entries[size_t(sel[0])].window).c_str() : L"");
  G->updatingTitle = false;
  EnableWindow(Item(IDC_TITLE), one);
  for (int id : {IDC_MARK_GAME, IDC_MARK_EXCLUDED, IDC_REMOVE}) EnableWindow(Item(id), !sel.empty());
  InvalidateRect(G->wnd, nullptr, FALSE);
}

// Aggiunge (o sposta) i programmi scelti come giochi o esclusi.
void AddPrograms(const std::vector<std::string>& exes, Kind kind) {
  std::set<std::string> added;
  for (const auto& exe : exes) {
    if (exe.empty()) continue;
    auto it = std::find_if(G->entries.begin(), G->entries.end(),
                           [&](const Entry& e) { return e.process == exe && e.window.empty(); });
    if (it == G->entries.end())
      G->entries.push_back({exe, "", kind});
    else
      it->kind = kind;
    added.insert(exe);
  }
  G->status = added.empty() ? T(L"Nessun programma aggiunto.")
                            : TF(L"{} {} alla lista.", added.size(),
                                          added.size() == 1 ? T(L"programma aggiunto") : T(L"programmi aggiunti"));
  Rebuild(added);
  UpdateTitleField();
}

std::vector<Candidate> NotListed(std::vector<Candidate> c) {
  std::erase_if(c, [](const Candidate& x) {
    return std::any_of(G->entries.begin(), G->entries.end(),
                       [&](const Entry& e) { return e.process == x.exe && e.kind != kExcluded; });
  });
  return c;
}

void OnAddRunning() {
  auto c = NotListed(RunningPrograms());
  const auto picked = RunPicker(G->wnd, T(L"PROGRAMMI APERTI"),
                                T(L"Avvia il gioco, poi spuntalo qui. Sono elencati i programmi con una finestra aperta "
                                L"che non sono già nella lista."),
                                std::move(c));
  std::vector<std::string> exes;
  for (const auto& p : picked) exes.push_back(p.exe);
  if (!picked.empty()) AddPrograms(exes, kGame);
}

void OnAddInstalled() {
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  auto c = NotListed(InstalledGames());
  const auto picked = RunPicker(G->wnd, T(L"GIOCHI INSTALLATI"),
                                T(L"Eseguibili trovati nelle librerie Steam, Epic Games e GOG. Se un gioco ha più "
                                L"eseguibili spunta quello del gioco (es. ...-Win64-Shipping.exe)."),
                                std::move(c));
  std::vector<std::string> exes;
  for (const auto& p : picked) exes.push_back(p.exe);
  if (!picked.empty()) AddPrograms(exes, kGame);
}

void OnAddExe() {
  wchar_t buf[MAX_PATH * 4] = {};
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = G->wnd;
  ofn.lpstrFilter = T(L"Programmi (*.exe)\0*.exe\0");
  ofn.lpstrFile = buf;
  ofn.nMaxFile = DWORD(std::size(buf));
  ofn.lpstrTitle = T(L"Scegli l'eseguibile del gioco");
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return;
  std::vector<std::string> exes;
  // Selezione multipla: cartella\0file1\0file2\0\0; singola: percorso completo\0\0.
  const std::wstring first = buf;
  const wchar_t* p = buf + first.size() + 1;
  if (!*p) {
    exes.push_back(ToLowerAscii(ToUtf8(fs::path(first).filename().wstring())));
  } else {
    for (; *p; p += wcslen(p) + 1) exes.push_back(ToLowerAscii(ToUtf8(std::wstring(p))));
  }
  AddPrograms(exes, kGame);
}

void Mark(Kind kind) {
  std::set<std::string> sel;
  for (int i : SelectedEntries()) {
    G->entries[size_t(i)].kind = kind;
    if (kind == kExcluded) G->entries[size_t(i)].window.clear();  // gli esclusi sono solo per nome
    sel.insert(G->entries[size_t(i)].process);
  }
  Rebuild(sel);
  UpdateTitleField();
}

void RemoveSelected() {
  auto sel = SelectedEntries();
  std::sort(sel.rbegin(), sel.rend());
  for (int i : sel) G->entries.erase(G->entries.begin() + i);
  G->status = std::format(L"{} {}.", sel.size(), sel.size() == 1 ? T(L"voce rimossa") : T(L"voci rimosse"));
  Rebuild();
  UpdateTitleField();
}

void EditorPaint(HDC dc, const RECT&) {
  DrawTextAt(dc, T(L"LISTA GIOCHI"), SR({20, 8, 600, 36}), C.fontTitle, ui::col::Accent, DT_LEFT | DT_TOP | DT_SINGLELINE);
  DrawTextAt(dc,
             T(L"L'overlay compare su questi programmi. Quelli \"appresi\" li ha riconosciuti da solo (renderizzano e "
             L"usano molta GPU); gli esclusi non mostrano mai l'overlay."),
             SR({21, 40, 700, 80}), C.font, ui::col::Sub, DT_LEFT | DT_TOP | DT_WORDBREAK);
  ui::FillRound(dc, SR({740, 16, 980, 16 + kRowH}), S(6), ui::col::Input,
                G->focusedEdit == IDC_SEARCH ? ui::col::Accent : ui::col::Line);
  RECT border = SR({20, 88, 720, 588});
  InflateRect(&border, 1, 1);
  ui::FillRound(dc, border, S(2), ui::col::Line, ui::col::Line, 0);

  auto section = [&](const wchar_t* t, int y) {
    DrawTextAt(dc, t, SR({744, y, 980, y + 18}), C.fontBold, ui::col::Sub, DT_LEFT | DT_TOP | DT_SINGLELINE);
  };
  section(T(L"AGGIUNGI"), 88);
  section(T(L"VOCI SELEZIONATE"), 272);

  int counts[3] = {};
  for (const auto& e : G->entries) ++counts[e.kind];
  DrawTextAt(dc, TF(L"{} giochi  ·  {} appresi  ·  {} esclusi", counts[0], counts[1], counts[2]),
             SR({744, 456, 980, 480}), C.font, ui::col::Sub, DT_LEFT | DT_TOP | DT_SINGLELINE);
  DrawTextAt(dc, G->status, SR({744, 482, 980, 540}), C.font, ui::col::Good, DT_LEFT | DT_TOP | DT_WORDBREAK);

  DrawTextAt(dc, T(L"Solo se il titolo contiene"), SR({20, 604, 200, 632}), C.font, ui::col::Sub,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  ui::FillRound(dc, SR({204, 604, 520, 604 + kRowH}), S(6), ui::col::Input,
                G->focusedEdit == IDC_TITLE ? ui::col::Accent : ui::col::Line);
  DrawTextAt(dc, T(L"(facoltativo)"), SR({528, 604, 740, 632}), C.font, ui::col::Disabled,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT:
      PaintBuffered(h, EditorPaint);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_DRAWITEM:
      ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
      return CtlColor(wp, lp);
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->idFrom != IDC_LIST) return 0;
      if (nm->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
          const size_t row = cd->nmcd.dwItemSpec;
          const Kind k = row < G->shown.size() ? G->entries[size_t(G->shown[row])].kind : kGame;
          if (cd->iSubItem == 2)
            cd->clrText = k == kGame ? ui::col::Good : k == kLearned ? ui::col::Accent : ui::col::Bad;
          else
            cd->clrText = cd->iSubItem == 0 ? (k == kExcluded ? ui::col::Sub : ui::col::Text) : ui::col::Sub;
          SelectObject(cd->nmcd.hdc, cd->iSubItem == 0 && k != kExcluded ? C.fontBold : C.font);
          return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
      }
      if (nm->code == LVN_ITEMCHANGED) {
        const auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)) UpdateTitleField();
      }
      if (nm->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(lp)->wVKey == VK_DELETE) RemoveSelected();
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      if ((id == IDC_SEARCH || id == IDC_TITLE) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        G->focusedEdit = code == EN_SETFOCUS ? id : 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      if (id == IDC_SEARCH && code == EN_CHANGE) {
        Rebuild();
        UpdateTitleField();
        return 0;
      }
      if (id == IDC_TITLE && code == EN_CHANGE && !G->updatingTitle) {
        const auto sel = SelectedEntries();
        if (sel.size() == 1) {
          const int r = ListView_GetNextItem(G->list, -1, LVNI_SELECTED);
          G->entries[size_t(sel[0])].window = Trim(ToUtf8(GetText(Item(IDC_TITLE))));
          SetItemText(G->list, r, 1, ToWide(G->entries[size_t(sel[0])].window));
        }
        return 0;
      }
      if (code != BN_CLICKED) return 0;
      switch (id) {
        case IDC_ADD_RUNNING: OnAddRunning(); break;
        case IDC_ADD_INSTALLED: OnAddInstalled(); break;
        case IDC_ADD_EXE: OnAddExe(); break;
        case IDC_MARK_GAME: Mark(kGame); break;
        case IDC_MARK_EXCLUDED: Mark(kExcluded); break;
        case IDC_REMOVE: RemoveSelected(); break;
        case IDC_OK: G->ok = G->done = true; break;
        case IDC_CANCEL: G->done = true; break;
      }
      return 0;
    }
    case WM_CLOSE:
      G->done = true;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

HFONT MakeFont(int tenthsOfPoint, int weight) {
  return CreateFontW(-MulDiv(tenthsOfPoint, C.dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

bool RunGamesEditor(HWND owner, HINSTANCE inst, HFONT uiFont, GameList& games) {
  C.inst = inst;
  C.dpi = int(GetDpiForSystem());
  C.font = uiFont;
  C.fontBold = MakeFont(95, FW_SEMIBOLD);
  C.fontTitle = MakeFont(150, FW_BOLD);
  C.brBg = CreateSolidBrush(ui::col::Bg);
  C.brInput = CreateSolidBrush(ui::col::Input);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpfnWndProc = EditorProc;
    wc.lpszClassName = kEditorClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = PickerProc;
    wc.lpszClassName = kPickerClass;
    RegisterClassExW(&wc);
    registered = true;
  }

  Editor ed;
  G = &ed;
  for (const auto& e : games.known) ed.entries.push_back({e.process, e.window, kGame});
  for (const auto& e : games.learned) ed.entries.push_back({e.process, e.window, kLearned});
  for (const auto& e : games.exclude) ed.entries.push_back({e, "", kExcluded});

  ed.wnd = CreateCentered(kEditorClass, T(L"Lista giochi"), owner, 1000, 652);
  bool ok = false;
  if (ed.wnd) {
    HWND search = MakeCtl(ed.wnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 749, 22, 222, kRowH - 11, IDC_SEARCH);
    SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(T(L"Cerca")));
    ed.list = MakeList(ed.wnd, IDC_LIST, {20, 88, 720, 588}, 0,
                       {{T(L"Programma"), 280}, {T(L"Titolo contiene"), 200}, {T(L"Stato"), 190}}, ed.rowHeight);
    MakeBtn(ed.wnd, IDC_ADD_RUNNING, T(L"Dai programmi aperti..."), 744, 110, 236, 32, ui::col::Panel);
    MakeBtn(ed.wnd, IDC_ADD_INSTALLED, T(L"Dai giochi installati..."), 744, 150, 236, 32, ui::col::Panel);
    MakeBtn(ed.wnd, IDC_ADD_EXE, T(L"Scegli file .exe..."), 744, 190, 236, 32, ui::col::Panel);
    MakeBtn(ed.wnd, IDC_MARK_GAME, T(L"Segna come gioco"), 744, 294, 236, 32, ui::col::Panel);
    MakeBtn(ed.wnd, IDC_MARK_EXCLUDED, T(L"Escludi (mai overlay)"), 744, 334, 236, 32, ui::col::Panel);
    MakeBtn(ed.wnd, IDC_REMOVE, T(L"Rimuovi dalla lista"), 744, 374, 236, 32, ui::col::Panel);
    MakeCtl(ed.wnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 213, 610, 298, kRowH - 11, IDC_TITLE);
    MakeBtn(ed.wnd, IDC_CANCEL, T(L"Annulla"), 760, 602, 104, 32, ui::col::Bg);
    MakeBtn(ed.wnd, IDC_OK, L"OK", 876, 602, 104, 32, ui::col::Bg, true);
    Rebuild();
    UpdateTitleField();
    SetFocus(ed.list);
    RunModal(ed.wnd, owner, ed.done);
    ok = ed.ok;
  }
  if (ok) {
    games.known.clear();
    games.learned.clear();
    games.exclude.clear();
    for (const auto& e : ed.entries) {
      if (e.kind == kExcluded)
        games.exclude.push_back(e.process);
      else
        (e.kind == kGame ? games.known : games.learned).push_back({e.process, e.window});
    }
  }
  if (ed.rowHeight) ImageList_Destroy(ed.rowHeight);
  for (HGDIOBJ o : std::initializer_list<HGDIOBJ>{C.fontBold, C.fontTitle, C.brBg, C.brInput}) DeleteObject(o);
  G = nullptr;
  return ok;
}

}  // namespace po
