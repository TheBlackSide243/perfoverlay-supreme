#include "settings/benchmark_page.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>
// gdiplus.h usa min/max non qualificati, disattivati da NOMINMAX.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include "common/benchmark.h"
#include "common/branding.h"
#include "common/i18n.h"
#include "common/util.h"
#include "settings/theme.h"

namespace po::ui {
namespace {

// ------------------------------------------------------------------ testi
std::wstring Num(double v, int decimals) {
  std::wstring s = std::format(L"{:.{}f}", v, decimals);
  if (CurrentLang() == Lang::It) std::replace(s.begin(), s.end(), L'.', L',');
  return s;
}
std::wstring Fps(double v) { return v > 0 ? Num(v, v >= 100 ? 0 : 1) : L"—"; }

// "2026-11-21 21:14:03" → "21/11/2026 21:14" (italiano) o "2026-11-21 21:14"; senza anno per la lista.
std::wstring DateText(const std::string& t, bool withYear) {
  if (t.size() < 16) return ToWide(t);
  const std::wstring y = ToWide(t.substr(0, 4)), m = ToWide(t.substr(5, 2)), d = ToWide(t.substr(8, 2)),
                     hm = ToWide(t.substr(11, 5));
  if (CurrentLang() == Lang::It) return d + L"/" + m + (withYear ? L"/" + y : L"") + L" " + hm;
  return (withYear ? y + L"-" : L"") + m + L"-" + d + L" " + hm;
}

std::wstring DurationText(double seconds) {
  const int s = int(seconds + 0.5);
  if (s < 60) return std::format(L"{} s", s);
  if (s < 3600) return std::format(L"{} min {:02} s", s / 60, s % 60);
  return std::format(L"{} h {:02} min", s / 3600, s / 60 % 60);
}

std::wstring ClockText(double seconds) {
  const int s = int(seconds + 0.5);
  return s >= 3600 ? std::format(L"{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60)
                   : std::format(L"{}:{:02}", s / 60, s % 60);
}

std::wstring ApiText(const std::string& api) {
  if (api == "DXGI") return L"DirectX 10-12";
  if (api == "D3D9") return L"DirectX 9";
  if (api == "DxgKrnl") return L"Vulkan / OpenGL";
  return ToWide(api);
}

const wchar_t* KindText(const RunSummary& s) { return s.IsBenchmark() ? L"Benchmark" : T(L"Partita"); }

// ------------------------------------------------------------------ scala e font
float Scale(HWND h) {
  if (HWND root = GetAncestor(h, GA_ROOT))
    if (const auto v = reinterpret_cast<uintptr_t>(GetPropW(root, kScaleProp))) return float(v) / 1000.0f;
  return GetDpiForWindow(h) / 96.0f;
}

struct Fonts {
  float scale = 0;
  HFONT normal = nullptr, bold = nullptr, tiny = nullptr, big = nullptr, head = nullptr;
};
Fonts g_fonts;

HFONT MakeFont(float scale, int tenthsOfPoint, int weight) {
  return CreateFontW(-int(std::lround(tenthsOfPoint * 96.0f * scale / 720.0f)), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                     L"Segoe UI");
}

const Fonts& FontsFor(float scale) {
  if (g_fonts.normal && std::fabs(g_fonts.scale - scale) < 0.001f) return g_fonts;
  for (HFONT f : {g_fonts.normal, g_fonts.bold, g_fonts.tiny, g_fonts.big, g_fonts.head})
    if (f) DeleteObject(f);
  g_fonts.scale = scale;
  g_fonts.normal = MakeFont(scale, 95, FW_NORMAL);
  g_fonts.bold = MakeFont(scale, 95, FW_SEMIBOLD);
  g_fonts.tiny = MakeFont(scale, 85, FW_NORMAL);
  g_fonts.big = MakeFont(scale, 150, FW_SEMIBOLD);
  g_fonts.head = MakeFont(scale, 110, FW_SEMIBOLD);
  return g_fonts;
}

void Text(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextW(dc, t.c_str(), int(t.size()), &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

int TextWidth(HDC dc, const std::wstring& t, HFONT f) {
  SelectObject(dc, f);
  SIZE sz{};
  GetTextExtentPoint32W(dc, t.c_str(), int(t.size()), &sz);
  return sz.cx;
}

Gdiplus::Color Gp(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// Colore della registrazione di confronto: ambra, o ciano se l'accento scelto è già un giallo-arancio.
COLORREF CompareColor() {
  const COLORREF a = col::Accent;
  const int dist = std::abs(GetRValue(a) - 250) + std::abs(GetGValue(a) - 204) + std::abs(GetBValue(a) - 21);
  return dist < 200 ? RGB(34, 211, 238) : RGB(250, 204, 21);
}

// ------------------------------------------------------------------ riquadro del dettaglio
struct View {
  bool has = false, hasB = false;
  RunSummary a, b;
  std::vector<float> framesA, framesB;  // frame time in ms (solo benchmark)
  std::wstring hotkey;
};

double NiceCeil(double v) {
  if (v <= 0) return 1;
  const double mag = std::pow(10.0, std::floor(std::log10(v)));
  for (double m : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0})
    if (v <= m * mag) return m * mag;
  return 10 * mag;
}

// Una serie nel grafico: per ogni colonna di pixel il valore minimo e massimo che ci cade dentro.
struct Column {
  float lo = 0, hi = 0;
  bool any = false;
};

std::vector<Column> ColumnsFromFrames(const std::vector<float>& frames, double xMaxSeconds, int width) {
  std::vector<Column> cols(size_t(std::max(1, width)));
  double t = 0;
  for (const float ms : frames) {
    t += ms;
    Column& c = cols[std::min(cols.size() - 1, size_t(t / 1000.0 / xMaxSeconds * double(cols.size())))];
    c.lo = c.any ? std::min(c.lo, ms) : ms;
    c.hi = c.any ? std::max(c.hi, ms) : ms;
    c.any = true;
  }
  return cols;
}

std::vector<Column> ColumnsFromSeries(const std::vector<float>& series, int step, double xMaxSeconds, int width) {
  std::vector<Column> cols(size_t(std::max(1, width)));
  for (size_t i = 0; i < series.size(); ++i) {
    const double x = (double(i) + 0.5) * step;
    Column& c = cols[std::min(cols.size() - 1, size_t(x / xMaxSeconds * double(cols.size())))];
    c.lo = c.any ? std::min(c.lo, series[i]) : series[i];
    c.hi = c.any ? std::max(c.hi, series[i]) : series[i];
    c.any = true;
  }
  return cols;
}

void DrawSeries(Gdiplus::Graphics& g, const std::vector<Column>& cols, const RECT& plot, double yMax, COLORREF color,
                float scale) {
  std::vector<Gdiplus::PointF> hi, lo;
  const float h = float(plot.bottom - plot.top);
  auto y = [&](float v) { return float(plot.bottom) - h * float(std::clamp(double(v) / yMax, 0.0, 1.0)); };
  for (size_t i = 0; i < cols.size(); ++i) {
    if (!cols[i].any) continue;
    const float x = float(plot.left) + float(i) + 0.5f;
    hi.push_back({x, y(cols[i].hi)});
    lo.push_back({x, y(cols[i].lo)});
  }
  if (hi.empty()) return;
  if (hi.size() == 1) {  // un solo punto: un tratto orizzontale, altrimenti non si vedrebbe
    hi.push_back({hi[0].X + 6 * scale, hi[0].Y});
    lo.push_back({lo[0].X + 6 * scale, lo[0].Y});
  }
  // Fascia tra minimo e massimo (visibile quando in una colonna cadono molti frame), poi la linea dei massimi.
  std::vector<Gdiplus::PointF> band = hi;
  band.insert(band.end(), lo.rbegin(), lo.rend());
  Gdiplus::SolidBrush fill(Gp(color, 70));
  g.FillPolygon(&fill, band.data(), int(band.size()));
  Gdiplus::Pen pen(Gp(color), 1.4f * scale);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  g.DrawLines(&pen, hi.data(), int(hi.size()));
}

struct Tile {
  std::wstring label, value, other;  // other = valore della registrazione di confronto
  double a = 0, b = 0;
  int better = 0;  // +1 = più alto è meglio, -1 = più basso è meglio, 0 = neutro
};

void DrawTile(HDC dc, const RECT& r, const Tile& t, bool compare, const Fonts& f, float s) {
  auto S = [s](int v) { return int(std::lround(v * s)); };
  FillRound(dc, r, S(8), Blend(col::Panel, col::Bg, 0.5f), col::Line);
  Text(dc, t.label, {r.left + S(10), r.top + S(7), r.right - S(6), r.top + S(23)}, f.tiny, col::Sub,
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  Text(dc, t.value, {r.left + S(10), r.top + S(24), r.right - S(6), r.top + S(52)}, f.big, col::Text,
       DT_LEFT | DT_VCENTER);
  if (!compare) return;
  // Terza riga: pallino del colore di confronto con il suo valore, e a destra la differenza in percentuale.
  const int cy = r.top + S(63);
  FillCircle(dc, r.left + S(14), cy, S(3), CompareColor(), CompareColor());
  Text(dc, t.other, {r.left + S(22), cy - S(9), r.right - S(6), cy + S(9)}, f.tiny, col::Sub, DT_LEFT | DT_VCENTER);
  if (t.a > 0 && t.b > 0) {
    const double pct = (t.a - t.b) / t.b * 100.0;
    const bool flat = std::fabs(pct) < 0.5;
    const COLORREF c = flat || !t.better ? col::Sub : (pct > 0) == (t.better > 0) ? col::Good : col::Bad;
    const std::wstring d = flat ? L"=" : (pct > 0 ? L"+" : L"−") + Num(std::fabs(pct), std::fabs(pct) < 10 ? 1 : 0) + L"%";
    Text(dc, d, {r.left + S(22), cy - S(9), r.right - S(8), cy + S(9)}, f.tiny, c, DT_RIGHT | DT_VCENTER);
  }
}

std::vector<Tile> MakeTiles(const View& v) {
  const RunSummary& a = v.a;
  const RunSummary& b = v.b;
  auto pct = [](double x) { return x > 0 ? Num(x, 0) + L"%" : std::wstring(L"—"); };
  auto temp = [](double x) { return x > 0 ? Num(x, 0) + L" °C" : std::wstring(L"—"); };
  auto stutter = [](const RunSummary& r) { return r.frames ? Num(r.stutterPct, 1) + L"%" : std::wstring(L"—"); };
  return {
      {T(L"FPS medi"), Fps(a.fpsAvg), Fps(b.fpsAvg), a.fpsAvg, b.fpsAvg, 1},
      {L"1% low", Fps(a.low1), Fps(b.low1), a.low1, b.low1, 1},
      {T(L"0,1% low"), Fps(a.low01), Fps(b.low01), a.low01, b.low01, 1},
      {L"Stutter", stutter(a), stutter(b), a.stutterPct, b.stutterPct, -1},
      {T(L"CPU media"), pct(a.cpuAvg), pct(b.cpuAvg), a.cpuAvg, b.cpuAvg, 0},
      {T(L"GPU media"), pct(a.gpuAvg), pct(b.gpuAvg), a.gpuAvg, b.gpuAvg, 0},
      {T(L"Temp. GPU max"), temp(a.gpuTempMax), temp(b.gpuTempMax), a.gpuTempMax, b.gpuTempMax, -1},
      {T(L"Temp. CPU max"), temp(a.cpuTempMax), temp(b.cpuTempMax), a.cpuTempMax, b.cpuTempMax, -1},
  };
}

void DrawGraph(HDC dc, const View& v, const RECT& area, const Fonts& f, float s) {
  auto S = [s](int x) { return int(std::lround(x * s)); };
  // Frame time se tutte le registrazioni mostrate hanno i frame, altrimenti gli FPS nel tempo.
  const bool frameMode = !v.framesA.empty() && (!v.hasB || !v.framesB.empty());
  const COLORREF colA = col::Accent, colB = CompareColor();

  // Intestazione: cosa mostra il grafico; a destra la legenda (confronto) o i valori estremi.
  const RECT head{area.left, area.top, area.right, area.top + S(20)};
  Text(dc, frameMode ? T(L"Tempo di ogni frame (ms)") : T(L"FPS nel tempo"), head, f.bold, col::Text,
       DT_LEFT | DT_VCENTER);
  if (v.hasB) {
    const std::wstring la = DateText(v.a.time, false), lb = DateText(v.b.time, false);
    int x = head.right - TextWidth(dc, lb, f.tiny);
    Text(dc, lb, {x, head.top, head.right, head.bottom}, f.tiny, col::Sub, DT_LEFT | DT_VCENTER);
    FillCircle(dc, x - S(8), (head.top + head.bottom) / 2, S(3), colB, colB);
    x -= S(22) + TextWidth(dc, la, f.tiny);
    Text(dc, la, {x, head.top, head.right, head.bottom}, f.tiny, col::Sub, DT_LEFT | DT_VCENTER);
    FillCircle(dc, x - S(8), (head.top + head.bottom) / 2, S(3), colA, colA);
  } else {
    const std::wstring info = frameMode
                                  ? TF(L"P99 {} ms, massimo {} ms", Num(v.a.p99Ms, 1), Num(v.a.frameTimeMaxMs, 1))
                                  : TF(L"minimo {}, massimo {}", Fps(v.a.fpsMin), Fps(v.a.fpsMax));
    Text(dc, info, head, f.tiny, col::Sub, DT_RIGHT | DT_VCENTER);
  }

  const RECT plot{area.left + S(34), area.top + S(28), area.right - S(4), area.bottom - S(18)};
  if (plot.right - plot.left < 20 || plot.bottom - plot.top < 20) return;
  const double xMax = std::max({1.0, v.a.seconds, v.hasB ? v.b.seconds : 0.0});
  double yMax = 1;
  if (frameMode) {
    // Un singolo blocco lunghissimo schiaccerebbe tutto il resto: l'asse si ferma a tre volte il P99.
    auto top = [](const RunSummary& r) { return std::min(r.frameTimeMaxMs, std::max(3.0 * r.p99Ms, 1.0)); };
    yMax = NiceCeil(std::max(top(v.a), v.hasB ? top(v.b) : 0.0) * 1.08);
  } else {
    double m = 1;
    for (float x : v.a.fps) m = std::max(m, double(x));
    if (v.hasB)
      for (float x : v.b.fps) m = std::max(m, double(x));
    yMax = NiceCeil(m * 1.08);
  }

  // Griglia: tre righe orizzontali con il valore, tempo a inizio, metà e fine.
  HPEN grid = CreatePen(PS_SOLID, 1, col::Line);
  HGDIOBJ oldPen = SelectObject(dc, grid);
  for (int i = 0; i <= 2; ++i) {
    const int y = plot.bottom - (plot.bottom - plot.top) * i / 2;
    MoveToEx(dc, plot.left, y, nullptr);
    LineTo(dc, plot.right, y);
    const double val = yMax * i / 2;
    Text(dc, Num(val, val < 10 && val > 0 ? 1 : 0), {area.left, y - S(9), plot.left - S(6), y + S(9)}, f.tiny,
         col::Sub, DT_RIGHT | DT_VCENTER);
  }
  SelectObject(dc, oldPen);
  DeleteObject(grid);
  const int ty = plot.bottom + S(2);
  Text(dc, L"0:00", {plot.left, ty, plot.right, ty + S(16)}, f.tiny, col::Sub, DT_LEFT | DT_VCENTER);
  Text(dc, ClockText(xMax / 2), {plot.left, ty, plot.right, ty + S(16)}, f.tiny, col::Sub, DT_CENTER | DT_VCENTER);
  Text(dc, ClockText(xMax), {plot.left, ty, plot.right, ty + S(16)}, f.tiny, col::Sub, DT_RIGHT | DT_VCENTER);

  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetClip(Gdiplus::Rect(plot.left, plot.top, plot.right - plot.left, plot.bottom - plot.top + 1));
  const int w = plot.right - plot.left;
  auto cols = [&](const RunSummary& r, const std::vector<float>& frames) {
    return frameMode ? ColumnsFromFrames(frames, xMax, w) : ColumnsFromSeries(r.fps, r.step, xMax, w);
  };
  if (v.hasB) DrawSeries(g, cols(v.b, v.framesB), plot, yMax, colB, s);
  DrawSeries(g, cols(v.a, v.framesA), plot, yMax, colA, s);
}

void PaintView(HWND h, HDC dc, const RECT& rc, const View& v) {
  const float s = Scale(h);
  const Fonts& f = FontsFor(s);
  auto S = [s](int x) { return int(std::lround(x * s)); };
  HBRUSH bg = CreateSolidBrush(col::Panel);
  FillRect(dc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(dc, TRANSPARENT);

  if (!v.has) {  // ancora nessuna registrazione: come si fa ad averne una
    const int cy = (rc.top + rc.bottom) / 2;
    Text(dc, T(L"Nessuna registrazione"), {rc.left, cy - S(44), rc.right, cy - S(20)}, f.head, col::Text,
         DT_CENTER | DT_VCENTER);
    const std::wstring how = v.hotkey.empty() ? std::wstring(T(L"Scegli una hotkey per il benchmark nella sezione Hotkey."))
                                              : TF(L"In gioco premi {} per avviare e fermare un benchmark.", v.hotkey);
    Text(dc, how, {rc.left, cy - S(12), rc.right, cy + S(10)}, f.normal, col::Sub, DT_CENTER | DT_VCENTER);
    Text(dc, T(L"Il riepilogo di ogni partita compare qui quando chiudi il gioco."),
         {rc.left, cy + S(12), rc.right, cy + S(34)}, f.normal, col::Sub, DT_CENTER | DT_VCENTER);
    return;
  }

  // Intestazione: gioco e tipo, poi data, durata, risoluzione e API.
  const COLORREF kindColor = v.a.IsBenchmark() ? col::Accent : col::Sub;
  const std::wstring kind = KindText(v.a);
  const int kw = TextWidth(dc, kind, f.tiny) + S(20);
  const RECT pill{rc.right - kw - S(2), rc.top + S(2), rc.right - S(2), rc.top + S(24)};
  FillRound(dc, pill, S(11), Blend(col::Panel, kindColor, 0.15f), Blend(col::Panel, kindColor, 0.5f));
  Text(dc, kind, pill, f.tiny, v.a.IsBenchmark() ? col::AccentHover : col::Text, DT_CENTER | DT_VCENTER);
  Text(dc, ToWide(v.a.DisplayName()), {rc.left + S(4), rc.top, pill.left - S(10), rc.top + S(26)}, f.head, col::Text,
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  std::wstring where = v.a.width > 0 ? std::format(L"{}×{}", v.a.width, v.a.height) : std::wstring();
  if (!v.a.api.empty()) where += (where.empty() ? L"" : L", ") + ApiText(v.a.api);
  Text(dc, where, {rc.left + S(4), rc.top + S(26), rc.right - S(4), rc.top + S(46)}, f.normal, col::Sub,
       DT_RIGHT | DT_VCENTER);
  Text(dc, DateText(v.a.time, true) + L", " + DurationText(v.a.seconds),
       {rc.left + S(4), rc.top + S(26), rc.right - S(4), rc.top + S(46)}, f.normal, col::Sub, DT_LEFT | DT_VCENTER);

  // Otto riquadri: prestazioni sopra, sensori sotto. Con un confronto ogni riquadro ha una riga in più.
  const std::vector<Tile> tiles = MakeTiles(v);
  const int gap = S(8), top = rc.top + S(54), th = S(v.hasB ? 76 : 60);
  const int tw = (rc.right - rc.left - 3 * gap) / 4;
  for (size_t i = 0; i < tiles.size(); ++i) {
    const int cx = int(i % 4), cyi = int(i / 4);
    const int x = rc.left + cx * (tw + gap), y = top + cyi * (th + gap);
    DrawTile(dc, {x, y, cx == 3 ? rc.right : x + tw, y + th}, tiles[i], v.hasB, f, s);
  }
  // Riga sotto i riquadri: che cosa ha limitato gli FPS e quanti frame erano generati (solo se registrati).
  auto share = [](double x) { return Num(x, 0) + L"%"; };
  std::wstring note;
  auto part = [&](const wchar_t* name, double pct) {
    if (pct < 0.5) return;
    note += (note.empty() ? std::wstring(T(L"Limitato da: ")) : std::wstring(L", ")) + name + L" " + share(pct);
  };
  part(L"GPU", v.a.gpuBoundPct);
  part(L"CPU", v.a.cpuBoundPct);
  part(T(L"limite FPS"), v.a.capPct);
  if (!v.a.fgTech.empty() && v.a.generatedPct >= 0.5)
    note += (note.empty() ? L"" : L".  ") + TF(L"Frame generati: {} ({})", share(v.a.generatedPct), ToWide(v.a.fgTech));
  int graphTop = top + 2 * th + gap + S(12);
  if (!note.empty()) {
    Text(dc, note, {rc.left + S(4), graphTop - S(6), rc.right - S(4), graphTop + S(12)}, f.tiny, col::Sub,
         DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    graphTop += S(20);
  }
  DrawGraph(dc, v, {rc.left + S(2), graphTop, rc.right - S(2), rc.bottom}, f, s);
}

LRESULT CALLBACK ViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* v = reinterpret_cast<View*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new View()));
      break;
    case WM_NCDESTROY:
      delete v;
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      if (v && rc.right > 0 && rc.bottom > 0) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintView(h, mem, rc, *v);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
      }
      EndPaint(h, &ps);
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

View* ViewOf(HWND h) { return h ? reinterpret_cast<View*>(GetWindowLongPtrW(h, GWLP_USERDATA)) : nullptr; }

// ------------------------------------------------------------------ stato della sezione
struct Page {
  BenchPageControls c;
  std::vector<RunSummary> all;   // dalla più recente
  std::vector<int> shown;        // riga della lista → indice in all
  std::vector<int> compareIdx;   // riga di "Confronta con" → indice in all (-1 = nessun confronto)
  std::wstring hotkey;
};
Page g_page;

const RunSummary* Selected() {
  const int row = g_page.c.list ? ListBox_GetCurSel(g_page.c.list) : -1;
  if (row < 0 || size_t(row) >= g_page.shown.size()) return nullptr;
  return &g_page.all[size_t(g_page.shown[size_t(row)])];
}

const RunSummary* Compared() {
  const int row = g_page.c.compare ? ComboBox_GetCurSel(g_page.c.compare) : -1;
  if (row < 0 || size_t(row) >= g_page.compareIdx.size() || g_page.compareIdx[size_t(row)] < 0) return nullptr;
  return &g_page.all[size_t(g_page.compareIdx[size_t(row)])];
}

void UpdateView() {
  View* v = ViewOf(g_page.c.view);
  if (!v) return;
  const RunSummary* a = Selected();
  const RunSummary* b = a ? Compared() : nullptr;
  v->hotkey = g_page.hotkey;
  v->has = a != nullptr;
  v->hasB = b != nullptr;
  v->framesA.clear();
  v->framesB.clear();
  if (a) {
    v->a = *a;
    if (a->IsBenchmark()) v->framesA = LoadFrameTimes(a->id);
  }
  if (b) {
    v->b = *b;
    if (b->IsBenchmark()) v->framesB = LoadFrameTimes(b->id);
  }
  EnableWindow(g_page.c.remove, a != nullptr);
  EnableWindow(g_page.c.file, a != nullptr);
  EnableWindow(g_page.c.compare, a != nullptr && g_page.compareIdx.size() > 1);
  InvalidateRect(g_page.c.view, nullptr, FALSE);
}

// "Confronta con": prima le altre registrazioni dello stesso gioco, poi il resto.
void RebuildCompare(const std::string& keepId) {
  HWND cb = g_page.c.compare;
  if (!cb) return;
  ComboBox_ResetContent(cb);
  g_page.compareIdx.assign(1, -1);
  ComboBox_AddString(cb, T(L"Nessun confronto"));
  const RunSummary* sel = Selected();
  int keep = 0;
  if (sel) {
    for (const bool sameGame : {true, false}) {
      for (size_t i = 0; i < g_page.all.size(); ++i) {
        const RunSummary& r = g_page.all[i];
        if (&r == sel || (r.game == sel->game) != sameGame) continue;
        std::wstring label = DateText(r.time, false) + L"  " + Fps(r.fpsAvg) + L" FPS";
        if (!sameGame) label += L"  " + ToWide(r.DisplayName());
        if (r.id == keepId) keep = int(g_page.compareIdx.size());
        g_page.compareIdx.push_back(int(i));
        ComboBox_AddString(cb, label.c_str());
      }
    }
  }
  ComboBox_SetCurSel(cb, keep);
}

}  // namespace

void RegisterBenchView(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = ViewProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kBenchViewClass;
  RegisterClassExW(&wc);
}

void BenchPageInit(const BenchPageControls& c) { g_page.c = c; }

void BenchPageSetHotkey(const std::wstring& hotkey) {
  g_page.hotkey = hotkey;
  if (View* v = ViewOf(g_page.c.view)) {
    v->hotkey = hotkey;
    InvalidateRect(g_page.c.view, nullptr, FALSE);
  }
}

void BenchPageReload() {
  if (!g_page.c.list) return;
  const RunSummary* sel = Selected();
  const RunSummary* cmp = Compared();
  const std::string selId = sel ? sel->id : std::string(), cmpId = cmp ? cmp->id : std::string();
  g_page.all = ListRuns();
  const int filter = g_page.c.filter ? std::max(0, ComboBox_GetCurSel(g_page.c.filter)) : 0;
  SendMessageW(g_page.c.list, WM_SETREDRAW, FALSE, 0);
  ListBox_ResetContent(g_page.c.list);
  g_page.shown.clear();
  int row = -1;
  for (size_t i = 0; i < g_page.all.size(); ++i) {
    const RunSummary& r = g_page.all[i];
    if ((filter == 1 && !r.IsBenchmark()) || (filter == 2 && r.IsBenchmark())) continue;
    if (r.id == selId) row = int(g_page.shown.size());
    g_page.shown.push_back(int(i));
    ListBox_AddString(g_page.c.list, ToWide(r.DisplayName()).c_str());
  }
  if (row < 0 && !g_page.shown.empty()) row = 0;
  ListBox_SetCurSel(g_page.c.list, row);
  SendMessageW(g_page.c.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_page.c.list, nullptr, TRUE);
  RebuildCompare(cmpId);
  UpdateView();
}

void BenchPageListChanged() {
  RebuildCompare({});
  UpdateView();
}

void BenchPageCompareChanged() { UpdateView(); }

int BenchPageItemHeight(HWND list) { return int(std::lround(46 * Scale(list))); }

void BenchPageDrawItem(const DRAWITEMSTRUCT* d) {
  if (d->itemID == UINT(-1) || size_t(d->itemID) >= g_page.shown.size()) return;
  const RunSummary& run = g_page.all[size_t(g_page.shown[d->itemID])];
  const float s = Scale(d->hwndItem);
  const Fonts& f = FontsFor(s);
  auto S = [s](int x) { return int(std::lround(x * s)); };
  HDC dc = d->hDC;
  const RECT& r = d->rcItem;
  HBRUSH bg = CreateSolidBrush(col::Panel);
  FillRect(dc, &r, bg);
  DeleteObject(bg);
  const bool sel = d->itemState & ODS_SELECTED;
  if (sel) {
    RECT b = r;
    InflateRect(&b, -S(2), -S(2));
    FillRound(dc, b, S(6), Blend(col::Panel, col::Accent, 0.20f), Blend(col::Panel, col::Accent, 0.55f));
  }
  SetBkMode(dc, TRANSPARENT);
  // Riga 1: gioco e FPS medi. Riga 2: pallino del tipo (pieno = benchmark), data, tipo e durata.
  const std::wstring fps = Fps(run.fpsAvg) + L" FPS";
  const int fw = TextWidth(dc, fps, f.bold);
  Text(dc, fps, {r.right - S(12) - fw, r.top + S(5), r.right - S(12), r.top + S(25)}, f.bold,
       sel ? col::AccentHover : col::Text, DT_RIGHT | DT_VCENTER);
  Text(dc, ToWide(run.DisplayName()), {r.left + S(12), r.top + S(5), r.right - S(20) - fw, r.top + S(25)},
       sel ? f.bold : f.normal, col::Text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  const int cy = r.top + S(33);
  if (run.IsBenchmark())
    FillCircle(dc, r.left + S(16), cy, S(3), col::Accent, col::Accent);
  else
    FillCircle(dc, r.left + S(16), cy, S(3), col::Panel, col::Sub);
  Text(dc, DateText(run.time, false) + L"  " + KindText(run) + L", " + DurationText(run.seconds),
       {r.left + S(26), r.top + S(24), r.right - S(12), r.top + S(42)}, f.tiny, col::Sub,
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

bool BenchPageDelete(HWND owner) {
  const RunSummary* sel = Selected();
  if (!sel) return false;
  // Copie: quando la domanda si chiude la finestra torna attiva e ricarica l'elenco, e "sel" non vale più.
  const std::string id = sel->id;
  std::wstring name = ToWide(sel->DisplayName());
  if (name.size() > 60) name = name.substr(0, 59) + L"…";
  const std::wstring what = name + L", " + DateText(sel->time, true);
  const int row = ListBox_GetCurSel(g_page.c.list);
  if (MessageBoxW(owner, TF(L"Eliminare la registrazione \"{}\"?", what).c_str(), kAppName,
                  MB_YESNO | MB_ICONQUESTION) != IDYES)
    return false;
  if (!DeleteRun(id)) return false;
  BenchPageReload();
  // Dopo l'eliminazione resta selezionata la riga nello stesso punto della lista.
  if (!g_page.shown.empty()) {
    ListBox_SetCurSel(g_page.c.list, std::min(row, int(g_page.shown.size()) - 1));
    BenchPageListChanged();
  }
  return true;
}

void BenchPageShowFile() {
  const RunSummary* sel = Selected();
  if (!sel) return;
  // Il CSV dei frame se c'è (benchmark), altrimenti il riepilogo JSON.
  std::error_code ec;
  const auto csv = RunCsvPath(sel->id);
  const auto path = std::filesystem::exists(csv, ec) ? csv : RunJsonPath(sel->id);
  const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace po::ui
