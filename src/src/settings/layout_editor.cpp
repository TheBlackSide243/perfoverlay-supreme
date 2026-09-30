#include "settings/layout_editor.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "common/darkmode.h"
#include "common/resources.h"
#include "common/sensor_feed.h"
#include "monitor/rtss_text.h"
#include "common/util.h"
#include "settings/sensor_browser.h"
#include "settings/theme.h"
#include "common/i18n.h"

#pragma comment(lib, "msimg32.lib")  // GradientFill

namespace po {
namespace {

// Coordinate a 96 DPI. La tela del formato scelto viene adattata al riquadro 1200x600.
constexpr int kClientW = 1480, kClientH = 900;
constexpr int kCanvasX = 20, kCanvasY = 84, kCanvasW = 1200, kCanvasH = 740;
constexpr int kPanelX = 1240;  // colonna destra
constexpr int kRowH = 28;

enum : int {
  IDC_CANVAS = 2000,
  IDC_VIS = 2010,  // + indice elemento
  IDC_EX = 2030,
  IDC_EY,
  IDC_ESCALE,
  IDC_SNAP,
  IDC_GRID,
  IDC_RESET,
  IDC_OK,
  IDC_CANCEL,
  IDC_FORMAT,
  IDC_SENSORS,
  IDC_RES,
  IDC_FIELD = 2050,  // + indice campo (max kMaxFieldsPerElement)
};

const wchar_t* const kNames[] = {L"FPS", T(L"Grafico FPS"), L"Frame time", L"CPU", L"GPU", L"RAM", T(L"Batteria")};
constexpr int kElementCount = int(std::size(kNames));
constexpr int kGridSteps[] = {5, 10, 20, 40};
constexpr const wchar_t* kGridNames[] = {L"5 px", L"10 px", L"20 px", L"40 px"};
constexpr wchar_t kCanvasClass[] = L"POLayoutCanvas";
constexpr wchar_t kEditorClass[] = L"POLayoutEditor";

struct Editor {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr, canvas = nullptr;
  HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSection = nullptr;
  HBRUSH brBg = nullptr, brPanel = nullptr, brInput = nullptr;
  int dpi = 96;
  Profile p;  // copia di lavoro: va nel profilo vero solo con OK
  std::string fmt = kDefaultLayoutFormat;  // formato della tela in modifica
  int sel = 0;
  bool snap = true;
  int grid = 10;
  bool dragging = false;
  POINT dragOffset{};                  // pixel della tela tra mouse e angolo del blocco
  float zoom = 1.0f;                   // 1 = tela intera nel riquadro; Ctrl+rotellina per ingrandire
  int scrollX = 0, scrollY = 0;        // spostamento della vista ingrandita (pixel del controllo)
  bool panning = false;
  POINT panFrom{};
  std::vector<RECT> blockRects;        // blocchi sulla tela (pixel del controllo), per indice
  std::map<int, HFONT> fonts;          // font dell'anteprima per dimensione/grassetto
  std::vector<std::pair<int, RECT>> frames;  // cornici disegnate attorno agli EDIT
  int focusedEdit = 0;
  bool updatingFields = false;
  bool ok = false, done = false;
  SensorList feed;  // valori veri per l'anteprima dei blocchi sensore (letti all'apertura)
  // Risoluzione in cui mostrare/inserire X e Y (solo visualizzazione: il layout resta salvato sulla tela 1080).
  struct Res {
    std::wstring label;
    int w = 0, h = 1080;  // w = 0: larghezza della tela del formato scalata con l'altezza
    std::string fmt;      // per il monitor: formato a cui corrisponde la sua larghezza vera
  };
  std::vector<Res> resolutions;
  int res = 0;

  // Modalità preset RTSS: i blocchi sono i testi (layer) del preset importato, posizionati come nell'overlay.
  bool rtss = false;
  std::vector<LayoutItem> rtssItems;  // posizione di ogni layer sulla tela (pixel a 1080p)
  struct RtssGeo {
    std::wstring text;                // testo con valori di esempio
    float w = 0, h = 0;               // riquadro del layer
    float tx = 0, ty = 0;             // posizione del testo nel riquadro (allineamento)
  };
  std::vector<RtssGeo> rtssGeo;
  float boxX = 0, boxY = 0, boxW = 0, boxH = 0, boxPad = 0;  // riquadro di sfondo dell'intero preset
  std::wstring rtssFamily;
  std::map<int, HFONT> rtssFonts;
};
Editor* E = nullptr;

int S(int v) { return MulDiv(v, E->dpi, 96); }
RECT SR(RECT r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }
HWND Item(int id) { return GetDlgItem(E->wnd, id); }
std::vector<LayoutItem>& Items() { return E->rtss ? E->rtssItems : E->p.layouts[E->fmt]; }
int RefW() { return LayoutFormatWidth(E->fmt); }
// Pixel del controllo per pixel della tela con la tela intera nel riquadro, e con lo zoom.
float FitScale() { return std::min(float(S(kCanvasW)) / float(RefW()), float(S(kCanvasH)) / float(kLayoutRefH)); }
float CanvasScale() { return FitScale() * E->zoom; }
// Rettangolo della tela nella finestra, centrato nel riquadro (la vista, indipendente dallo zoom).
RECT CanvasRect() {
  const int w = int(std::lround(RefW() * FitScale())), h = int(std::lround(kLayoutRefH * FitScale()));
  const int x = S(kCanvasX) + (S(kCanvasW) - w) / 2, y = S(kCanvasY);
  return {x, y, x + w, y + h};
}
COLORREF Cr(Color c) { return RGB(c.r, c.g, c.b); }

// Pixel della risoluzione scelta per pixel della tela (la tela è alta 1080).
int ResH() { return E->resolutions.empty() ? kLayoutRefH : E->resolutions[size_t(E->res)].h; }
int ResW() {
  if (E->resolutions.empty()) return RefW();
  const auto& r = E->resolutions[size_t(E->res)];
  // Il monitor ha la sua larghezza vera solo sul formato che gli corrisponde.
  return r.w > 0 && r.fmt == E->fmt ? r.w : int(std::lround(double(RefW()) * r.h / kLayoutRefH));
}
int DispX(int x) { return int(std::lround(double(x) * ResW() / RefW())); }
int DispY(int y) { return int(std::lround(double(y) * ResH() / kLayoutRefH)); }
int RefX(int d) { return int(std::lround(double(d) * RefW() / ResW())); }
int RefY(int d) { return int(std::lround(double(d) * kLayoutRefH / ResH())); }

const SensorPick* PickOf(const std::string& el) {
  if (!IsSensorElement(el)) return nullptr;
  const std::string id = el.substr(kSensorElementPrefix.size());
  for (const auto& sp : E->p.sensors)
    if (sp.id == id) return &sp;
  return nullptr;
}

// Nome del blocco i: gli elementi fissi, poi i sensori scelti (con la loro etichetta).
std::wstring ElementName(int i) {
  if (E->rtss) {
    if (i < 0 || i >= int(E->p.rtss.layers.size())) return L"";
    std::wstring t = ToWide(E->p.rtss.layers[size_t(i)].text);
    std::replace(t.begin(), t.end(), L'\n', L' ');
    if (t.size() > 18) t = t.substr(0, 17) + L"…";
    return TF(L"Testo {}: {}", i + 1, t);
  }
  if (i >= 0 && i < kElementCount) return kNames[i];
  if (i < 0 || i >= int(Items().size())) return L"";
  const SensorPick* sp = PickOf(Items()[size_t(i)].element);
  return sp ? T(L"Sensore: ") + ToWide(sp->label.empty() ? sp->id : sp->label) : T(L"Sensore");
}

bool& Visible(Profile& p, const std::string& el) {
  if (el == "fps") return p.show.fps;
  if (el == "graph") return p.show.graph;
  if (el == "frametime") return p.show.frametime;
  if (el == "cpu") return p.show.cpu;
  if (el == "gpu") return p.show.gpu;
  if (el == "ram") return p.show.ram;
  return p.show.battery;
}

// ------------------------------------------------------------ campi degli elementi
bool FieldOn(const Profile& p, const std::string& el, std::string_view key) {
  if (el == "frametime" && (key == "p95" || key == "p99")) return p.show.percentiles && p.Field(el, key);
  if (el == "frametime" && key == "stutter") return p.show.stutter && p.Field(el, key);
  return p.Field(el, key);
}

void SetFieldOn(Profile& p, const std::string& el, std::string_view key, bool on) {
  p.SetField(el, key, on);
  if (on && el == "frametime" && (key == "p95" || key == "p99")) p.show.percentiles = true;
  if (on && el == "frametime" && key == "stutter") p.show.stutter = true;
}

std::vector<const LayoutField*> FieldsOf(const std::string& el) {
  std::vector<const LayoutField*> v;
  for (const auto& f : kLayoutFields)
    if (el == f.element) v.push_back(&f);
  return v;
}

// ------------------------------------------------------------ anteprima dei blocchi
struct Piece {
  std::wstring text;
  COLORREF color;
  float scale = 1.0f;
  bool bold = false;
};

std::vector<Piece> Sample(const Profile& p, const std::string& el) {
  if (const SensorPick* sp = PickOf(el)) {  // blocco sensore: etichetta e valore vero, se disponibile
    SensorEntry probe;
    probe.id = sp->id;
    probe.group = sp->group;
    const std::string cat = SensorCategory(probe);
    const Color lc = cat == "cpu"       ? p.colors.cpu
                     : cat == "gpu"     ? p.colors.gpu
                     : cat == "ram"     ? p.colors.ram
                     : cat == "battery" ? p.colors.battery
                                        : p.colors.text;
    const COLORREF vc = p.style == "neon" ? Cr(lc) : Cr(p.colors.text);
    std::wstring value = L"—";
    for (const auto& e : E->feed)
      if (e.id == sp->id) value = FormatSensorValue(e.value, e.unit, false);
    return {{ToWide(sp->label.empty() ? sp->id : sp->label) + L" ", Cr(lc), 0.8f, true},
            {value, vc},
            {sp->unit.empty() ? L"" : ToWide(sp->unit), vc, 0.72f}};
  }
  const bool neon = p.style == "neon";
  const auto val = [&](Color element) { return neon ? Cr(element) : Cr(p.colors.text); };
  auto label = [&](const wchar_t* t2, Color c) { return Piece{t2, Cr(c), 0.8f, true}; };
  auto on = [&](const char* key) { return FieldOn(p, el, key); };
  std::vector<Piece> v;
  auto add = [&](std::initializer_list<Piece> pieces) { v.insert(v.end(), pieces); };
  if (el == "fps") {
    const COLORREF c = val(p.colors.fps);
    add({label(L"FPS ", p.colors.fps)});
    if (on("value")) add({{L"144", c, 1.0f, true}});
    if (on("minmax")) add({{L" ↓138 ↑151", c, 0.7f}});
    if (on("low1")) add({label(L"  1%L ", p.colors.fps), {L"121", c, 0.85f}});
  } else if (el == "frametime") {
    const COLORREF c = val(p.colors.fps);
    if (on("value")) add({{L"6.9", c}, {L"ms", c, 0.72f}});
    if (on("p95")) add({label(v.empty() ? L"P95 " : L"  P95 ", p.colors.fps), {L"7.4", c}});
    if (on("p99")) add({label(v.empty() ? L"P99 " : L"  P99 ", p.colors.fps), {L"9.1", c}});
    if (on("p95") || on("p99")) add({{L"ms", c, 0.72f}});
    if (on("stutter")) add({label(v.empty() ? L"ST " : L"  ST ", p.colors.fps), {L"0.4%", c}});
    if (v.empty()) add({{T(L"(nessun campo)"), RGB(150, 150, 162), 0.8f}});
  } else if (el == "cpu") {
    const COLORREF c = val(p.colors.cpu);
    add({label(L"CPU ", p.colors.cpu)});
    if (on("usage")) add({{L"32%", c}});
    if (on("topcore")) add({{on("usage") ? L"/↑" : L"↑", c, 0.72f}, {L"71%", c}});
    if (on("temp")) add({{L" 68", c}, {L"°C", c, 0.72f}});
    if (on("freq")) add({{L" 5.19", c}});
    if (on("maxfreq")) add({{on("freq") ? L"/↑5.23" : L" ↑5.23", c, on("freq") ? 0.85f : 1.0f}});
    if (on("freq") || on("maxfreq")) add({{L"GHz", c, 0.72f}});
  } else if (el == "gpu") {
    const COLORREF c = val(p.colors.gpu);
    add({label(L"GPU ", p.colors.gpu)});
    if (on("usage")) add({{L"97%", c}});
    if (on("temp")) add({{L" 71", c}, {L"°C", c, 0.72f}});
    if (on("clock")) add({{L" 2610", c}, {L"MHz", c, 0.72f}});
    if (on("vram")) add({{L" 7.9/9.8", c}, {L"GB", c, 0.72f}});
  } else if (el == "ram") {
    const COLORREF c = val(p.colors.ram);
    add({label(L"RAM ", p.colors.ram)});
    if (on("used")) add({{L"16.8/31.1", c}, {L"GB", c, 0.72f}});
    if (on("speed")) add({{on("used") ? L" 4800" : L"4800", c}, {L"MT/s", c, 0.72f}});
  } else if (el == "battery") {
    const COLORREF c = val(p.colors.battery);
    add({label(L"BAT ", p.colors.battery)});
    if (on("percent")) add({{L"85%", c}});
    if (on("state")) add({{T(L" ▼ a batteria"), c, 0.72f}});
  }
  return v;
}

HFONT PreviewFont(float px, bool bold) {
  const int key = int(std::lround(px * 4)) * 2 + (bold ? 1 : 0);
  if (auto it = E->fonts.find(key); it != E->fonts.end()) return it->second;
  const std::string& f = E->p.fontFamily;
  const std::wstring family = IEquals(f, "mono") ? L"Consolas" : IEquals(f, "sans") ? L"Segoe UI" : ToWide(f);
  HFONT h = CreateFontW(-std::max(1, int(std::lround(px))), 0, 0, 0, bold || E->p.bold ? FW_BOLD : FW_NORMAL, FALSE,
                        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, family.c_str());
  E->fonts[key] = h;
  return h;
}

// Misura o disegna una riga di pezzi allineati sulla linea di base. Restituisce la dimensione.
SIZE Pieces(HDC dc, const std::vector<Piece>& pieces, float base, int x, int y, bool draw) {
  int ascent = 0, descent = 0, width = 0;
  for (const auto& pc : pieces) {
    SelectObject(dc, PreviewFont(base * pc.scale, pc.bold));
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    ascent = std::max(ascent, int(tm.tmAscent));
    descent = std::max(descent, int(tm.tmDescent));
  }
  for (const auto& pc : pieces) {
    SelectObject(dc, PreviewFont(base * pc.scale, pc.bold));
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE sz;
    GetTextExtentPoint32W(dc, pc.text.c_str(), int(pc.text.size()), &sz);
    if (draw) {
      SetTextColor(dc, pc.color);
      TextOutW(dc, x + width, y + ascent - tm.tmAscent, pc.text.c_str(), int(pc.text.size()));
    }
    width += sz.cx;
  }
  return {width, ascent + descent};
}

// ------------------------------------------------------------ preset RTSS
HFONT RtssFont(float px, bool bold) {
  const int key = int(std::lround(px * 4)) * 2 + (bold ? 1 : 0);
  if (auto it = E->rtssFonts.find(key); it != E->rtssFonts.end()) return it->second;
  HFONT h = CreateFontW(-std::max(1, int(std::lround(px))), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                        E->rtssFamily.c_str());
  E->rtssFonts[key] = h;
  return h;
}
bool RtssBold() { return E->p.bold || E->p.rtss.fontWeight >= 600; }

// Font del preset se installato (es. Unispace), altrimenti Consolas: come l'overlay.
std::wstring ResolveRtssFamily(const std::string& face) {
  const std::wstring want = ToWide(face);
  if (want.empty()) return L"Consolas";
  HDC dc = GetDC(nullptr);
  LOGFONTW lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(lf.lfFaceName, want.c_str(), _TRUNCATE);
  bool found = false;
  EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM l) -> int {
    *reinterpret_cast<bool*>(l) = true;
    return 0;
  }, reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found ? want : L"Consolas";
}

// Valori di esempio per l'anteprima dei testi del preset.
std::wstring RtssSampleText(const RtssLayer& l) {
  static const auto kSnap = [] {
    SystemSnapshot s;
    s.ready = true;
    s.cpu.usage = 32;
    s.cpu.freqGHz = 5.19;
    s.cpuTempC = 68;
    s.cpuPowerW = 54;
    s.gpu.usage = 97;
    s.gpu.tempC = 71;
    s.gpu.clockMHz = 2610;
    s.gpu.powerW = 212;
    s.gpu.vramUsedGB = 7.9;
    s.gpu.vramTotalGB = 9.8;
    s.ram.usedGB = 16.8;
    s.ram.totalGB = 31.1;
    s.battery.present = true;
    s.battery.percent = 85;
    s.battery.rateW = 18;
    s.battery.remainMin = 123;
    return s;
  }();
  static const auto kFrames = [] {
    FrameStats f;
    f.valid = true;
    f.fps = 144;
    f.frameTimeMs = 6.9;
    return f;
  }();
  return ExpandRtssText(l.text, E->p.rtss, kSnap, &kFrames);
}

// Ricalcola dimensioni e posizione di ogni layer sulla tela (pixel a 1080p, scala 1).
void LayoutRtss() {
  const RtssLayout& preset = E->p.rtss;
  const Profile& p = E->p;
  const float base = p.fontSize;
  const bool bold = RtssBold();
  HDC dc = CreateCompatibleDC(nullptr);
  SelectObject(dc, RtssFont(base, bold));
  SIZE zero{};
  GetTextExtentPoint32W(dc, L"0", 1, &zero);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  const float cellW = float(zero.cx), cellH = float(tm.tmHeight + tm.tmExternalLeading);
  auto unitX = [&](int v) { return v < 0 ? -v * cellW : float(v); };
  auto unitY = [&](int v) { return v < 0 ? -v * cellH : float(v); };

  const size_t n = preset.layers.size();
  E->rtssGeo.assign(n, {});
  std::vector<std::pair<float, float>> pos(n);
  float maxX = 0, maxY = 0;
  for (size_t i = 0; i < n; ++i) {
    const RtssLayer& l = preset.layers[i];
    auto& g = E->rtssGeo[i];
    g.text = RtssSampleText(l);
    SelectObject(dc, RtssFont(base * std::clamp(l.size, 10, 1000) / 100.0f, bold));
    RECT tr{0, 0, 0, 0};
    if (!g.text.empty()) DrawTextW(dc, g.text.c_str(), int(g.text.size()), &tr, DT_CALCRECT | DT_NOPREFIX);
    const float tw = float(tr.right), th = float(tr.bottom);
    const float px = unitX(l.x), py = unitY(l.y);
    const float bw = l.extentX ? unitX(l.extentX) : tw, bh = l.extentY ? unitY(l.extentY) : th;
    const int origin = std::clamp(l.origin, 0, 8), col = origin % 3, row = origin / 3;
    g.tx = col == 0 ? 0 : col == 1 ? (bw - tw) / 2 : bw - tw;
    g.ty = row == 0 ? 0 : row == 1 ? (bh - th) / 2 : bh - th;
    g.w = g.text.empty() ? 0 : std::max(bw, 1.0f);
    g.h = g.text.empty() ? 0 : std::max(bh, 1.0f);
    pos[i] = {px, py};
    if (!g.text.empty()) {
      maxX = std::max({maxX, px + bw, px + g.tx + tw});
      maxY = std::max({maxY, py + bh, py + g.ty + th});
    }
  }
  DeleteDC(dc);

  // Riquadro del preset posizionato come nell'overlay (angolo scelto nel profilo, margine).
  E->boxPad = base * 0.4f;
  E->boxW = maxX + 2 * E->boxPad;
  E->boxH = maxY + 2 * E->boxPad;
  const float m = float(p.margin), W = float(RefW()), H = float(kLayoutRefH);
  E->boxX = m;
  E->boxY = m;
  if (p.position == "top-right" || p.position == "bottom-right") E->boxX = W - E->boxW - m;
  if (p.position == "bottom-left" || p.position == "bottom-right") E->boxY = H - E->boxH - m;
  if (p.position == "custom") {
    E->boxX = float(p.x);
    E->boxY = float(p.y);
  }
  E->boxX = std::clamp(E->boxX, 0.0f, std::max(0.0f, W - E->boxW));
  E->boxY = std::clamp(E->boxY, 0.0f, std::max(0.0f, H - E->boxH));

  E->rtssItems.resize(n);
  for (size_t i = 0; i < n; ++i)
    E->rtssItems[i] = {"rtss:" + std::to_string(i), int(std::lround(E->boxX + E->boxPad + pos[i].first)),
                       int(std::lround(E->boxY + E->boxPad + pos[i].second)), preset.layers[i].size};
}

// Riporta nel preset la posizione/dimensione del blocco i (le posizioni diventano in pixel).
void SyncRtssLayer(int i) {
  if (!E->rtss || i < 0 || i >= int(E->p.rtss.layers.size())) return;
  RtssLayer& l = E->p.rtss.layers[size_t(i)];
  const LayoutItem& it = E->rtssItems[size_t(i)];
  l.x = std::max(0, int(std::lround(it.x - E->boxX - E->boxPad)));
  l.y = std::max(0, int(std::lround(it.y - E->boxY - E->boxPad)));
  l.size = it.scale;
  LayoutRtss();
}

// Rettangolo del blocco i sulla tela (pixel del controllo); vuoto se l'elemento è nascosto.
RECT BlockRect(HDC dc, int i) {
  const LayoutItem& it = Items()[size_t(i)];
  if (E->rtss) {
    const auto& g = E->rtssGeo[size_t(i)];
    if (g.w <= 0) return {};
    const float cs = CanvasScale();
    const int x = int(std::lround(it.x * cs)) - E->scrollX, y = int(std::lround(it.y * cs)) - E->scrollY;
    return {x, y, x + std::max(2, int(std::lround(g.w * cs))), y + std::max(2, int(std::lround(g.h * cs)))};
  }
  if (!IsSensorElement(it.element) && !Visible(E->p, it.element)) return {};
  const float cs = CanvasScale();
  const float base = E->p.fontSize * it.scale / 100.0f * cs;
  const int pad = int(std::lround(base * 0.35f));
  SIZE content;
  if (it.element == "graph")
    content = {LONG(base * 1.6f * 3.6f), LONG(base * 1.6f * 0.9f)};
  else
    content = Pieces(dc, Sample(E->p, it.element), base, 0, 0, false);
  const int x = int(std::lround(it.x * cs)) - E->scrollX, y = int(std::lround(it.y * cs)) - E->scrollY;
  return {x, y, x + content.cx + 2 * pad, y + content.cy + 2 * pad};
}

void DrawRtssBlock(HDC dc, int i, const RECT& r) {
  const RtssLayer& l = E->p.rtss.layers[size_t(i)];
  const auto& g = E->rtssGeo[size_t(i)];
  const float cs = CanvasScale();
  SelectObject(dc, RtssFont(E->p.fontSize * std::clamp(l.size, 10, 1000) / 100.0f * cs, RtssBold()));
  SetBkMode(dc, TRANSPARENT);
  RECT tr{r.left + int(std::lround(g.tx * cs)), r.top + int(std::lround(g.ty * cs)), 0, 0};
  tr.right = tr.left + 10000;
  tr.bottom = tr.top + 10000;
  RECT shadow = tr;  // ombra come l'OSD di RTSS
  OffsetRect(&shadow, std::max(1, int(cs)), std::max(1, int(cs)));
  SetTextColor(dc, RGB(0, 0, 0));
  DrawTextW(dc, g.text.c_str(), int(g.text.size()), &shadow, DT_NOPREFIX | DT_NOCLIP);
  SetTextColor(dc, Cr(l.color));
  DrawTextW(dc, g.text.c_str(), int(g.text.size()), &tr, DT_NOPREFIX | DT_NOCLIP);
}

void DrawBlock(HDC dc, int i, const RECT& r) {
  if (E->rtss) {
    DrawRtssBlock(dc, i, r);
    return;
  }
  const LayoutItem& it = Items()[size_t(i)];
  const Profile& p = E->p;
  const float cs = CanvasScale();
  const float base = p.fontSize * it.scale / 100.0f * cs;
  const int pad = int(std::lround(base * 0.35f));
  const int radius = std::max(2, int(base * 0.3f));
  if (p.style != "minimal" && p.bgOpacity > 0)  // sfondo semitrasparente simulato sul colore della tela
    ui::FillRound(dc, r, radius, ui::Blend(RGB(46, 34, 44), Cr(p.colors.background), p.bgOpacity / 100.0f),
                  ui::Blend(RGB(46, 34, 44), Cr(p.colors.background), p.bgOpacity / 100.0f), 0);
  if (p.style == "neon") ui::StrokeRound(dc, r, radius, Cr(p.colors.fps), std::max(1.0f, cs * 3));
  SetBkMode(dc, TRANSPARENT);
  if (it.element == "graph") {
    // Curva FPS di esempio
    const int gx = r.left + pad, gy = r.top + pad, gw = r.right - r.left - 2 * pad, gh = r.bottom - r.top - 2 * pad;
    HPEN base0 = CreatePen(PS_SOLID, 1, ui::Blend(RGB(46, 34, 44), Cr(p.colors.text), 0.25f));
    HGDIOBJ old = SelectObject(dc, base0);
    MoveToEx(dc, gx, gy + gh, nullptr);
    LineTo(dc, gx + gw, gy + gh);
    HPEN line = CreatePen(PS_SOLID, std::max(1, int(cs * 2)), Cr(p.colors.fps));
    SelectObject(dc, line);
    static const float kWave[] = {0.62f, 0.66f, 0.58f, 0.70f, 0.64f, 0.35f, 0.60f, 0.66f, 0.68f, 0.63f, 0.57f, 0.65f,
                                  0.69f, 0.66f, 0.60f, 0.64f};
    for (int k = 0; k < int(std::size(kWave)); ++k) {
      const int px = gx + gw * k / int(std::size(kWave) - 1), py = gy + int(gh * (1.0f - kWave[k]));
      k == 0 ? MoveToEx(dc, px, py, nullptr) : LineTo(dc, px, py);
    }
    SelectObject(dc, old);
    DeleteObject(line);
    DeleteObject(base0);
  } else {
    if (p.style == "minimal") {  // ombra, come nell'overlay
      auto shadow = Sample(p, it.element);
      for (auto& pc : shadow) pc.color = RGB(0, 0, 0);
      Pieces(dc, shadow, base, r.left + pad + 1, r.top + pad + 1, true);
    }
    Pieces(dc, Sample(p, it.element), base, r.left + pad, r.top + pad, true);
  }
}

void PaintCanvas(HDC dc, const RECT& rc) {
  // Sfondo "di gioco" sfumato
  TRIVERTEX v[2] = {{rc.left, rc.top, 0x1800, 0x1E00, 0x3400, 0xFF00},
                    {rc.right, rc.bottom, 0x5C00, 0x2C00, 0x1800, 0xFF00}};
  GRADIENT_RECT gr{0, 1};
  GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);

  // Griglia in pixel della tela: linee sottili a ogni passo, più marcate ogni 5 passi.
  const float cs = CanvasScale();
  const float stepPx = E->grid * cs;
  {
    const bool showMinor = stepPx >= 6.0f;  // linee fini solo se non troppo fitte
    HPEN minor = CreatePen(PS_SOLID, 1, RGB(62, 54, 72));
    HPEN major = CreatePen(PS_SOLID, 1, RGB(104, 92, 118));
    for (int gx = 0, n = 0; gx <= RefW(); gx += E->grid, ++n) {
      if (n % 5 != 0 && !showMinor) continue;
      SelectObject(dc, n % 5 == 0 ? major : minor);
      const int x = int(std::lround(gx * cs)) - E->scrollX;
      MoveToEx(dc, x, 0, nullptr);
      LineTo(dc, x, rc.bottom);
    }
    for (int gy = 0, n = 0; gy <= kLayoutRefH; gy += E->grid, ++n) {
      if (n % 5 != 0 && !showMinor) continue;
      SelectObject(dc, n % 5 == 0 ? major : minor);
      const int y = int(std::lround(gy * cs)) - E->scrollY;
      MoveToEx(dc, 0, y, nullptr);
      LineTo(dc, rc.right, y);
    }
    SelectObject(dc, GetStockObject(BLACK_PEN));
    DeleteObject(minor);
    DeleteObject(major);
  }
  // Assi centrali
  HPEN center = CreatePen(PS_DOT, 1, ui::Blend(RGB(92, 80, 104), ui::col::Accent, 0.6f));
  SelectObject(dc, center);
  SetBkMode(dc, TRANSPARENT);
  const int midX = int(std::lround(RefW() * cs / 2)) - E->scrollX, midY = int(std::lround(kLayoutRefH * cs / 2)) - E->scrollY;
  MoveToEx(dc, midX, 0, nullptr);
  LineTo(dc, midX, rc.bottom);
  MoveToEx(dc, 0, midY, nullptr);
  LineTo(dc, rc.right, midY);
  SelectObject(dc, GetStockObject(BLACK_PEN));
  DeleteObject(center);

  if (E->rtss) {  // sfondo dell'intero preset, come nell'overlay
    LayoutRtss();
    const RECT box{int(std::lround(E->boxX * cs)) - E->scrollX, int(std::lround(E->boxY * cs)) - E->scrollY,
                   int(std::lround((E->boxX + E->boxW) * cs)) - E->scrollX,
                   int(std::lround((E->boxY + E->boxH) * cs)) - E->scrollY};
    if (E->p.bgOpacity > 0) {
      const COLORREF bg = ui::Blend(RGB(46, 34, 44), Cr(E->p.colors.background), E->p.bgOpacity / 100.0f);
      ui::FillRound(dc, box, std::max(2, int(E->p.fontSize * 0.3f * cs)), bg, bg, 0);
    }
  }
  E->blockRects.assign(Items().size(), RECT{});
  for (int i = 0; i < int(Items().size()); ++i) {
    E->blockRects[size_t(i)] = BlockRect(dc, i);
    if (!IsRectEmpty(&E->blockRects[size_t(i)])) DrawBlock(dc, i, E->blockRects[size_t(i)]);
  }

  // Blocco selezionato: bordo d'accento + etichetta con coordinate.
  if (E->sel >= 0 && E->sel < int(E->blockRects.size()) && !IsRectEmpty(&E->blockRects[size_t(E->sel)])) {
    RECT r = E->blockRects[size_t(E->sel)];
    InflateRect(&r, S(3), S(3));
    const bool focus = GetFocus() == E->canvas;
    ui::StrokeRound(dc, r, S(5), focus ? ui::col::Accent : ui::col::AccentHover, 2.0f);
    const LayoutItem& it = Items()[size_t(E->sel)];
    const std::wstring tag =
        std::format(L" {}   x {}  y {}   {}% ", ElementName(E->sel), DispX(it.x), DispY(it.y), it.scale);
    SelectObject(dc, E->fontBold);
    SIZE sz;
    GetTextExtentPoint32W(dc, tag.c_str(), int(tag.size()), &sz);
    const int tw = sz.cx + S(6), th = sz.cy + S(4);
    const int ty = std::clamp(int((r.top + r.bottom - th) / 2), 0, int(rc.bottom) - th);
    RECT t{r.right + S(6), ty, r.right + S(6) + tw, ty + th};
    if (t.right > rc.right) t = {r.left - S(6) - tw, ty, r.left - S(6), ty + th};
    ui::FillRound(dc, t, S(4), ui::col::Accent, ui::col::Accent, 0);
    SetTextColor(dc, ui::col::AccentText);
    DrawTextW(dc, tag.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
}

// ------------------------------------------------------------ modifica dei blocchi
void UpdateFieldChecks() {
  const std::string& el = Items()[size_t(E->sel)].element;
  const auto fields = FieldsOf(el);
  for (int i = 0; i < kMaxFieldsPerElement; ++i) {
    HWND c = Item(IDC_FIELD + i);
    if (i < int(fields.size())) {
      SetWindowTextW(c, T(fields[size_t(i)]->label));
      Button_SetCheck(c, FieldOn(E->p, el, fields[size_t(i)]->key) ? BST_CHECKED : BST_UNCHECKED);
      ShowWindow(c, SW_SHOWNA);
    } else {
      ShowWindow(c, SW_HIDE);
    }
  }
  const RECT r = SR({1240, 528, 1460, 676});
  InvalidateRect(E->wnd, &r, FALSE);
}

void UpdateFields() {
  E->updatingFields = true;
  const LayoutItem& it = Items()[size_t(E->sel)];
  SetWindowTextW(Item(IDC_EX), std::to_wstring(DispX(it.x)).c_str());
  SetWindowTextW(Item(IDC_EY), std::to_wstring(DispY(it.y)).c_str());
  SetWindowTextW(Item(IDC_ESCALE), std::to_wstring(it.scale).c_str());
  E->updatingFields = false;
  UpdateFieldChecks();
  const RECT r = SR({1240, 340, 1460, 380});  // nome del blocco selezionato
  InvalidateRect(E->wnd, &r, FALSE);
  const RECT l = SR({1240, 84, 1254, 300});   // indicatore nella lista elementi
  InvalidateRect(E->wnd, &l, FALSE);
}

void Refresh() { InvalidateRect(E->canvas, nullptr, FALSE); }

// Sposta il blocco selezionato (coordinate tela), con snap opzionale e limiti della tela.
void MoveSelected(int x, int y, bool snap) {
  LayoutItem& it = Items()[size_t(E->sel)];
  if (snap) {
    x = int(std::lround(double(x) / E->grid)) * E->grid;
    y = int(std::lround(double(y) / E->grid)) * E->grid;
  }
  const RECT& r = E->blockRects.size() > size_t(E->sel) ? E->blockRects[size_t(E->sel)] : RECT{};
  const float cs = CanvasScale();
  const int wRef = int((r.right - r.left) / cs), hRef = int((r.bottom - r.top) / cs);
  it.x = std::clamp(x, 0, std::max(0, RefW() - wRef));
  it.y = std::clamp(y, 0, std::max(0, kLayoutRefH - hRef));
  SyncRtssLayer(E->sel);
  UpdateFields();
  Refresh();
}

// ------------------------------------------------------------ zoom e vista
void ClampScroll() {
  RECT rc;
  GetClientRect(E->canvas, &rc);
  const float cs = CanvasScale();
  E->scrollX = std::clamp(E->scrollX, 0, std::max(0, int(std::lround(RefW() * cs)) - int(rc.right)));
  E->scrollY = std::clamp(E->scrollY, 0, std::max(0, int(std::lround(kLayoutRefH * cs)) - int(rc.bottom)));
}

void InvalidateZoomLabel() {
  const RECT r = SR({24, 62, 1010, 80});
  InvalidateRect(E->wnd, &r, FALSE);
}

// Zoom mantenendo fermo il punto della tela sotto "at" (pixel del controllo).
void SetZoom(float z, POINT at) {
  z = std::clamp(z, 1.0f, 6.0f);
  const float before = CanvasScale();
  const float px = (at.x + E->scrollX) / before, py = (at.y + E->scrollY) / before;
  E->zoom = z;
  const float after = CanvasScale();
  E->scrollX = int(std::lround(px * after - at.x));
  E->scrollY = int(std::lround(py * after - at.y));
  ClampScroll();
  InvalidateRect(E->canvas, nullptr, FALSE);
  InvalidateZoomLabel();
}

// Con lo zoom, porta in vista il blocco selezionato se è fuori dal riquadro.
void RevealSelected() {
  if (E->zoom <= 1.0f) return;
  RECT rc;
  GetClientRect(E->canvas, &rc);
  const LayoutItem& it = Items()[size_t(E->sel)];
  const float cs = CanvasScale();
  const int x = int(it.x * cs) - E->scrollX, y = int(it.y * cs) - E->scrollY;
  if (x < 0 || x > rc.right - S(40)) E->scrollX = int(it.x * cs) - rc.right / 4;
  if (y < 0 || y > rc.bottom - S(30)) E->scrollY = int(it.y * cs) - rc.bottom / 4;
  ClampScroll();
}

void Select(int i) {
  E->sel = std::clamp(i, 0, int(Items().size()) - 1);
  RevealSelected();
  UpdateFields();
  Refresh();
}

bool BlockVisible(int i) {
  if (E->rtss) return E->rtssGeo.size() > size_t(i) && E->rtssGeo[size_t(i)].w > 0;
  const std::string& el = Items()[size_t(i)].element;
  return IsSensorElement(el) || Visible(E->p, el);
}

// Blocco visibile successivo/precedente (a giro).
void SelectNext(int dir) {
  const int n = int(Items().size());
  for (int k = 1; k <= n; ++k) {
    const int i = ((E->sel + dir * k) % n + n) % n;
    if (BlockVisible(i)) {
      Select(i);
      return;
    }
  }
}

LRESULT CALLBACK CanvasProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      PaintCanvas(mem, rc);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      Refresh();
      return 0;
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(h, &pt);
      const int delta = GET_WHEEL_DELTA_WPARAM(wp);
      if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
        SetZoom(E->zoom * (delta > 0 ? 1.25f : 0.8f), pt);
      } else if (E->zoom > 1.0f) {  // rotellina = su/giù, Shift+rotellina = destra/sinistra
        (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT ? E->scrollX : E->scrollY) -= delta * S(60) / WHEEL_DELTA;
        ClampScroll();
        Refresh();
      }
      return 0;
    }
    case WM_MOUSEHWHEEL:
      if (E->zoom > 1.0f) {
        E->scrollX += GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA;
        ClampScroll();
        Refresh();
      }
      return 0;
    case WM_MBUTTONDOWN:  // tasto centrale: trascina la vista
      SetFocus(h);
      E->panning = true;
      E->panFrom = {GET_X_LPARAM(lp) + E->scrollX, GET_Y_LPARAM(lp) + E->scrollY};
      SetCapture(h);
      return 0;
    case WM_MBUTTONUP:
      if (E->panning) {
        E->panning = false;
        ReleaseCapture();
      }
      return 0;
    case WM_LBUTTONDOWN: {
      SetFocus(h);
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      for (int i = int(E->blockRects.size()) - 1; i >= 0; --i) {  // il blocco disegnato per ultimo sta sopra
        if (PtInRect(&E->blockRects[size_t(i)], pt)) {
          Select(i);
          E->dragging = true;
          E->dragOffset = {pt.x - E->blockRects[size_t(i)].left, pt.y - E->blockRects[size_t(i)].top};
          SetCapture(h);
          return 0;
        }
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (E->panning) {
        E->scrollX = E->panFrom.x - pt.x;
        E->scrollY = E->panFrom.y - pt.y;
        ClampScroll();
        Refresh();
        return 0;
      }
      if (E->dragging) {
        const float cs = CanvasScale();
        // Alt durante il trascinamento: niente snap (posizionamento al pixel)
        const bool snap = E->snap && GetKeyState(VK_MENU) >= 0;
        MoveSelected(int(std::lround((pt.x + E->scrollX - E->dragOffset.x) / cs)),
                     int(std::lround((pt.y + E->scrollY - E->dragOffset.y) / cs)), snap);
      } else {
        bool over = false;
        for (const auto& r : E->blockRects) over = over || PtInRect(&r, pt);
        SetCursor(LoadCursorW(nullptr, over ? IDC_SIZEALL : IDC_ARROW));
      }
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) return TRUE;  // gestito in WM_MOUSEMOVE
      break;
    case WM_LBUTTONUP:
      if (E->dragging) {
        E->dragging = false;
        ReleaseCapture();
      }
      return 0;
    case WM_KEYDOWN: {
      const LayoutItem& it = Items()[size_t(E->sel)];
      const int step = GetKeyState(VK_SHIFT) < 0 ? E->grid : 1;  // frecce = 1 px, Shift = passo griglia
      int dx = 0, dy = 0;
      if (wp == VK_LEFT) dx = -step;
      if (wp == VK_RIGHT) dx = step;
      if (wp == VK_UP) dy = -step;
      if (wp == VK_DOWN) dy = step;
      if (dx || dy) MoveSelected(it.x + dx, it.y + dy, false);
      if (wp == VK_NEXT) SelectNext(1);  // PagGiù / PagSu: blocco successivo / precedente
      if (wp == VK_PRIOR) SelectNext(-1);
      RECT rc;
      GetClientRect(h, &rc);
      const POINT mid{rc.right / 2, rc.bottom / 2};
      if (wp == VK_ADD || wp == VK_OEM_PLUS) SetZoom(E->zoom * 1.25f, mid);  // + / - / 0: zoom da tastiera
      if (wp == VK_SUBTRACT || wp == VK_OEM_MINUS) SetZoom(E->zoom * 0.8f, mid);
      if (wp == '0' || wp == VK_NUMPAD0) SetZoom(1.0f, mid);
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------ finestra dell'editor
HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), E->wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), E->inst, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(E->font), TRUE);
  return c;
}
void Edit(int id, int x, int y, int w) {
  E->frames.push_back({id, {x, y, x + w, y + kRowH}});
  Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, x + 9, y + 6, w - 18, kRowH - 11, id);
}
void Button(int id, const wchar_t* t, int x, int y, int w, int h, COLORREF bg, bool primary = false) {
  ui::MakeButton(Make(L"BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, id), bg, primary);
}

// Passa a un altro formato della tela. Se il profilo non ha ancora un layout per quel formato,
// lo crea copiando il 16:9 in proporzione.
void SetFormat(const std::string& id) {
  if (!E->p.layouts.contains(id))
    E->p.layouts[id] = ScaleLayout(E->p.layouts[kDefaultLayoutFormat], 1920, LayoutFormatWidth(id));
  E->fmt = id;
  E->zoom = 1.0f;
  E->scrollX = E->scrollY = 0;
  const RECT cr = CanvasRect();
  MoveWindow(E->canvas, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top, TRUE);
  InvalidateRect(E->wnd, nullptr, FALSE);
  UpdateFields();
  Refresh();
}

void Build() {
  const RECT cr = CanvasRect();
  E->canvas = CreateWindowExW(0, kCanvasClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cr.left, cr.top,
                              cr.right - cr.left, cr.bottom - cr.top, E->wnd, reinterpret_cast<HMENU>(IDC_CANVAS),
                              E->inst, nullptr);
  HWND res = Make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 804, 52, 204, 240, IDC_RES);
  ApplyDarkControlTheme(res, L"DarkMode_CFD");
  ui::MakeCombo(res, ui::col::Bg);
  for (const auto& r : E->resolutions) SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.label.c_str()));
  ComboBox_SetCurSel(res, E->res);
  HWND fmt = Make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 1016, 52, 204, 240, IDC_FORMAT);
  ApplyDarkControlTheme(fmt, L"DarkMode_CFD");
  ui::MakeCombo(fmt, ui::col::Bg);
  for (int i = 0; i < int(std::size(kLayoutFormats)); ++i) {
    SendMessageW(fmt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kLayoutFormats[i].label));
    if (E->fmt == kLayoutFormats[i].id) ComboBox_SetCurSel(fmt, i);
  }
  for (int i = 0; i < kElementCount; ++i)
    Make(ui::kCheckClass, kNames[i], WS_TABSTOP, 1258, 96 + i * 28, 190, 24, IDC_VIS + i);
  Edit(IDC_EX, 1360, 386, 86);
  Edit(IDC_EY, 1360, 420, 86);
  Edit(IDC_ESCALE, 1360, 454, 86);
  for (int i = 0; i < kMaxFieldsPerElement; ++i)
    Make(ui::kCheckClass, L"", WS_TABSTOP, 1254, 540 + i * 26, 196, 24, IDC_FIELD + i);
  Make(ui::kCheckClass, T(L"Aggancia alla griglia"), WS_TABSTOP, 1254, 728, 196, 24, IDC_SNAP);
  HWND grid = Make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 1360, 761, 86, 200, IDC_GRID);
  ApplyDarkControlTheme(grid, L"DarkMode_CFD");
  ui::MakeCombo(grid, ui::col::Panel);
  for (const wchar_t* g : kGridNames) SendMessageW(grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g));
  Button(IDC_RESET, T(L"Ripristina disposizione"), 1254, 794, 192, 28, ui::col::Panel);
  Button(IDC_SENSORS, T(L"Sensori di sistema..."), 20, 848, 210, 32, ui::col::Bg);
  Button(IDC_CANCEL, T(L"Annulla"), 1240, 848, 104, 32, ui::col::Bg);
  Button(IDC_OK, L"OK", 1356, 848, 104, 32, ui::col::Bg, true);

  if (E->rtss) {  // il preset RTSS ha un solo layout per tutti i formati
    for (int i = 0; i < kElementCount; ++i) ShowWindow(Item(IDC_VIS + i), SW_HIDE);
    EnableWindow(Item(IDC_FORMAT), FALSE);
    EnableWindow(Item(IDC_RESET), FALSE);
  } else {
    for (int i = 0; i < kElementCount; ++i)
      Button_SetCheck(Item(IDC_VIS + i), Visible(E->p, Items()[size_t(i)].element) ? BST_CHECKED : BST_UNCHECKED);
  }
  Button_SetCheck(Item(IDC_SNAP), E->snap ? BST_CHECKED : BST_UNCHECKED);
  for (int i = 0; i < int(std::size(kGridSteps)); ++i)
    if (kGridSteps[i] == E->grid) ComboBox_SetCurSel(grid, i);
  UpdateFields();
}

void DrawTextAt(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextW(dc, t.c_str(), int(t.size()), &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

void PaintEditor(HDC dc, const RECT& client) {
  FillRect(dc, &client, E->brBg);
  SetBkMode(dc, TRANSPARENT);
  DrawTextAt(dc, E->rtss ? T(L"EDITOR PRESET RTSS") : T(L"EDITOR LAYOUT"), SR({20, 10, 600, 38}), E->fontTitle, ui::col::Accent, DT_LEFT | DT_TOP);
  DrawTextAt(dc,
             T(L"Trascina  ·  frecce 1 px (Shift = griglia)  ·  Alt = senza snap  ·  PagSu/Giù = blocco  ·  "
             L"Ctrl+rotellina = zoom"),
             SR({21, 40, 1460, 60}), E->font, ui::col::Sub, DT_LEFT | DT_TOP);
  DrawTextAt(dc, T(L"Pixel in"), SR({740, 52, 800, 80}), E->font, ui::col::Sub, DT_RIGHT | DT_VCENTER);

  auto section = [&](const wchar_t* title, RECT r) {
    const RECT s = SR(r);
    DrawTextAt(dc, title, {s.left + S(4), s.top - S(22), s.right, s.top - S(4)}, E->fontSection, ui::col::Sub,
               DT_LEFT | DT_BOTTOM);
    ui::FillRound(dc, s, S(8), ui::col::Panel, ui::col::Line);
  };
  const bool own = E->fmt != kDefaultLayoutFormat;
  DrawTextAt(dc,
             TF(L"TELA {} x {}  ·  FORMATO {}{}  ·  ZOOM {}% (0 = intera, tasto centrale = sposta)", ResW(), ResH(), ToWide(E->fmt),
                         own ? T(L" (dedicato)") : T(L" (base)"),
                         int(std::lround(E->zoom * 100))),
             SR({24, 62, 736, 80}), E->fontSection, ui::col::Sub, DT_LEFT | DT_BOTTOM);
  RECT border = CanvasRect();
  InflateRect(&border, 1, 1);
  ui::FillRound(dc, border, S(2), ui::col::Line, ui::col::Line, 0);

  section(E->rtss ? L"PRESET RTSS" : T(L"ELEMENTI"), {1240, 84, 1460, 300});
  if (E->rtss) {
    DrawTextAt(dc, ToWide(E->p.rtss.name), SR({1254, 96, 1450, 122}), E->fontBold, ui::col::Text,
               DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    RECT info = SR({1254, 128, 1450, 292});
    SelectObject(dc, E->font);
    SetTextColor(dc, ui::col::Sub);
    const std::wstring t = TF(
        L"{} testi. Clicca un testo sulla tela e trascinalo, oppure usa le frecce.\n\nPosizione dell'intero "
        L"overlay, font e sfondo: impostazioni del profilo.",
        E->p.rtss.layers.size());
    DrawTextW(dc, t.c_str(), -1, &info, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
  }
  // indicatore del blocco selezionato accanto alla sua casella
  if (!E->rtss && E->sel < kElementCount) {
    const RECT mark = SR({1244, 98 + E->sel * 28, 1249, 118 + E->sel * 28});
    ui::FillRound(dc, mark, S(2), ui::col::Accent, ui::col::Accent, 0);
  }
  DrawTextAt(dc,
             E->p.sensors.empty()
                 ? std::wstring(T(L"Temperature, ventole, dischi, rete...: aggiungili con \"Sensori di sistema\" "
                                L"e spostali qui come gli altri blocchi."))
                 : TF(L"{} {}: sono blocchi sulla tela (clic per selezionarli, PagSu/PagGiù per scorrerli).",
                               E->p.sensors.size(), E->p.sensors.size() == 1 ? T(L"sensore aggiunto") : T(L"sensori aggiunti")),
             SR({244, 848, 1220, 880}), E->font, ui::col::Sub, DT_LEFT | DT_VCENTER);

  section(T(L"BLOCCO SELEZIONATO"), {1240, 340, 1460, 488});
  DrawTextAt(dc, ElementName(E->sel), SR({1254, 352, 1450, 380}), E->fontBold, ui::col::Text,
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  DrawTextAt(dc, L"X (px)", SR({1254, 386, 1356, 414}), E->font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  DrawTextAt(dc, L"Y (px)", SR({1254, 420, 1356, 448}), E->font, ui::col::Sub, DT_LEFT | DT_VCENTER);
  DrawTextAt(dc, T(L"Dimensione %"), SR({1254, 454, 1356, 482}), E->font, ui::col::Sub, DT_LEFT | DT_VCENTER);

  section(T(L"CAMPI DEL BLOCCO"), {1240, 528, 1460, 676});
  if (E->rtss) {
    RECT tr = SR({1254, 540, 1450, 668});
    SelectObject(dc, E->font);
    SetTextColor(dc, ui::col::Sub);
    const std::wstring raw = T(L"Testo nel preset:\n") + ToWide(E->p.rtss.layers[size_t(E->sel)].text);
    DrawTextW(dc, raw.c_str(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
  } else if (const SensorPick* sp = PickOf(Items()[size_t(E->sel)].element)) {
    DrawTextAt(dc, ToWide(sp->group.empty() ? "Sensore" : sp->group), SR({1254, 540, 1450, 566}), E->font,
               ui::col::Text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    DrawTextAt(dc, T(L"Etichetta e rimozione:"), SR({1254, 574, 1450, 600}), E->font, ui::col::Sub, DT_LEFT | DT_VCENTER);
    DrawTextAt(dc, T(L"\"Sensori di sistema...\""), SR({1254, 600, 1450, 626}), E->font, ui::col::Sub,
               DT_LEFT | DT_VCENTER);
  } else if (FieldsOf(Items()[size_t(E->sel)].element).empty()) {
    DrawTextAt(dc, T(L"Il grafico non ha campi da scegliere."), SR({1254, 540, 1450, 566}), E->font, ui::col::Sub,
               DT_LEFT | DT_VCENTER);
  }

  section(T(L"GRIGLIA"), {1240, 716, 1460, 828});
  DrawTextAt(dc, T(L"Passo"), SR({1254, 761, 1356, 789}), E->font, ui::col::Sub, DT_LEFT | DT_VCENTER);

  for (const auto& [id, r] : E->frames)
    ui::FillRound(dc, SR(r), S(6), ui::col::Input, E->focusedEdit == id ? ui::col::Accent : ui::col::Line);
}

int ReadInt(int id, int fallback) {
  wchar_t buf[32];
  GetWindowTextW(Item(id), buf, 32);
  try {
    return std::stoi(buf);
  } catch (...) {
    return fallback;
  }
}

LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      PaintEditor(mem, rc);
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
    case WM_DRAWITEM:
      ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT cr;
      GetWindowRect(E->canvas, &cr);
      if (PtInRect(&cr, pt)) return SendMessageW(E->canvas, msg, wp, lp);
      break;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, ui::col::Text);
      SetBkColor(dc, ui::col::Input);
      return reinterpret_cast<LRESULT>(E->brInput);
    }
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, ui::col::Text);
      SetBkColor(dc, ui::col::Input);
      return reinterpret_cast<LRESULT>(E->brInput);
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      if ((id == IDC_EX || id == IDC_EY || id == IDC_ESCALE) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        E->focusedEdit = code == EN_SETFOCUS ? id : 0;
        for (const auto& [fid, r] : E->frames)
          if (fid == id) {
            RECT sr = SR(r);
            InvalidateRect(h, &sr, FALSE);
          }
        return 0;
      }
      if ((id == IDC_EX || id == IDC_EY || id == IDC_ESCALE) && code == EN_CHANGE && !E->updatingFields) {
        LayoutItem& it = Items()[size_t(E->sel)];
        if (id == IDC_ESCALE) {
          it.scale = std::clamp(ReadInt(id, it.scale), 40, 400);
          SyncRtssLayer(E->sel);
          Refresh();
        } else {
          const int x = id == IDC_EX ? RefX(ReadInt(id, DispX(it.x))) : it.x;
          const int y = id == IDC_EY ? RefY(ReadInt(id, DispY(it.y))) : it.y;
          it.x = std::clamp(x, 0, RefW() - 1);
          it.y = std::clamp(y, 0, kLayoutRefH - 1);
          SyncRtssLayer(E->sel);
          Refresh();
        }
        return 0;
      }
      if (id == IDC_RES && code == CBN_SELCHANGE) {
        E->res = std::max(0, ComboBox_GetCurSel(Item(IDC_RES)));
        UpdateFields();
        Refresh();
        InvalidateRect(E->wnd, nullptr, FALSE);
        SetFocus(E->canvas);
        return 0;
      }
      if (id == IDC_FORMAT && code == CBN_SELCHANGE) {
        SetFormat(kLayoutFormats[std::max(0, ComboBox_GetCurSel(Item(IDC_FORMAT)))].id);
        SetFocus(E->canvas);
        return 0;
      }
      if (id == IDC_GRID && code == CBN_SELCHANGE) {
        E->grid = kGridSteps[std::max(0, ComboBox_GetCurSel(Item(IDC_GRID)))];
        Refresh();
        SetFocus(E->canvas);
        return 0;
      }
      if (code != BN_CLICKED) return 0;
      if (id != IDC_OK && id != IDC_CANCEL && id != IDC_SENSORS) SetFocus(E->canvas);
      if (id >= IDC_FIELD && id < IDC_FIELD + kMaxFieldsPerElement) {
        const std::string el = Items()[size_t(E->sel)].element;
        const auto fields = FieldsOf(el);
        const int fi = id - IDC_FIELD;
        if (fi < int(fields.size())) {
          SetFieldOn(E->p, el, fields[size_t(fi)]->key, Button_GetCheck(Item(id)) == BST_CHECKED);
          Refresh();
        }
      } else if (id >= IDC_VIS && id < IDC_VIS + kElementCount) {
        const int i = id - IDC_VIS;
        const bool on = Button_GetCheck(Item(id)) == BST_CHECKED;
        Visible(E->p, Items()[size_t(i)].element) = on;
        if (on)
          Select(i);
        else if (E->sel == i)
          SelectNext(1);
        else
          Refresh();
      } else if (id == IDC_SNAP) {
        E->snap = Button_GetCheck(Item(IDC_SNAP)) == BST_CHECKED;
      } else if (id == IDC_RESET) {
        Items() = ScaleLayout(DefaultLayoutItems(), 1920, RefW());
        Sanitize(E->p);  // rimette i blocchi dei sensori in colonna
        UpdateFields();
        Refresh();
      } else if (id == IDC_SENSORS) {
        if (RunSensorBrowser(E->wnd, E->inst, E->font, E->p)) {
          Sanitize(E->p);  // blocchi per i sensori nuovi, via quelli tolti
          if (const auto feed = ReadSensorFeed()) E->feed = feed->sensors;
          E->sel = std::min(E->sel, int(Items().size()) - 1);
          UpdateFields();
          Refresh();
          InvalidateRect(E->wnd, nullptr, FALSE);
        }
      } else if (id == IDC_OK) {
        E->ok = true;
        E->done = true;
      } else if (id == IDC_CANCEL) {
        E->done = true;
      }
      return 0;
    }
    case WM_CLOSE:
      E->done = true;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

HFONT MakeFont(int tenthsOfPoint, int weight) {
  return CreateFontW(-MulDiv(tenthsOfPoint, E->dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

bool RunLayoutEditor(HWND owner, HINSTANCE inst, HFONT uiFont, Profile& profile) {
  Editor ed;
  E = &ed;
  ed.inst = inst;
  ed.dpi = int(GetDpiForSystem());
  ed.p = profile;
  Sanitize(ed.p);  // garantisce un blocco per ogni elemento
  if (const auto feed = ReadSensorFeed()) ed.feed = feed->sensors;
  MONITORINFO mi{sizeof(mi)};
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi)) {
    const double aspect = double(mi.rcMonitor.right - mi.rcMonitor.left) / (mi.rcMonitor.bottom - mi.rcMonitor.top);
    ed.fmt = ClosestLayoutFormat(aspect);
    // Risoluzioni per X/Y: quella vera del monitor (anche se Windows scala i DPI) più le classiche.
    MONITORINFOEXW mx{};
    mx.cbSize = sizeof(mx);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mx) &&
        EnumDisplaySettingsW(mx.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
      mw = int(dm.dmPelsWidth);
      mh = int(dm.dmPelsHeight);
    }
    ed.resolutions.push_back({std::format(L"Monitor  {} x {}", mw, mh), mw, mh, ClosestLayoutFormat(double(mw) / mh)});
    for (const auto& [label, h] : {std::pair{L"1080p", 1080}, std::pair{L"1440p (2K)", 1440}, std::pair{L"2160p (4K)", 2160}})
      ed.resolutions.push_back({label, 0, h, ""});
    if (!ed.p.layouts.contains(ed.fmt))
      ed.p.layouts[ed.fmt] = ScaleLayout(ed.p.layouts[kDefaultLayoutFormat], 1920, LayoutFormatWidth(ed.fmt));
  }
  ed.rtss = ed.p.layout == "rtss" && !ed.p.rtss.layers.empty();
  if (ed.rtss) {
    ed.rtssFamily = ResolveRtssFamily(ed.p.rtss.fontFace);
    LayoutRtss();
  }
  ed.font = uiFont;
  ed.fontBold = MakeFont(95, FW_SEMIBOLD);
  ed.fontTitle = MakeFont(150, FW_BOLD);
  ed.fontSection = MakeFont(80, FW_BOLD);
  ed.brBg = CreateSolidBrush(ui::col::Bg);
  ed.brPanel = CreateSolidBrush(ui::col::Panel);
  ed.brInput = CreateSolidBrush(ui::col::Input);

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kCanvasClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = EditorProc;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = ed.brBg;
    wc.lpszClassName = kEditorClass;
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
  MONITORINFO om{sizeof(om)};  // sempre tutta dentro lo schermo della finestra impostazioni
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &om)) {
    x = std::clamp(x, int(om.rcWork.left), std::max(int(om.rcWork.left), int(om.rcWork.right) - w));
    y = std::clamp(y, int(om.rcWork.top), std::max(int(om.rcWork.top), int(om.rcWork.bottom) - h));
  }
  ed.wnd = CreateWindowExW(0, kEditorClass, ((ed.rtss ? T(L"Editor preset RTSS - ") : T(L"Editor layout - ")) + ToWide(profile.name)).c_str(), style, x, y, w, h,
                           owner, nullptr, inst, nullptr);
  if (!ed.wnd) {
    E = nullptr;
    return false;
  }
  ApplyDarkTitleBar(ed.wnd, ui::col::Bg);
  Build();
  EnableWindow(owner, FALSE);
  ShowWindow(ed.wnd, SW_SHOW);
  SetFocus(ed.canvas);

  MSG msg;
  while (!ed.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(ed.wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  DestroyWindow(ed.wnd);

  if (ed.ok) {
    if (!ed.rtss) ed.p.layout = "free";  // l'editor salva il layout libero, oppure il preset RTSS modificato
    profile = ed.p;
  }
  for (auto& [k, f] : ed.rtssFonts) DeleteObject(f);
  for (auto& [k, f] : ed.fonts) DeleteObject(f);
  for (HGDIOBJ o2 : std::initializer_list<HGDIOBJ>{ed.fontBold, ed.fontTitle, ed.fontSection, ed.brBg, ed.brPanel,
                                                   ed.brInput})
    DeleteObject(o2);
  E = nullptr;
  return ed.ok;
}

}  // namespace po
