// Rendering dei preset RTSS avanzati (ipertesto dell'OverlayEditor): testo ricco, riquadri, immagini dallo
// sprite sheet (colorate e ruotate), animazioni pilotate da sorgenti, grafici, condizioni <IF>/<ELSE>.
#include "monitor/overlay_window.h"

#include <d2d1effects.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <format>

#include "common/log.h"
#include "common/paths.h"
#include "common/util.h"
#include "monitor/overlay_window.h"
#include "monitor/rtss_engine.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

namespace po {
namespace {

D2D1_COLOR_F D(const ArgbColor& c) { return {c.r, c.g, c.b, c.a}; }

struct Item {
  enum class K { Text, Box, Image, Graph } k = K::Text;
  float x = 0, w = 0, h = 0;
  ArgbColor color;
  ComPtr<IDWriteTextLayout> layout;
  float baseline = 0;
  float border = 0;                  // Box: <B=w,h,spessore> = solo contorno
  D2D1_RECT_F src{};                 // Image
  float angle = 0;
  std::string source;                // Graph
  float gmin = 0, gmax = 0;
  int gflags = 0;
};
struct Line {
  std::vector<Item> items;
  float w = 0, ascent = 0, descent = 0, objH = 0;
  bool absolute = false;  // dopo <P=x,y>: posizione assoluta dall'origine dell'overlay
  float ax = 0, ay = 0;
  float Height() const { return std::max(ascent + descent, objH); }
};
struct PreparedLayer {
  std::vector<Line> lines;
  float x = 0, y = 0;           // angolo del contenuto
  D2D1_RECT_F bg{};
  ArgbColor bgColor{0, 0, 0, 0};
};

std::vector<double> Numbers(const std::vector<std::string>& parts, size_t from) {
  std::vector<double> v;
  for (size_t i = from; i < parts.size(); ++i) {
    try {
      v.push_back(Trim(parts[i]).empty() ? 0.0 : std::stod(Trim(parts[i])));
    } catch (...) {
      v.push_back(0);
    }
  }
  return v;
}

double PercentileMs(std::vector<float> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[size_t(std::lround(std::clamp(p / 100.0, 0.0, 1.0) * double(v.size() - 1)))];
}

// Macro di RTSS con i nomi dell'hardware: %GPU%, %CPU%, %CPUShort%.
std::wstring HardwareName(const std::string& macro, const SystemSnapshot& s) {
  const std::string m = ToLowerAscii(macro);
  auto strip = [](std::string n, std::initializer_list<const char*> words) {
    for (const char* w : words)
      for (size_t p; (p = n.find(w)) != std::string::npos;) n.erase(p, strlen(w));
    return Trim(n);
  };
  if (m == "gpu" || m == "gpushort") {
    std::string n = strip(s.gpu.name, {"NVIDIA ", "GeForce ", "AMD ", "Radeon(TM) ", "Intel(R) ", "Graphics"});
    return n.empty() ? L"GPU" : ToWide(n);
  }
  if (m == "cpu") return ToWide(s.cpu.name.empty() ? "CPU" : s.cpu.name);
  if (m == "cpushort") {
    // "Intel(R) Core(TM) i9-13900K" → "i9-13900K"; "AMD Ryzen 7 9800X3D 8-Core Processor" → "R7 9800X3D"
    std::string n = strip(s.cpu.name, {"Intel(R) ", "Core(TM) ", "(R)", "(TM)", " Processor", "AMD "});
    if (const auto core = n.find("-Core"); core != std::string::npos) {
      const auto sp = n.rfind(' ', core);
      n = Trim(n.substr(0, sp == std::string::npos ? 0 : sp));
    }
    if (n.rfind("Ryzen ", 0) == 0 && n.size() > 8) n = "R" + n.substr(6);
    return n.empty() ? L"CPU" : ToWide(n);
  }
  return {};
}

}  // namespace

// ------------------------------------------------------------------ risorse
IDWriteTextFormat* OverlayWindow::AdvFormat(const std::wstring& family, float px, bool bold) {
  if (!presetFontsLoaded_) {
    presetFontsLoaded_ = true;
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (SUCCEEDED(dwrite_.As(&f5)) && SUCCEEDED(f5->CreateFontSetBuilder(&builder))) {
      std::error_code ec;
      int added = 0;
      for (const auto& e : std::filesystem::directory_iterator(PresetsDir() / L"fonts", ec)) {
        ComPtr<IDWriteFontFile> file;
        if (SUCCEEDED(f5->CreateFontFileReference(e.path().c_str(), nullptr, &file)) &&
            SUCCEEDED(builder->AddFontFile(file.Get())))
          ++added;
      }
      ComPtr<IDWriteFontSet> set;
      ComPtr<IDWriteFontCollection1> coll;
      if (added && SUCCEEDED(builder->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll)))
        presetFonts_ = coll;
      if (added) LogInfo("Preset RTSS: {} font caricati", added);
    }
  }
  const std::wstring key = std::format(L"{}|{}|{}", family, int(std::lround(px * 4)), bold ? 1 : 0);
  if (auto it = advFormats_.find(key); it != advFormats_.end()) return it->second.Get();

  // Famiglia: prima i font del preset, poi quelli di sistema; "Adderley Bold" = famiglia "Adderley" in grassetto.
  std::wstring name = family;
  IDWriteFontCollection* coll = nullptr;
  auto has = [&](IDWriteFontCollection* c, const std::wstring& n) {
    UINT32 i = 0;
    BOOL exists = FALSE;
    return c && SUCCEEDED(c->FindFamilyName(n.c_str(), &i, &exists)) && exists;
  };
  ComPtr<IDWriteFontCollection> sys;
  dwrite_->GetSystemFontCollection(&sys);
  bool b = bold;
  if (has(presetFonts_.Get(), name)) {
    coll = presetFonts_.Get();
  } else if (has(sys.Get(), name)) {
    coll = sys.Get();
  } else if (name.size() > 5 && IEquals(ToUtf8(name.substr(name.size() - 5)), " bold")) {
    name = name.substr(0, name.size() - 5);
    b = true;
    coll = has(presetFonts_.Get(), name) ? presetFonts_.Get() : has(sys.Get(), name) ? sys.Get() : nullptr;
  }
  if (!coll) name = L"Bahnschrift";  // stretto e leggibile, presente in Windows 10/11
  ComPtr<IDWriteTextFormat> fmt;
  if (FAILED(dwrite_->CreateTextFormat(name.c_str(), coll, b ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, std::max(0.5f, px), L"",
                                       &fmt)))
    return nullptr;
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  advFormats_[key] = fmt;
  return fmt.Get();
}

ID2D1Bitmap1* OverlayWindow::PresetImage(const std::wstring& file) {
  if (file.empty() || !dc_) return nullptr;
  if (auto it = images_.find(file); it != images_.end()) return it->second.Get();
  images_[file] = nullptr;  // non riprovare a ogni frame se manca
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
    return nullptr;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  const auto path = PresetsDir() / file;
  if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) ||
      FAILED(dec->GetFrame(0, &frame)) || FAILED(wic_->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom))) {
    LogWarn("Immagine del preset non leggibile: {}", ToUtf8(path.wstring()));
    return nullptr;
  }
  const auto props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  ComPtr<ID2D1Bitmap1> bmp;
  if (FAILED(dc_->CreateBitmapFromWicBitmap(conv.Get(), &props, &bmp))) return nullptr;
  images_[file] = bmp;
  return bmp.Get();
}

// ------------------------------------------------------------------ rendering
void OverlayWindow::RenderRtssAdvanced(RtssEngine& engine, const Profile& p, const RECT& anchor,
                                       const std::vector<Row>& extra) {
  (void)extra;
  const RtssLayout* preset = engine.Preset();
  if (!preset || !Prepare(p)) return;
  const float uiScale = UiScale(p, anchor);
  // ZoomRatio dell'OverlayEditor: 1 = 1×, ogni passo +0,25× (i preset a zoom 3 sono 1,5×).
  const float zoom = preset->zoomRatio <= 1 ? 1.0f : 1.0f + 0.25f * float(preset->zoomRatio - 1);
  const float k = zoom * uiScale * std::clamp(p.fontSize / 15.0f, 0.3f, 4.0f);
  const float fontPx = float(std::abs(preset->fontHeight ? preset->fontHeight : -16)) * k;
  const bool bold = preset->fontWeight >= 600;
  const std::wstring family = ToWide(preset->fontFace);

  // Celle della griglia (coordinate negative): carattere del font principale.
  float cellW = fontPx * 0.6f, cellH = fontPx * 1.2f;
  if (IDWriteTextFormat* f = AdvFormat(family, fontPx, bold)) {
    ComPtr<IDWriteTextLayout> probe;
    if (SUCCEEDED(dwrite_->CreateTextLayout(L"0", 1, f, 1000, 1000, &probe))) {
      DWRITE_TEXT_METRICS m;
      probe->GetMetrics(&m);
      cellW = m.widthIncludingTrailingWhitespace;
      cellH = m.height;
    }
  }
  auto ux = [&](double v) { return v < 0 ? float(-v) * cellW : float(v) * k; };
  auto uy = [&](double v) { return v < 0 ? float(-v) * cellH : float(v) * k; };

  const FrameStats* frames = engine.Frames();
  auto frameText = [&](const std::string& tag) -> std::wstring {
    if (!frames) return tag == "FT" ? L"0.0" : L"0";
    if (tag == "FR") return std::format(L"{:.0f}", frames->fps);
    if (tag == "FT") return std::format(L"{:.1f}", frames->frameTimeMs);
    if (tag == "FRAVG") return std::format(L"{:.0f}", frames->fpsAvg);
    if (tag == "FRMIN") return std::format(L"{:.0f}", frames->fpsMin);
    if (tag == "FRMAX") return std::format(L"{:.0f}", frames->fpsMax);
    const double pct = tag == "FR10L" ? 90 : tag == "FR01L" ? 99 : 99.9;
    const double ms = PercentileMs(frames->frameTimes, pct);
    return std::format(L"{:.0f}", ms > 0 ? 1000.0 / ms : 0.0);
  };

  std::vector<PreparedLayer> prepared;
  float maxX = 0, maxY = 0;
  for (const auto& l : preset->layers) {
    if (!l.visibility.empty() && !engine.Truthy(l.visibility)) continue;
    const ArgbColor layerColor = ParseRtssColor(l.colorSpec, engine, {1, 1, 1, 1});
    const float ex = l.extentX ? ux(l.extentX) : 0, ey = l.extentY ? uy(l.extentY) : 0;

    PreparedLayer pl;
    pl.lines.emplace_back();
    ArgbColor color = layerColor;
    // <S=n> è una percentuale assoluta del font principale; <S> torna alla dimensione del layer.
    const float layerPct = float(std::max(l.size, 0));
    float sizePct = layerPct;
    float cursor = 0, lastStart = 0;
    struct Cond {
      bool parent, active, taken;
    };
    std::vector<Cond> conds;
    auto active = [&] { return conds.empty() || conds.back().active; };
    // Grafico: i parametri dell'ultimo <G=sorgente,w,h,margine,min,max,flag> valgono anche per i <G=sorgente> dopo.
    double gw = 0, gh = 0, gmin = 0, gmax = 100;
    int gflags = 0;
    std::string run;

    auto place = [&](Item it) {
      Line& ln = pl.lines.back();
      it.x = cursor;
      lastStart = cursor;
      cursor += it.w;
      if (it.k == Item::K::Text) {
        ln.ascent = std::max(ln.ascent, it.baseline);
        ln.descent = std::max(ln.descent, it.h - it.baseline);
      } else {
        ln.objH = std::max(ln.objH, it.h);
      }
      ln.w = std::max(ln.w, cursor);
      ln.items.push_back(std::move(it));
    };
    auto flush = [&] {
      if (run.empty()) return;
      const std::wstring w = ToWide(run);
      run.clear();
      const float px = fontPx * sizePct / 100.0f;
      if (px < 0.3f) return;
      IDWriteTextFormat* f = AdvFormat(family, px, bold);
      Item it;
      if (!f || FAILED(dwrite_->CreateTextLayout(w.c_str(), UINT32(w.size()), f, 8192, 2048, &it.layout))) return;
      DWRITE_TEXT_METRICS m;
      DWRITE_LINE_METRICS lm;
      UINT32 lines = 0;
      it.layout->GetMetrics(&m);
      it.layout->GetLineMetrics(&lm, 1, &lines);
      it.w = m.widthIncludingTrailingWhitespace;
      it.h = lm.height;
      it.baseline = lm.baseline;
      it.color = color;
      place(std::move(it));
    };

    const std::string& t = l.text;
    for (size_t i = 0; i < t.size();) {
      const char c = t[i];
      if (c == '\\' && i + 1 < t.size() && (t[i + 1] == 'n' || t[i + 1] == 'b' || t[i + 1] == 'r' || t[i + 1] == 't')) {
        const char e = t[i + 1];
        i += 2;
        if (!active()) continue;
        flush();
        if (e == 'n') {
          const Line prev = pl.lines.back();
          pl.lines.emplace_back();
          if (prev.absolute) {  // la riga dopo un <P> continua sotto, nella stessa colonna
            pl.lines.back().absolute = true;
            pl.lines.back().ax = prev.ax;
            pl.lines.back().ay = prev.ay + prev.Height();
          }
          cursor = lastStart = 0;
        } else if (e == 'b') {
          cursor = lastStart;  // l'oggetto dopo si sovrappone al precedente
        } else if (e == 'r') {
          cursor = lastStart = 0;
        } else {
          run += "    ";
        }
        continue;
      }
      if (c == '<') {
        const size_t close = t.find('>', i);
        if (close != std::string::npos) {
          const std::string tag = t.substr(i + 1, close - i - 1);
          const size_t sep = tag.find_first_of("= ");
          const std::string name = ToLowerAscii(tag.substr(0, sep));
          const std::string arg = sep == std::string::npos ? "" : tag.substr(sep + 1);
          static const char* kKnown[] = {"c",     "s",     "b",     "i",     "ai",    "g",   "if",  "else",
                                         "p",     "a",     "fr",    "ft",    "fravg", "frmin", "frmax", "fr10l",
                                         "fr01l", "fr001l", "btime", "exe",   "app",   "api", "res", "arch", "d"};
          const bool known = std::any_of(std::begin(kKnown), std::end(kKnown), [&](const char* s) { return name == s; });
          if (known) {
            i = close + 1;
            if (name == "if") {
              if (Trim(arg).empty()) {
                if (!conds.empty()) conds.pop_back();
              } else {
                flush();
                const bool parent = active();
                const bool cond = parent && engine.Truthy(arg);
                conds.push_back({parent, cond, cond});
              }
              continue;
            }
            if (name == "else") {
              if (!conds.empty()) {
                flush();
                conds.back().active = conds.back().parent && !conds.back().taken;
              }
              continue;
            }
            if (!active()) continue;
            if (name == "p") {  // <P=x,y>: nuova posizione assoluta del cursore
              flush();
              const auto v = Numbers(Split(arg, ','), 0);
              pl.lines.emplace_back();
              pl.lines.back().absolute = true;
              pl.lines.back().ax = v.size() > 0 ? ux(v[0]) : 0;
              pl.lines.back().ay = v.size() > 1 ? uy(v[1]) : 0;
              cursor = lastStart = 0;
            } else if (name == "c") {
              flush();
              color = arg.empty() ? layerColor : ParseRtssColor(arg, engine, layerColor);
            } else if (name == "s") {
              flush();
              sizePct = arg.empty() ? layerPct : std::abs(float(atof(arg.c_str())));
            } else if (name == "b") {
              flush();
              const auto v = Numbers(Split(arg, ','), 0);
              Item it;
              it.k = Item::K::Box;
              it.w = v.size() > 0 && v[0] != 0 ? ux(v[0]) : ex;
              it.h = v.size() > 1 && v[1] != 0 ? uy(v[1]) : ey;
              it.border = v.size() > 2 ? float(v[2]) * k : 0;
              it.color = color;
              place(std::move(it));
            } else if (name == "i") {  // <I=w,h,x,y,larghezza,altezza[,angolo]> dallo sprite sheet
              flush();
              const auto v = Numbers(Split(arg, ','), 0);
              if (v.size() >= 6) {
                Item it;
                it.k = Item::K::Image;
                it.src = D2D1::RectF(float(v[2]), float(v[3]), float(v[2] + v[4]), float(v[3] + v[5]));
                it.w = v[0] != 0 ? ux(v[0]) : ex ? ex : float(v[4]) * k;
                it.h = v[1] != 0 ? uy(v[1]) : ey ? ey : float(v[5]) * k;
                it.angle = v.size() > 6 ? float(v[6]) : 0;
                it.color = color;
                place(std::move(it));
              }
            } else if (name == "ai") {  // <AI=sorgente,w,h,min,max,x,y,lf,af,fotogrammi,colonne,?,angolo min,angolo max>
              flush();
              const auto parts = Split(arg, ',');
              const auto v = Numbers(parts, 1);
              if (v.size() >= 10) {
                const auto val = engine.Value(parts[0]);
                const double mn = v[2], mx = v[3];
                const double tnorm = val.ok && mx > mn ? std::clamp((val.v - mn) / (mx - mn), 0.0, 1.0) : 0.0;
                const int framesN = std::max(1, int(v[8])), cols = std::max(1, int(v[9]));
                const int fi = std::clamp(int(tnorm * framesN), 0, framesN - 1);
                Item it;
                it.k = Item::K::Image;
                const float sx = float(v[4] + (fi % cols) * v[6]), sy = float(v[5] + (fi / cols) * v[7]);
                it.src = D2D1::RectF(sx, sy, sx + float(v[6]), sy + float(v[7]));
                it.w = v[0] != 0 ? ux(v[0]) : ex ? ex : float(v[6]) * k;
                it.h = v[1] != 0 ? uy(v[1]) : ey ? ey : float(v[7]) * k;
                if (v.size() >= 13 && (v[11] != 0 || v[12] != 0)) it.angle = float(v[11] + tnorm * (v[12] - v[11]));
                it.color = color;
                place(std::move(it));
              }
            } else if (name == "g") {  // grafico / barra di una sorgente
              flush();
              auto parts = Split(arg, ',');
              std::string src = Trim(parts[0]);
              if (src.size() >= 2 && src.front() == '"') src = src.substr(1, src.size() - 2);
              const auto v = Numbers(parts, 1);
              if (v.size() >= 6) {
                gw = v[0];
                gh = v[1];
                gmin = v[3];
                gmax = v[4];
                gflags = int(v[5]);
              }
              Item it;
              it.k = Item::K::Graph;
              it.w = gw != 0 ? ux(gw) : ex;
              it.h = gh != 0 ? uy(gh) : ey;
              it.source = src;
              it.gmin = float(gmin);
              it.gmax = float(gmax);
              it.gflags = gflags;
              it.color = color;
              place(std::move(it));
            } else if (name == "fr" || name == "ft" || name == "fravg" || name == "frmin" || name == "frmax" ||
                       name == "fr10l" || name == "fr01l" || name == "fr001l") {
              std::string up = name;
              for (auto& ch : up) ch = char(toupper(uint8_t(ch)));
              run += ToUtf8(frameText(up));
            } else if (name == "btime") {
              // nessun benchmark in corso: RTSS non mostra il tempo
            } else if (name == "api" || name == "app") {
              run += frames ? frames->api : "";
            } else if (name == "res") {
              run += std::format("{}x{}", anchor.right - anchor.left, anchor.bottom - anchor.top);
            }
            // <P>, <A>, <D>, <EXE>, <ARCH>: ignorati
            continue;
          }
        }
      }
      if (c == '%') {
        const size_t close = t.find('%', i + 1);
        if (close != std::string::npos && close > i + 1) {
          const std::string name = t.substr(i + 1, close - i - 1);
          const std::wstring hw = HardwareName(name, engine.System());
          if (!hw.empty()) {
            if (active()) run += ToUtf8(hw);
            i = close + 1;
            continue;
          }
          if (name.find('<') == std::string::npos && engine.Knows(name)) {
            if (active()) run += ToUtf8(engine.Formatted(name));
            i = close + 1;
            continue;
          }
        }
      }
      if (active()) run += c;
      ++i;
    }
    flush();

    float cw = 0, ch = 0, absMaxX = 0, absMaxY = 0;
    for (const auto& ln : pl.lines) {
      if (ln.absolute) {
        absMaxX = std::max(absMaxX, ln.ax + ln.w);
        absMaxY = std::max(absMaxY, ln.ay + ln.Height());
        continue;
      }
      cw = std::max(cw, ln.w);
      ch += ln.Height();
    }
    const float px = ux(l.x), py = uy(l.y);
    const float bw = ex ? ex : cw, bh = ey ? ey : ch;
    const int origin = std::clamp(l.origin, 0, 8), col = origin % 3, row = origin / 3;
    pl.x = px + (col == 0 ? 0 : col == 1 ? (bw - cw) / 2 : bw - cw);
    pl.y = py + float(l.marginTop) * k + (row == 0 ? 0 : row == 1 ? (bh - ch) / 2 : bh - ch);
    if (!l.bgColor.empty()) {
      pl.bgColor = ParseRtssColor(l.bgColor, engine, {0, 0, 0, 0});
      pl.bg = D2D1::RectF(px, py, px + bw, py + bh);
    }
    maxX = std::max({maxX, px + bw, pl.x + cw, absMaxX});
    maxY = std::max({maxY, py + bh, pl.y + ch, absMaxY});
    prepared.push_back(std::move(pl));
  }
  if (prepared.empty() || maxX <= 0 || maxY <= 0) {
    Show(false);
    return;
  }

  // Posizione dell'intero overlay (angolo scelto nel profilo).
  const UINT w = UINT(std::ceil(maxX)) + 2, h = UINT(std::ceil(maxY)) + 2;
  const int margin = int(p.margin * uiScale);
  int x = anchor.left + margin, y = anchor.top + margin;
  if (p.position == "top-right" || p.position == "bottom-right") x = anchor.right - int(w) - margin;
  if (p.position == "bottom-left" || p.position == "bottom-right") y = anchor.bottom - int(h) - margin;
  if (p.position == "custom") {
    x = anchor.left + int(p.x * uiScale);
    y = anchor.top + int(p.y * uiScale);
  }
  x = std::clamp(x, int(anchor.left), std::max(int(anchor.left), int(anchor.right) - int(w)));
  y = std::clamp(y, int(anchor.top), std::max(int(anchor.top), int(anchor.bottom) - int(h)));
  if (!BeginFrame(x, y, w, h)) return;

  ID2D1Bitmap1* sheet = PresetImage(ToWide(preset->image));
  if (sheet && !tint_) dc_->CreateEffect(CLSID_D2D1ColorMatrix, &tint_);
  if (tint_) tint_->SetInput(0, sheet);
  D2D1_MATRIX_3X2_F base;
  dc_->GetTransform(&base);

  for (const auto& pl : prepared) {
    if (pl.bgColor.a > 0) {
      brush_->SetColor(D(pl.bgColor));
      dc_->FillRectangle(pl.bg, brush_.Get());
    }
    float flowTop = pl.y;
    for (const auto& ln : pl.lines) {
      const float lineX = ln.absolute ? ln.ax : pl.x;
      const float lineTop = ln.absolute ? ln.ay : flowTop;
      for (const auto& it : ln.items) {
        const float ix = lineX + it.x;
        const float top = lineTop;
        if (it.color.a <= 0.003f) continue;
        switch (it.k) {
          case Item::K::Text: {
            const float ty = top + ln.ascent - it.baseline;
            brush_->SetColor(D(it.color));
            dc_->DrawTextLayout({ix, ty}, it.layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            break;
          }
          case Item::K::Box: {
            brush_->SetColor(D(it.color));
            const auto r = D2D1::RectF(ix, top, ix + it.w, top + it.h);
            if (it.border > 0) {
              const float hb = it.border / 2;
              dc_->DrawRectangle(D2D1::RectF(r.left + hb, r.top + hb, r.right - hb, r.bottom - hb), brush_.Get(),
                                 it.border);
            } else {
              dc_->FillRectangle(r, brush_.Get());
            }
            break;
          }
          case Item::K::Image: {
            if (!sheet || !tint_ || it.w <= 0 || it.h <= 0) break;
            // Sprite bianchi colorati col colore corrente (moltiplicazione, alfa compreso).
            tint_->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                            D2D1::Matrix5x4F(it.color.r, 0, 0, 0, 0, it.color.g, 0, 0, 0, 0, it.color.b, 0, 0, 0, 0,
                                             it.color.a, 0, 0, 0, 0));
            const float sw = it.src.right - it.src.left, sh = it.src.bottom - it.src.top;
            const auto m = D2D1::Matrix3x2F::Scale(it.w / sw, it.h / sh) *
                           D2D1::Matrix3x2F::Rotation(it.angle, D2D1::Point2F(it.w / 2, it.h / 2)) *
                           D2D1::Matrix3x2F::Translation(ix, top) * base;
            dc_->SetTransform(m);
            dc_->DrawImage(tint_.Get(), D2D1::Point2F(0, 0), it.src, D2D1_INTERPOLATION_MODE_LINEAR);
            dc_->SetTransform(base);
            break;
          }
          case Item::K::Graph: {
            if (it.w <= 0 || it.h <= 0) break;
            brush_->SetColor(D(it.color));
            const auto rect = D2D1::RectF(ix, top, ix + it.w, top + it.h);
            float mn = it.gmin, mx = it.gmax;
            if (it.gflags & 8) {  // barra (es. barchart dei core CPU)
              const auto v = engine.Value(it.source);
              double val = v.ok ? v.v : 0;
              if (mx <= 1.0f && val > 1.0) val /= 100.0;  // sorgenti in % su scala 0..1
              const float tn = mx > mn ? float(std::clamp((val - mn) / (mx - mn), 0.0, 1.0)) : 0.0f;
              if (it.h >= it.w)
                dc_->FillRectangle(D2D1::RectF(rect.left, rect.bottom - it.h * tn, rect.right, rect.bottom), brush_.Get());
              else
                dc_->FillRectangle(D2D1::RectF(rect.left, rect.top, rect.left + it.w * tn, rect.bottom), brush_.Get());
              break;
            }
            const auto* hist = engine.History(it.source);
            if (!hist || hist->size() < 2) break;
            if (mx <= mn) {
              mn = *std::min_element(hist->begin(), hist->end());
              mx = *std::max_element(hist->begin(), hist->end());
              if (mx <= mn) mx = mn + 1;
            }
            const size_t n = std::min<size_t>(hist->size(), size_t(std::max(2.0f, it.w / std::max(1.0f, k))));
            const float step = it.w / float(n - 1);
            D2D1_POINT_2F prev{};
            for (size_t j = 0; j < n; ++j) {
              const float v = (*hist)[hist->size() - n + j];
              const float tn = std::clamp((v - mn) / (mx - mn), 0.0f, 1.0f);
              const D2D1_POINT_2F pt{rect.left + step * float(j), rect.bottom - tn * it.h};
              if (j) dc_->DrawLine(prev, pt, brush_.Get(), std::max(1.0f, k * 0.8f));
              prev = pt;
            }
            break;
          }
        }
      }
      if (!ln.absolute) flowTop += ln.Height();
    }
  }
  EndFrame();
}

}  // namespace po
