#include "settings/gallery.h"

#include <commctrl.h>
#include <urlmon.h>
#include <wininet.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/darkmode.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/resources.h"
#include "common/rtss_preset.h"
#include "common/util.h"
#include "settings/theme.h"
#include "common/i18n.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")

namespace po {
namespace fs = std::filesystem;
namespace {

// ------------------------------------------------------------------ overlay base inclusi
struct BasePreset {
  const wchar_t* name;
  const wchar_t* desc;
  void (*apply)(Profile&);
};

void Reset(Profile& p, const char* layout, const char* position, float font, int opacity) {
  p.layout = layout;
  p.position = position;
  p.fontSize = font;
  p.bgOpacity = opacity;
  p.show = ShowFlags{};
  p.fields.clear();
  p.style = "steam";
}

const BasePreset kBase[] = {
    {T(L"Steam classico"), T(L"Barra orizzontale in alto a sinistra: FPS e grafico, frame time, CPU, GPU, RAM."),
     [](Profile& p) { Reset(p, "bar", "top-left", 15, 55); }},
    {T(L"Solo FPS"), T(L"Solo gli FPS con minimo/massimo e 1% low, piccolo nell'angolo."),
     [](Profile& p) {
       Reset(p, "bar", "top-left", 16, 45);
       p.show.frametime = p.show.cpu = p.show.gpu = p.show.ram = p.show.battery = false;
       p.SetField("fps", "low1", true);
     }},
    {T(L"Completo verticale"), T(L"Una riga per elemento, con tutti i dati: temperature, frequenze, VRAM, velocità RAM."),
     [](Profile& p) {
       Reset(p, "vertical", "top-left", 16, 60);
       p.SetField("fps", "low1", true);
     }},
    {T(L"Compatto a destra"), T(L"Verticale piccolo nell'angolo in alto a destra: utilizzo e temperatura di CPU e GPU."),
     [](Profile& p) {
       Reset(p, "vertical", "top-right", 13, 50);
       p.show.graph = p.show.frametime = false;
       for (const char* f : {"topcore", "freq", "maxfreq"}) p.SetField("cpu", f, false);
       for (const char* f : {"clock", "vram"}) p.SetField("gpu", f, false);
       p.SetField("ram", "speed", false);
     }},
    {T(L"Benchmark grande"), T(L"Verticale grande e ben leggibile per registrare o confrontare, sfondo più scuro."),
     [](Profile& p) {
       Reset(p, "vertical", "top-left", 22, 75);
       p.SetField("fps", "low1", true);
     }},
    {T(L"Trasparente"), T(L"Barra senza sfondo, solo testo: disturba il meno possibile."),
     [](Profile& p) { Reset(p, "bar", "top-left", 15, 0); }},
};

// ------------------------------------------------------------------ store (GitHub)
// Pacchetti di overlay RTSS pubblicati su GitHub. "release": zip dell'ultima versione; "tree": i file
// .ovl/.ovx (con immagini e font) presi direttamente dal repository.
struct Package {
  std::wstring name, author, desc;
  std::string repo;  // "proprietario/nome"
  bool release = false;
  int stars = -1;    // solo per quelli trovati con la ricerca
};

std::vector<Package> Featured() {
  return {
      {L"TroyMetrics Benchmark", L"TroyMetrics",
       T(L"Overlay benchmark completo: FPS, frametime, CPU con barchart per core, GPU, VRAM, RAM, animazioni (1080p e "
       L"1440p, varie colorazioni)."),
       "TroyMetrics/Benchmark-Overlays", true},
      {L"RTSS-Overlay", L"PeterKelemen2",
       T(L"Set di overlay per combinazioni di CPU e GPU AMD / Intel / NVIDIA (GPL-3.0)."), "PeterKelemen2/RTSS-Overlay"},
      {L"My RivaTuner overlays", L"PillarsZhang", T(L"Overlay compatti con opacità e grandezza diverse."),
       "PillarsZhang/my-rivatuner-overlays"},
      {L"Minimalist overlay", L"itsmeraktim", T(L"Overlay minimale e pulito, pensato per i portatili Lenovo."),
       "itsmeraktim/msiab_ovlconfig_itsmeraktim"},
  };
}

fs::path PackageDir(const Package& p) {
  std::string d = p.repo;
  std::replace(d.begin(), d.end(), '/', '_');
  return AppDataDir() / L"downloads" / ToWide(d);
}

std::vector<fs::path> PackageOverlays(const Package& p) {
  std::vector<fs::path> v;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(PackageDir(p), ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    const auto ext = ToLowerAscii(ToUtf8(it->path().extension().wstring()));
    if (ext == ".ovl" || ext == ".ovx") v.push_back(it->path());
  }
  std::sort(v.begin(), v.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
  return v;
}

std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// GET di un JSON dell'API di GitHub (salvato in un file temporaneo del pacchetto).
std::optional<nlohmann::json> GitHubJson(const std::wstring& url) {
  std::error_code ec;
  const fs::path tmp = AppDataDir() / L"downloads" / L"api.json";
  fs::create_directories(tmp.parent_path(), ec);
  DeleteUrlCacheEntryW(url.c_str());
  if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), tmp.c_str(), 0, nullptr))) return std::nullopt;
  try {
    auto j = nlohmann::json::parse(ReadAll(tmp));
    fs::remove(tmp, ec);
    return j;
  } catch (...) {
    return std::nullopt;
  }
}

std::wstring UrlPath(const std::string& path) {  // spazi e caratteri speciali nei percorsi dei file
  std::string out;
  for (unsigned char c : path) {
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
      out += char(c);
    else
      out += std::format("%{:02X}", c);
  }
  return ToWide(out);
}

bool DownloadRelease(const Package& p, std::wstring& error) {
  const auto j = GitHubJson(ToWide("https://api.github.com/repos/" + p.repo + "/releases/latest"));
  std::string url, tag;
  if (j) {
    tag = j->value("tag_name", "");
    if (j->contains("assets"))
      for (const auto& a : j->at("assets"))
        if (a.value("name", "").ends_with(".zip")) url = a.value("browser_download_url", "");
  }
  if (url.empty()) {
    error = T(L"Nessun pacchetto nell'ultima versione (o GitHub non raggiungibile).");
    return false;
  }
  std::error_code ec;
  fs::remove_all(PackageDir(p), ec);
  fs::create_directories(PackageDir(p), ec);
  const fs::path zip = PackageDir(p) / L"release.zip";
  if (FAILED(URLDownloadToFileW(nullptr, ToWide(url).c_str(), zip.c_str(), 0, nullptr))) {
    error = T(L"Download non riuscito.");
    return false;
  }
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);  // tar.exe di Windows estrae gli zip
  std::wstring cmd = std::format(L"\"{}\\tar.exe\" -xf \"{}\" -C \"{}\"", sys, zip.wstring(), PackageDir(p).wstring());
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = T(L"Impossibile estrarre il pacchetto.");
    return false;
  }
  WaitForSingleObject(pi.hProcess, 60000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  fs::remove(zip, ec);
  LogInfo("Store: {} {} scaricato", p.repo, tag);
  return true;
}

bool DownloadTree(const Package& p, std::wstring& error) {
  const auto info = GitHubJson(ToWide("https://api.github.com/repos/" + p.repo));
  const std::string branch = info ? info->value("default_branch", "main") : "main";
  const auto tree = GitHubJson(ToWide("https://api.github.com/repos/" + p.repo + "/git/trees/" + branch + "?recursive=1"));
  if (!tree || !tree->contains("tree")) {
    error = T(L"GitHub non raggiungibile o repository non trovato.");
    return false;
  }
  std::error_code ec;
  fs::remove_all(PackageDir(p), ec);
  int n = 0;
  for (const auto& t : tree->at("tree")) {
    const std::string path = t.value("path", "");
    const auto ext = ToLowerAscii(ToUtf8(fs::path(ToWide(path)).extension().wstring()));
    if (t.value("type", "") != "blob" || t.value("size", 0) > 20 * 1024 * 1024) continue;
    if (ext != ".ovl" && ext != ".ovx" && ext != ".png" && ext != ".ttf" && ext != ".otf") continue;
    if (path.rfind("docs/", 0) == 0) continue;  // immagini della documentazione
    const fs::path rel = fs::path(ToWide(path)).lexically_normal();
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() || *rel.begin() == L"..") continue;  // resta nella cartella del pacchetto
    const fs::path dst = PackageDir(p) / rel;
    fs::create_directories(dst.parent_path(), ec);
    const std::wstring url =
        ToWide("https://raw.githubusercontent.com/" + p.repo + "/" + branch + "/") + UrlPath(path);
    if (SUCCEEDED(URLDownloadToFileW(nullptr, url.c_str(), dst.c_str(), 0, nullptr))) ++n;
  }
  LogInfo("Store: {} scaricato ({} file)", p.repo, n);
  return true;
}

// Ricerca di altri repository con overlay RTSS.
std::vector<Package> SearchGitHub() {
  std::vector<Package> out;
  for (const char* q : {"rtss+overlay", "rivatuner+overlay", "rtss+overlayeditor", "rivatuner+ovl"}) {
    const auto j = GitHubJson(ToWide(std::string("https://api.github.com/search/repositories?per_page=20&q=") + q));
    if (!j || !j->contains("items")) continue;
    for (const auto& r : j->at("items")) {
      Package p;
      p.repo = r.value("full_name", "");
      if (p.repo.empty() || std::any_of(out.begin(), out.end(), [&](const Package& o) { return o.repo == p.repo; }))
        continue;
      p.name = ToWide(r.value("name", p.repo));
      p.author = ToWide(r.contains("owner") ? r["owner"].value("login", "") : "");
      p.desc = ToWide(r["description"].is_string() ? r["description"].get<std::string>() : "");
      p.stars = r.value("stargazers_count", 0);
      // Solo raccolte di overlay: niente progetti che toccano i processi di RTSS o dei giochi.
      const std::string d = ToLowerAscii(ToUtf8(p.desc));
      if (d.find("inject") != std::string::npos || d.find("hijack") != std::string::npos ||
          d.find("exploit") != std::string::npos || d.find("cheat") != std::string::npos)
        continue;
      out.push_back(std::move(p));
    }
  }
  std::sort(out.begin(), out.end(), [](const Package& a, const Package& b) { return a.stars > b.stars; });
  return out;
}

// ------------------------------------------------------------------ finestra
enum : int { IDC_LIST = 4200, IDC_APPLY, IDC_DOWNLOAD, IDC_CLOSE, IDC_SEARCH };
constexpr wchar_t kClass[] = L"POGallery";
constexpr int kW = 880, kH = 620;

struct Row {
  enum class K { Header, Base, Package, Overlay } k;
  int index = -1;  // Base: preset; Package/Overlay: pacchetto
  fs::path file;
};

struct Gallery {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr, list = nullptr;
  HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr;
  HBRUSH brBg = nullptr;
  HIMAGELIST rowHeight = nullptr;
  int dpi = 96;
  Profile* profile = nullptr;
  std::vector<Package> packages;  // in evidenza + trovati con la ricerca
  size_t featuredCount = 0;
  std::vector<Row> rows;
  std::wstring status;
  COLORREF statusColor = ui::col::Sub;
  bool applied = false, done = false;
};
Gallery* G = nullptr;

int S(int v) { return MulDiv(v, G->dpi, 96); }
RECT SR(RECT r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }

void AddRow(const std::wstring& a, const std::wstring& b, Row r) {
  LVITEMW it{LVIF_TEXT};
  it.iItem = int(G->rows.size());
  std::wstring t = a;
  it.pszText = t.data();
  const int i = ListView_InsertItem(G->list, &it);
  ListView_SetItemText(G->list, i, 1, const_cast<wchar_t*>(b.c_str()));
  G->rows.push_back(std::move(r));
}

void AddPackage(int idx) {
  const Package& p = G->packages[size_t(idx)];
  const auto files = PackageOverlays(p);
  std::wstring info = p.author.empty() ? p.desc : T(L"di ") + p.author + L"  ·  " + p.desc;
  if (p.stars >= 0) info = std::format(L"\u2605 {}  ·  ", p.stars) + info;
  AddRow(files.empty() ? L"    ⬇  " + p.name + T(L"   (da scaricare)")
                       : std::format(L"    ✔  {}   ({} overlay)", p.name, files.size()),
         info, {Row::K::Package, idx});
  for (const auto& f : files) {
    const std::wstring stem = f.stem().wstring();
    AddRow(L"          " + stem,
           stem.find(L"1080p") != std::wstring::npos ? T(L"doppio clic per applicare  ·  per schermi 1080p")
                                                     : T(L"doppio clic per applicare"),
           {Row::K::Overlay, idx, f});
  }
}

void Fill() {
  const int top = ListView_GetTopIndex(G->list);
  SendMessageW(G->list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(G->list);
  G->rows.clear();
  AddRow(T(L"OVERLAY BASE (inclusi)"), L"", {Row::K::Header});
  for (int i = 0; i < int(std::size(kBase)); ++i)
    AddRow(std::wstring(L"    ") + kBase[i].name, kBase[i].desc, {Row::K::Base, i});
  AddRow(T(L"STORE  ·  IN EVIDENZA"), T(L"overlay RTSS pubblicati su GitHub dai loro autori"), {Row::K::Header});
  for (size_t i = 0; i < G->featuredCount; ++i) AddPackage(int(i));
  if (G->packages.size() > G->featuredCount) {
    AddRow(T(L"STORE  ·  TROVATI SU GITHUB"), T(L"non verificati: se un repository non contiene overlay non viene aggiunto nulla"),
           {Row::K::Header});
    for (size_t i = G->featuredCount; i < G->packages.size(); ++i) AddPackage(int(i));
  }
  SendMessageW(G->list, WM_SETREDRAW, TRUE, 0);
  if (top > 0) ListView_EnsureVisible(G->list, std::min(top + 12, ListView_GetItemCount(G->list) - 1), FALSE);
  InvalidateRect(G->list, nullptr, TRUE);
}

void SetStatus(const std::wstring& s, COLORREF c = ui::col::Sub) {
  G->status = s;
  G->statusColor = c;
  const RECT r = SR({20, 572, 420, 604});
  InvalidateRect(G->wnd, &r, FALSE);
  UpdateWindow(G->wnd);
}

const Row* Selected() {
  const int r = ListView_GetNextItem(G->list, -1, LVNI_SELECTED);
  return r >= 0 && r < int(G->rows.size()) ? &G->rows[size_t(r)] : nullptr;
}

void Download(int idx) {
  const Package& p = G->packages[size_t(idx)];
  SetStatus(T(L"Scaricamento di \"") + p.name + T(L"\" da GitHub..."));
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  std::wstring err;
  const bool ok = p.release ? DownloadRelease(p, err) : DownloadTree(p, err);
  const size_t n = PackageOverlays(p).size();
  const std::wstring name = p.name;
  Fill();
  if (!ok)
    SetStatus(err, ui::col::Bad);
  else if (n == 0)
    SetStatus(L"\"" + name + T(L"\" non contiene overlay RTSS (.ovl/.ovx)."), ui::col::Bad);
  else
    SetStatus(TF(L"\"{}\": {} overlay scaricati. Scegline uno e premi Applica.", name, n), ui::col::Good);
}

void Search() {
  SetStatus(T(L"Ricerca di overlay RTSS su GitHub..."));
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  G->packages.resize(G->featuredCount);
  for (auto& p : SearchGitHub())
    if (std::none_of(G->packages.begin(), G->packages.end(), [&](const Package& o) { return IEquals(o.repo, p.repo); }))
      G->packages.push_back(std::move(p));
  Fill();
  const size_t found = G->packages.size() - G->featuredCount;
  SetStatus(found ? TF(L"Trovati {} repository: doppio clic per scaricarne gli overlay.", found)
                  : std::wstring(T(L"Nessun altro repository trovato (o GitHub non raggiungibile).")),
            found ? ui::col::Good : ui::col::Sub);
}

void Apply() {
  const Row* row = Selected();
  if (!row) return;
  Profile& p = *G->profile;
  if (row->k == Row::K::Package) {
    Download(row->index);
  } else if (row->k == Row::K::Base) {
    kBase[row->index].apply(p);
    Sanitize(p);
    G->applied = true;
    SetStatus(TF(L"\"{}\" applicato al profilo \"{}\".", kBase[row->index].name, ToWide(p.name)),
              ui::col::Good);
  } else if (row->k == Row::K::Overlay) {
    const auto res = ImportRtssPreset(row->file);
    if (!res.ok) {
      SetStatus(T(L"Overlay non importato: ") + ToWide(res.error), ui::col::Bad);
      return;
    }
    p.rtss = res.layout;
    p.layout = "rtss";
    if (res.layout.advanced) {  // i preset avanzati hanno già la loro grandezza e stanno in alto a sinistra
      p.position = "top-left";
      p.fontSize = 15;
    }
    Sanitize(p);
    G->applied = true;
    SetStatus(TF(L"\"{}\" applicato al profilo \"{}\".", row->file.stem().wstring(), ToWide(p.name)),
              ui::col::Good);
  }
}

void Paint(HDC dc, const RECT& rc) {
  FillRect(dc, &rc, G->brBg);
  SetBkMode(dc, TRANSPARENT);
  auto text = [&](const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_END_ELLIPSIS);
  };
  text(L"STORE OVERLAY", SR({20, 8, 600, 36}), G->fontTitle, ui::col::Accent, DT_LEFT | DT_TOP | DT_SINGLELINE);
  text(TF(L"Scegli un overlay e premi \"Applica\": va nel profilo \"{}\" e compare subito in gioco.",
                   ToWide(G->profile->name)),
       SR({21, 40, 860, 60}), G->font, ui::col::Sub, DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT border = SR({20, 72, 860, 556});
  InflateRect(&border, 1, 1);
  ui::FillRound(dc, border, S(2), ui::col::Line, ui::col::Line, 0);
  text(G->status, SR({20, 572, 420, 604}), G->font, G->statusColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      Paint(mem, rc);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DRAWITEM:
      ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->idFrom != IDC_LIST) return 0;
      if (nm->code == NM_DBLCLK) Apply();
      if (nm->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
          const size_t r = cd->nmcd.dwItemSpec;
          const bool header = r < G->rows.size() && G->rows[r].k == Row::K::Header;
          cd->clrText = header ? ui::col::Accent : cd->iSubItem == 0 ? ui::col::Text : ui::col::Sub;
          SelectObject(cd->nmcd.hdc, header || cd->iSubItem == 0 ? G->fontBold : G->font);
          return CDRF_NEWFONT;
        }
      }
      return 0;
    }
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == IDC_APPLY) Apply();
        if (LOWORD(wp) == IDC_DOWNLOAD) {
          if (const Row* r = Selected(); r && (r->k == Row::K::Package || r->k == Row::K::Overlay))
            Download(r->index);
          else
            SetStatus(T(L"Seleziona un pacchetto dello store da scaricare o aggiornare."));
        }
        if (LOWORD(wp) == IDC_SEARCH) Search();
        if (LOWORD(wp) == IDC_CLOSE) G->done = true;
      }
      return 0;
    case WM_CLOSE:
      G->done = true;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), G->wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), G->inst, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(G->font), TRUE);
  return c;
}

HFONT MakeFont(int tenths, int weight) {
  return CreateFontW(-MulDiv(tenths, G->dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

bool RunGallery(HWND owner, HINSTANCE inst, HFONT uiFont, Profile& profile) {
  Gallery g;
  G = &g;
  g.inst = inst;
  g.dpi = int(GetDpiForSystem());
  g.profile = &profile;
  g.packages = Featured();
  g.featuredCount = g.packages.size();
  g.font = uiFont;
  g.fontBold = MakeFont(95, FW_SEMIBOLD);
  g.fontTitle = MakeFont(150, FW_BOLD);
  g.brBg = CreateSolidBrush(ui::col::Bg);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    registered = true;
  }
  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  RECT r{0, 0, S(kW), S(kH)};
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
  g.wnd = CreateWindowExW(0, kClass, L"Store overlay", style, x, y, w, h, owner, nullptr, inst, nullptr);
  if (g.wnd) {
    ApplyDarkTitleBar(g.wnd, ui::col::Bg);
    g.list = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                  20, 72, 840, 484, IDC_LIST);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ApplyDarkControlTheme(g.list, L"DarkMode_Explorer");
    ApplyDarkControlTheme(ListView_GetHeader(g.list), L"DarkMode_ItemsView");
    ListView_SetBkColor(g.list, ui::col::Panel);
    ListView_SetTextBkColor(g.list, CLR_NONE);
    ListView_SetTextColor(g.list, ui::col::Text);
    g.rowHeight = ImageList_Create(1, S(26), ILC_COLOR32, 1, 0);
    ListView_SetImageList(g.list, g.rowHeight, LVSIL_SMALL);
    const std::pair<const wchar_t*, int> cols[] = {{L"Overlay", 330}, {T(L"Descrizione"), 490}};
    for (int i = 0; i < 2; ++i) {
      LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH};
      c.pszText = const_cast<wchar_t*>(cols[i].first);
      c.cx = S(cols[i].second);
      ListView_InsertColumn(g.list, i, &c);
    }
    ui::MakeButton(Make(L"BUTTON", T(L"Cerca su GitHub"), WS_TABSTOP | BS_OWNERDRAW, 428, 572, 132, 32, IDC_SEARCH),
                   ui::col::Panel, false);
    ui::MakeButton(Make(L"BUTTON", T(L"Scarica / aggiorna"), WS_TABSTOP | BS_OWNERDRAW, 568, 572, 132, 32, IDC_DOWNLOAD),
                   ui::col::Panel, false);
    ui::MakeButton(Make(L"BUTTON", T(L"Chiudi"), WS_TABSTOP | BS_OWNERDRAW, 708, 572, 70, 32, IDC_CLOSE), ui::col::Bg,
                   false);
    ui::MakeButton(Make(L"BUTTON", T(L"Applica"), WS_TABSTOP | BS_OWNERDRAW, 786, 572, 74, 32, IDC_APPLY), ui::col::Bg,
                   true);
    Fill();
    EnableWindow(owner, FALSE);
    ShowWindow(g.wnd, SW_SHOW);
    SetFocus(g.list);
    MSG msg;
    while (!g.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (IsDialogMessageW(g.wnd, &msg)) continue;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(g.wnd);
  }
  if (g.rowHeight) ImageList_Destroy(g.rowHeight);
  for (HGDIOBJ o2 : std::initializer_list<HGDIOBJ>{g.fontBold, g.fontTitle, g.brBg}) DeleteObject(o2);
  G = nullptr;
  return g.applied;
}

}  // namespace po
