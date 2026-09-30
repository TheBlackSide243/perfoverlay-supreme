#include "settings/sensor_browser.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "common/branding.h"
#include "common/darkmode.h"
#include "common/lhm.h"
#include "common/resources.h"
#include "common/sensor_feed.h"
#include "common/util.h"
#include "settings/theme.h"
#include "common/i18n.h"

namespace po {
namespace {

// Coordinate a 96 DPI.
constexpr int kClientW = 1040, kClientH = 760;
constexpr RECT kListRect{20, 96, 1020, 624};
constexpr int kRowH = 28;

enum : int {
  IDC_LIST = 3000,
  IDC_SEARCH,
  IDC_ONLY_PICKED,
  IDC_LABEL,
  IDC_OK,
  IDC_CANCEL,
  IDC_LHM,
};
enum Col { kColName, kColValue, kColMin, kColMax, kColLabel };
constexpr wchar_t kBrowserClass[] = L"POSensorBrowser";
constexpr UINT_PTR kTimerRefresh = 1;
constexpr UINT kMsgRefresh = WM_APP + 1;

struct RowInfo {
  bool header = false;
  std::string id;  // vuoto per le intestazioni di gruppo
};

struct Browser {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr, list = nullptr;
  HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSection = nullptr;
  HBRUSH brBg = nullptr, brInput = nullptr;
  HIMAGELIST rowHeight = nullptr;
  int dpi = 96;
  std::vector<SensorPick> picks;  // copia di lavoro: va nel profilo solo con OK
  std::vector<SensorEntry> sensors;  // ultimo elenco letto (+ sensori scelti che ora mancano)
  std::vector<RowInfo> rows;         // una per riga della lista
  std::string shownKey;              // id mostrati: se cambiano, la lista viene ricostruita
  bool running = false;              // il monitor pubblica i sensori
  std::string source;
  std::wstring lhmStatus;            // esito dell'installazione di LibreHardwareMonitor
  std::wstring filter;
  bool onlyPicked = false;
  bool rebuilding = false, updatingLabel = false;
  int focusedEdit = 0;
  bool ok = false, done = false;
};
Browser* B = nullptr;

int S(int v) { return MulDiv(v, B->dpi, 96); }
RECT SR(RECT r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }
HWND Item(int id) { return GetDlgItem(B->wnd, id); }

SensorPick* FindPick(const std::string& id) {
  for (auto& p : B->picks)
    if (p.id == id) return &p;
  return nullptr;
}
const SensorEntry* FindEntry(const std::string& id) {
  for (const auto& e : B->sensors)
    if (e.id == id) return &e;
  return nullptr;
}

// Etichetta proposta per l'overlay: il nome del sensore, col prefisso dell'hardware se il nome da solo
// non basta ("Temperatura" di un disco → "Disco 0 Temperatura", "Ventola 1" della GPU → "GPU Ventola 1").
std::string DefaultLabel(const SensorEntry& e) {
  const std::string cat = SensorCategory(e);
  if (e.group.rfind("Disco ", 0) == 0) {
    const auto end = e.group.find(' ', 6);
    return e.group.substr(0, end) + " " + e.name;
  }
  if (e.group.rfind("Rete", 0) == 0) return "NET " + e.name;
  static const std::map<std::string, std::string> kPrefix = {
      {"cpu", "CPU"}, {"gpu", "GPU"}, {"ram", "RAM"}, {"battery", "BAT"}};
  if (const auto it = kPrefix.find(cat); it != kPrefix.end() && !IContains(e.name, it->second))
    return it->second + " " + e.name;
  return e.name;
}

std::wstring ValueText(double v, const std::string& unit) {
  return std::isfinite(v) ? FormatSensorValue(v, unit) : L"—";
}

// ------------------------------------------------------------ contenuto della lista
bool Matches(const SensorEntry& e) {
  if (B->onlyPicked && !FindPick(e.id)) return false;
  if (B->filter.empty()) return true;
  const std::string f = ToUtf8(B->filter);
  return IContains(e.name, f) || IContains(e.group, f) || IContains(e.unit, f);
}

// Sensori da mostrare, raggruppati per hardware nell'ordine in cui arrivano.
std::vector<std::pair<std::string, std::vector<const SensorEntry*>>> Grouped() {
  std::vector<std::pair<std::string, std::vector<const SensorEntry*>>> groups;
  for (const auto& e : B->sensors) {
    if (!Matches(e)) continue;
    auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == e.group; });
    if (it == groups.end()) {
      groups.push_back({e.group, {}});
      it = groups.end() - 1;
    }
    it->second.push_back(&e);
  }
  return groups;
}

void SetText(int row, int col, const std::wstring& t) {
  wchar_t cur[256] = {};
  ListView_GetItemText(B->list, row, col, cur, 256);
  if (t != cur) ListView_SetItemText(B->list, row, col, const_cast<wchar_t*>(t.c_str()));
}

void FillSensorRow(int row, const SensorEntry& e) {
  SetText(row, kColValue, ValueText(e.value, e.unit));
  SetText(row, kColMin, ValueText(e.min, e.unit));
  SetText(row, kColMax, ValueText(e.max, e.unit));
  const SensorPick* p = FindPick(e.id);
  SetText(row, kColLabel, p ? ToWide(p->label) : L"");
}

void Rebuild() {
  B->rebuilding = true;
  SendMessageW(B->list, WM_SETREDRAW, FALSE, 0);
  const int top = ListView_GetTopIndex(B->list);
  const int selRow = ListView_GetNextItem(B->list, -1, LVNI_SELECTED);
  const std::string selId = selRow >= 0 && selRow < int(B->rows.size()) ? B->rows[size_t(selRow)].id : "";
  ListView_DeleteAllItems(B->list);
  B->rows.clear();
  B->shownKey.clear();
  int newSel = -1;
  for (const auto& [group, entries] : Grouped()) {
    LVITEMW it{LVIF_TEXT | LVIF_STATE};
    it.iItem = int(B->rows.size());
    std::wstring g = ToWide(group.empty() ? "Altro" : group);
    it.pszText = g.data();
    it.stateMask = LVIS_STATEIMAGEMASK;
    it.state = INDEXTOSTATEIMAGEMASK(0);  // niente casella sulle intestazioni
    const int hrow = ListView_InsertItem(B->list, &it);
    ListView_SetItemState(B->list, hrow, INDEXTOSTATEIMAGEMASK(0), LVIS_STATEIMAGEMASK);  // niente casella
    B->rows.push_back({true, ""});
    B->shownKey += "|#" + group;
    for (const SensorEntry* e : entries) {
      LVITEMW si{LVIF_TEXT};
      si.iItem = int(B->rows.size());
      std::wstring name = L"    " + ToWide(e->name);
      si.pszText = name.data();
      const int row = ListView_InsertItem(B->list, &si);
      ListView_SetCheckState(B->list, row, FindPick(e->id) != nullptr);
      B->rows.push_back({false, e->id});
      B->shownKey += "|" + e->id;
      FillSensorRow(row, *e);
      if (e->id == selId) newSel = row;
    }
  }
  if (newSel >= 0) ListView_SetItemState(B->list, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  const int count = ListView_GetItemCount(B->list);
  if (top > 0 && count > 0) {  // mantiene la posizione di scorrimento
    ListView_EnsureVisible(B->list, count - 1, FALSE);
    ListView_EnsureVisible(B->list, std::min(top, count - 1), FALSE);
  }
  SendMessageW(B->list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(B->list, nullptr, TRUE);
  B->rebuilding = false;
}

void Refresh(bool forceRebuild = false) {
  const auto feed = ReadSensorFeed();
  B->running = feed && feed->ageMs < 10000;
  B->source = feed ? feed->source : "";
  B->sensors = feed ? feed->sensors : SensorList{};
  // I sensori scelti che ora non ci sono (es. HWiNFO chiuso) restano in lista per poterli togliere.
  for (const auto& p : B->picks)
    if (!FindEntry(p.id)) {
      const double nan = std::numeric_limits<double>::quiet_NaN();
      B->sensors.push_back({p.id, TU("Scelti ma non disponibili ora"), p.group.empty() ? p.label : p.group + " · " + p.label,
                            p.unit, nan, nan, nan});
    }

  std::string key;
  for (const auto& [group, entries] : Grouped()) {
    key += "|#" + group;
    for (const SensorEntry* e : entries) key += "|" + e->id;
  }
  if (forceRebuild || key != B->shownKey) {
    Rebuild();
  } else {
    for (int r = 0; r < int(B->rows.size()); ++r)
      if (!B->rows[size_t(r)].header)
        if (const SensorEntry* e = FindEntry(B->rows[size_t(r)].id)) FillSensorRow(r, *e);
  }
  // Con una fonte di temperature attiva (LibreHardwareMonitor / HWiNFO) il pulsante non serve più e la
  // riga delle fonti usa tutta la larghezza.
  ShowWindow(Item(IDC_LHM), B->source.empty() || !B->lhmStatus.empty() ? SW_SHOW : SW_HIDE);
  const RECT info = SR({20, 36, 690, 84});
  InvalidateRect(B->wnd, &info, FALSE);
}

// ------------------------------------------------------------ selezione ed etichetta
int SelectedRow() { return ListView_GetNextItem(B->list, -1, LVNI_SELECTED); }

void UpdateLabelField() {
  const int r = SelectedRow();
  const bool sensor = r >= 0 && r < int(B->rows.size()) && !B->rows[size_t(r)].header;
  const SensorPick* p = sensor ? FindPick(B->rows[size_t(r)].id) : nullptr;
  B->updatingLabel = true;
  SetWindowTextW(Item(IDC_LABEL), p ? ToWide(p->label).c_str() : L"");
  B->updatingLabel = false;
  EnableWindow(Item(IDC_LABEL), p != nullptr);
  const RECT r2 = SR({20, 636, 1020, 700});
  InvalidateRect(B->wnd, &r2, FALSE);
}

void TogglePick(int row, bool on) {
  if (row < 0 || row >= int(B->rows.size()) || B->rows[size_t(row)].header) return;
  const std::string& id = B->rows[size_t(row)].id;
  const bool has = FindPick(id) != nullptr;
  if (on && !has) {
    const SensorEntry* e = FindEntry(id);
    if (!e) return;
    B->picks.push_back({id, DefaultLabel(*e), e->unit, e->group});
  } else if (!on && has) {
    std::erase_if(B->picks, [&](const SensorPick& p) { return p.id == id; });
  }
  if (const SensorEntry* e = FindEntry(id)) FillSensorRow(row, *e);
  ListView_SetItemState(B->list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  UpdateLabelField();
  const RECT r = SR({20, 700, 780, 752});
  InvalidateRect(B->wnd, &r, FALSE);
  if (B->onlyPicked && !on) PostMessageW(B->wnd, kMsgRefresh, 0, 0);  // non durante la notifica della lista
}

// ------------------------------------------------------------ LibreHardwareMonitor
void OnInstallLhm() {
  if (LhmInstalled()) {
    B->lhmStatus = StartLhm() ? T(L"LibreHardwareMonitor avviato: i sensori compaiono tra qualche secondo.")
                              : T(L"Avvio di LibreHardwareMonitor annullato.");
  } else {
    const int answer = MessageBoxW(
        B->wnd,
        T(L"Su questo PC Windows non espone la temperatura della CPU (e di scheda madre e ventole) senza un driver.\n\n"
        L"PerfOverlay Supreme può scaricare LibreHardwareMonitor (open source, circa 7 MB da GitHub) nella sua "
        L"cartella dati e avviarlo nascosto vicino all'orologio: al primo avvio ti chiederà di installare il suo "
        L"driver dei sensori (PawnIO). Poi l'overlay lo avvia da solo.\n\nProcedere?"),
        kAppName, MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) return;
    B->lhmStatus = T(L"Scaricamento di LibreHardwareMonitor...");
    InvalidateRect(B->wnd, nullptr, FALSE);
    UpdateWindow(B->wnd);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring err;
    if (!InstallLhm(err))
      B->lhmStatus = err;
    else
      B->lhmStatus = StartLhm() ? T(L"Installato e avviato: accetta il driver PawnIO, poi i sensori compaiono qui.")
                                : T(L"Installato. Avvio annullato: riprova con \"Temperatura CPU\".");
  }
  InvalidateRect(B->wnd, nullptr, FALSE);
}

// ------------------------------------------------------------ disegno
void DrawTextAt(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextW(dc, t.c_str(), int(t.size()), &r, fmt | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
}

std::wstring SourcesText() {
  bool windows = false, nvml = false;
  for (const auto& e : B->sensors) {
    windows = windows || e.id.rfind("cpu:", 0) == 0;
    nvml = nvml || e.id.rfind("nv:", 0) == 0;
  }
  std::wstring s = T(L"Fonti:");
  if (windows) s += L"  Windows";
  if (nvml) s += L"  ·  NVIDIA NVML";
  if (!B->source.empty()) s += L"  ·  " + ToWide(B->source);
  int n = 0;
  for (const auto& e : B->sensors) n += std::isfinite(e.value) ? 1 : 0;
  return s + TF(L"   —   {} sensori, aggiornati ogni secondo", n);
}

void Paint(HDC dc, const RECT& client) {
  FillRect(dc, &client, B->brBg);
  SetBkMode(dc, TRANSPARENT);
  DrawTextAt(dc, T(L"SENSORI DI SISTEMA"), SR({20, 8, 600, 36}), B->fontTitle, ui::col::Accent, DT_LEFT | DT_TOP);
  if (!B->running) {
    DrawTextAt(dc, T(L"PerfOverlay Supreme non è in esecuzione: avvialo per leggere i sensori (\"Avvia overlay\")."),
               SR({21, 40, 690, 60}), B->font, ui::col::Bad, DT_LEFT | DT_TOP);
  } else {
    const bool lhmButton = IsWindowVisible(Item(IDC_LHM));
    DrawTextAt(dc, SourcesText(), SR({21, 40, lhmButton ? 490 : 690, 60}), B->font, ui::col::Sub,
               DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
    if (!B->lhmStatus.empty())
      DrawTextAt(dc, B->lhmStatus, SR({21, 60, 490, 80}), B->font, RGB(255, 200, 60), DT_LEFT | DT_TOP);
    else if (B->source.empty())
      DrawTextAt(dc, T(L"Temperatura CPU, scheda madre e ventole: premi \"Temperatura CPU\"."), SR({21, 60, 490, 80}),
                 B->font, RGB(255, 200, 60), DT_LEFT | DT_TOP);
    else
      DrawTextAt(dc, T(L"Spunta un sensore per mostrarlo nell'overlay."), SR({21, 60, 690, 80}), B->font, ui::col::Sub,
                 DT_LEFT | DT_TOP);
  }

  RECT border = SR(kListRect);
  InflateRect(&border, 1, 1);
  ui::FillRound(dc, border, S(2), ui::col::Line, ui::col::Line, 0);

  // Riga del sensore selezionato
  const int r = SelectedRow();
  std::wstring what = T(L"Seleziona un sensore spuntato per cambiare l'etichetta che compare nell'overlay.");
  if (r >= 0 && r < int(B->rows.size()) && !B->rows[size_t(r)].header)
    if (const SensorEntry* e = FindEntry(B->rows[size_t(r)].id))
      what = ToWide(e->group + "  ›  " + e->name) + (FindPick(e->id) ? L"" : T(L"   (non nell'overlay)"));
  DrawTextAt(dc, T(L"Etichetta nell'overlay"), SR({20, 642, 186, 670}), B->font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  DrawTextAt(dc, what, SR({500, 642, 1020, 670}), B->font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  const RECT frame = SR({190, 642, 480, 642 + kRowH});
  ui::FillRound(dc, frame, S(6), ui::col::Input, B->focusedEdit ? ui::col::Accent : ui::col::Line);

  DrawTextAt(dc,
             TF(L"{} {} nell'overlay.  Nel layout libero ognuno è un blocco da spostare nell'Editor; "
                         L"nel preset RTSS compaiono sotto il preset.",
                         B->picks.size(), B->picks.size() == 1 ? T(L"sensore") : T(L"sensori")),
             SR({20, 712, 780, 740}), B->font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  ui::FillRound(dc, SR({690, 4, 1028, 84}), S(8), ui::col::Panel, ui::col::Line);
  const RECT sframe = SR({700, 12, 1020, 12 + kRowH});
  ui::FillRound(dc, sframe, S(6), ui::col::Input, B->focusedEdit == IDC_SEARCH ? ui::col::Accent : ui::col::Line);
}

// ------------------------------------------------------------ notifiche della lista
LRESULT ListCustomDraw(NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const size_t row = cd->nmcd.dwItemSpec;
      if (row < B->rows.size() && B->rows[row].header) {
        cd->clrText = ui::col::Accent;
        SelectObject(cd->nmcd.hdc, B->fontBold);
        return CDRF_NEWFONT;
      }
      return CDRF_NOTIFYSUBITEMDRAW;
    }
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
      const size_t row = cd->nmcd.dwItemSpec;
      const bool picked = row < B->rows.size() && FindPick(B->rows[row].id);
      switch (cd->iSubItem) {
        case kColValue: cd->clrText = ui::col::Text; break;
        case kColMin:
        case kColMax: cd->clrText = ui::col::Sub; break;
        case kColLabel: cd->clrText = ui::col::Accent; break;
        default: cd->clrText = picked ? ui::col::Text : ui::Blend(ui::col::Text, ui::col::Sub, 0.35f); break;
      }
      SelectObject(cd->nmcd.hdc, cd->iSubItem == kColValue || (cd->iSubItem == 0 && picked) ? B->fontBold : B->font);
      return CDRF_NEWFONT;
    }
  }
  return CDRF_DODEFAULT;
}

// Testo chiaro nell'intestazione delle colonne (le sue notifiche arrivano alla lista, non a noi).
LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
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

LRESULT OnListNotify(NMHDR* nm) {
  switch (nm->code) {
    case NM_CUSTOMDRAW:
      return ListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    case LVN_ITEMCHANGING: {
      // Le intestazioni di gruppo non hanno la casella: blocca la barra spaziatrice su di loro.
      const auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
      if (!B->rebuilding && (lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) &&
          lv->iItem >= 0 && lv->iItem < int(B->rows.size()) && B->rows[size_t(lv->iItem)].header)
        return TRUE;
      return FALSE;
    }
    case LVN_ITEMCHANGED: {
      const auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
      if (B->rebuilding || !(lv->uChanged & LVIF_STATE)) return 0;
      if ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) {
        const UINT img = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
        if (img == 1 || img == 2) TogglePick(lv->iItem, img == 2);
      }
      if ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED) UpdateLabelField();
      return 0;
    }
    case NM_DBLCLK: {
      const auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
      if (ia->iItem >= 0 && ia->iItem < int(B->rows.size()) && !B->rows[size_t(ia->iItem)].header)
        ListView_SetCheckState(B->list, ia->iItem, !ListView_GetCheckState(B->list, ia->iItem));
      return 0;
    }
  }
  return 0;
}

// ------------------------------------------------------------ finestra
HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), B->wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), B->inst, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(B->font), TRUE);
  return c;
}

void Build() {
  HWND search = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 709, 18, 302, kRowH - 11, IDC_SEARCH);
  SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(T(L"Cerca (es. temperatura, ventola, disco)")));
  Make(ui::kCheckClass, T(L"Solo quelli nell'overlay"), WS_TABSTOP, 700, 52, 320, 24, IDC_ONLY_PICKED);

  B->list = Make(WC_LISTVIEWW, L"",
                 WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                 kListRect.left, kListRect.top, kListRect.right - kListRect.left, kListRect.bottom - kListRect.top,
                 IDC_LIST);
  ListView_SetExtendedListViewStyle(B->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  ApplyDarkControlTheme(B->list, L"DarkMode_Explorer");
  ApplyDarkControlTheme(ListView_GetHeader(B->list), L"DarkMode_ItemsView");
  ListView_SetBkColor(B->list, ui::col::Panel);
  ListView_SetTextBkColor(B->list, CLR_NONE);
  ListView_SetTextColor(B->list, ui::col::Text);
  B->rowHeight = ImageList_Create(1, S(24), ILC_COLOR32, 1, 0);  // solo per righe più alte
  ListView_SetImageList(B->list, B->rowHeight, LVSIL_SMALL);
  SetWindowSubclass(B->list, ListSubclass, 0, 0);
  const std::pair<const wchar_t*, int> cols[] = {
      {T(L"Sensore"), 420}, {T(L"Valore"), 130}, {T(L"Minimo"), 110}, {T(L"Massimo"), 110}, {T(L"Nell'overlay come"), 200}};
  for (int i = 0; i < int(std::size(cols)); ++i) {
    LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
    c.pszText = const_cast<wchar_t*>(cols[i].first);
    c.cx = S(cols[i].second);
    c.fmt = i == kColName || i == kColLabel ? LVCFMT_LEFT : LVCFMT_RIGHT;
    ListView_InsertColumn(B->list, i, &c);
  }

  ui::MakeButton(Make(L"BUTTON", T(L"Temperatura CPU..."), WS_TABSTOP | BS_OWNERDRAW, 500, 52, 180, 28, IDC_LHM),
                 ui::col::Panel, false);
  Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 199, 648, 272, kRowH - 11, IDC_LABEL);
  SendMessageW(Item(IDC_LABEL), EM_LIMITTEXT, 40, 0);
  ui::MakeButton(Make(L"BUTTON", T(L"Annulla"), WS_TABSTOP | BS_OWNERDRAW, 800, 712, 104, 32, IDC_CANCEL), ui::col::Bg,
                 false);
  ui::MakeButton(Make(L"BUTTON", L"OK", WS_TABSTOP | BS_OWNERDRAW, 916, 712, 104, 32, IDC_OK), ui::col::Bg, true);
  Refresh(true);
  UpdateLabelField();
}

LRESULT CALLBACK BrowserProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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
      BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
             ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_TIMER:
      if (wp == kTimerRefresh) Refresh();
      return 0;
    case kMsgRefresh:
      Refresh();
      return 0;
    case WM_DRAWITEM:
      ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {  // EDIT disattivato = STATIC
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? ui::col::Text : ui::col::Disabled);
      SetBkColor(dc, ui::col::Input);
      return reinterpret_cast<LRESULT>(B->brInput);
    }
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->idFrom == IDC_LIST) return OnListNotify(nm);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      if ((id == IDC_SEARCH || id == IDC_LABEL) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        B->focusedEdit = code == EN_SETFOCUS ? id : 0;
        const RECT r1 = SR({700, 12, 1020, 40}), r2 = SR({190, 642, 480, 670});
        InvalidateRect(h, id == IDC_SEARCH ? &r1 : &r2, FALSE);
        return 0;
      }
      if (id == IDC_SEARCH && code == EN_CHANGE) {
        wchar_t buf[128];
        GetWindowTextW(Item(IDC_SEARCH), buf, 128);
        B->filter = buf;
        Refresh();
        return 0;
      }
      if (id == IDC_LABEL && code == EN_CHANGE && !B->updatingLabel) {
        const int r = SelectedRow();
        if (r >= 0 && r < int(B->rows.size()))
          if (SensorPick* p = FindPick(B->rows[size_t(r)].id)) {
            wchar_t buf[64];
            GetWindowTextW(Item(IDC_LABEL), buf, 64);
            p->label = ToUtf8(buf);
            SetText(r, kColLabel, buf);
          }
        return 0;
      }
      if (code != BN_CLICKED) return 0;
      if (id == IDC_LHM) {
        OnInstallLhm();
      } else if (id == IDC_ONLY_PICKED) {
        B->onlyPicked = Button_GetCheck(Item(IDC_ONLY_PICKED)) == BST_CHECKED;
        Refresh();
      } else if (id == IDC_OK) {
        B->ok = true;
        B->done = true;
      } else if (id == IDC_CANCEL) {
        B->done = true;
      }
      return 0;
    }
    case WM_CLOSE:
      B->done = true;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

HFONT MakeFont(int tenthsOfPoint, int weight) {
  return CreateFontW(-MulDiv(tenthsOfPoint, B->dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

bool RunSensorBrowser(HWND owner, HINSTANCE inst, HFONT uiFont, Profile& profile) {
  Browser br;
  Browser* const previous = B;  // non annidata, ma per sicurezza
  B = &br;
  br.inst = inst;
  br.dpi = int(GetDpiForSystem());
  br.picks = profile.sensors;
  br.font = uiFont;
  br.fontBold = MakeFont(95, FW_SEMIBOLD);
  br.fontTitle = MakeFont(150, FW_BOLD);
  br.fontSection = MakeFont(80, FW_BOLD);
  br.brBg = CreateSolidBrush(ui::col::Bg);
  br.brInput = CreateSolidBrush(ui::col::Input);

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = BrowserProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kBrowserClass;
    RegisterClassExW(&wc);
    registered = true;
  }

  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  RECT r{0, 0, S(kClientW), S(kClientH)};
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
  br.wnd = CreateWindowExW(0, kBrowserClass, (T(L"Sensori di sistema - ") + ToWide(profile.name)).c_str(), style, x, y, w,
                           h, owner, nullptr, inst, nullptr);
  if (!br.wnd) {
    B = previous;
    return false;
  }
  ApplyDarkTitleBar(br.wnd, ui::col::Bg);
  Build();
  SetTimer(br.wnd, kTimerRefresh, 1000, nullptr);
  EnableWindow(owner, FALSE);
  ShowWindow(br.wnd, SW_SHOW);
  SetFocus(br.list);

  MSG msg;
  while (!br.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(br.wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  KillTimer(br.wnd, kTimerRefresh);
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  DestroyWindow(br.wnd);
  if (br.rowHeight) ImageList_Destroy(br.rowHeight);

  if (br.ok) profile.sensors = br.picks;
  for (HGDIOBJ o2 : std::initializer_list<HGDIOBJ>{br.fontBold, br.fontTitle, br.fontSection, br.brBg, br.brInput})
    DeleteObject(o2);
  B = previous;
  return br.ok;
}

}  // namespace po
