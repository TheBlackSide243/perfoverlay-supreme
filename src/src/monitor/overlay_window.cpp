#include "monitor/overlay_window.h"

#include <algorithm>
#include <cmath>

#include "common/log.h"
#include "common/util.h"

using Microsoft::WRL::ComPtr;

namespace po {
namespace {

constexpr wchar_t kOverlayClass[] = L"PerfOverlaySupremeWindow";

D2D1_COLOR_F ToD2D(Color c, float alpha = 1.0f) {
  return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * alpha};
}

std::wstring ResolveFamily(const std::string& f) {
  if (IEquals(f, "mono")) return L"Consolas";
  if (IEquals(f, "sans")) return L"Segoe UI";
  return ToWide(f);
}

struct Laid {
  const Segment* seg = nullptr;
  ComPtr<IDWriteTextLayout> layout;
  float w = 0, h = 0, baseline = 0;
};
struct LaidRow {
  std::vector<Laid> items;
  float w = 0, ascent = 0, descent = 0;
};

}  // namespace

OverlayWindow::~OverlayWindow() {
  ReleaseDevice();
  if (hwnd_) DestroyWindow(hwnd_);
}

bool OverlayWindow::Create(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = inst;
  wc.lpszClassName = kOverlayClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);

  // TRANSPARENT + LAYERED: i click passano al gioco. NOACTIVATE: non ruba mai il focus.
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED |
                              WS_EX_NOREDIRECTIONBITMAP,
                          kOverlayClass, L"PerfOverlay Supreme", WS_POPUP, 0, 0, int(width_), int(height_), nullptr, nullptr,
                          inst, nullptr);
  if (!hwnd_) {
    LogError("Creazione finestra overlay fallita ({})", GetLastError());
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
    LogError("DirectWrite non disponibile");
    return false;
  }
  return InitDevice();
}

bool OverlayWindow::InitDevice() {
  ReleaseDevice();
  constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 &d3d_, nullptr, nullptr);
  if (FAILED(hr))
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d_,
                           nullptr, nullptr);
  if (FAILED(hr)) {
    LogError("D3D11CreateDevice fallito (0x{:08X})", unsigned(hr));
    return false;
  }
  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(d3d_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
    return false;

  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Width = width_;
  d.Height = height_;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  if (FAILED(hr = factory->CreateSwapChainForComposition(d3d_.Get(), &d, nullptr, &swap_))) {
    LogError("CreateSwapChainForComposition fallito (0x{:08X})", unsigned(hr));
    return false;
  }

  D2D1_FACTORY_OPTIONS opts{};
  ComPtr<ID2D1Device> d2dDevice;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, d2dFactory_.GetAddressOf())) ||
      FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice)) ||
      FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)))
    return false;
  dc_->SetDpi(96.0f, 96.0f);  // unità = pixel fisici
  dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType non funziona su sfondo trasparente
  dc_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_);

  if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_))) ||
      FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_)) || FAILED(dcomp_->CreateVisual(&visual_)))
    return false;
  visual_->SetContent(swap_.Get());
  target_->SetRoot(visual_.Get());
  dcomp_->Commit();
  return true;
}

void OverlayWindow::ReleaseDevice() {
  images_.clear();  // bitmap e effetti appartengono al device
  tint_.Reset();
  if (dc_) dc_->SetTarget(nullptr);
  brush_.Reset();
  visual_.Reset();
  target_.Reset();
  dcomp_.Reset();
  dc_.Reset();
  d2dFactory_.Reset();
  swap_.Reset();
  d3d_.Reset();
}

void OverlayWindow::Show(bool visible) {
  if (!hwnd_ || visible == visible_) return;
  visible_ = visible;
  ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool OverlayWindow::EnsureSize(UINT w, UINT h) {
  if (w == width_ && h == height_) return true;
  dc_->SetTarget(nullptr);
  if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
  width_ = w;
  height_ = h;
  return true;
}

IDWriteTextFormat* OverlayWindow::Format(float px, bool bold) {
  const int key = int(std::lround(px * 4.0f)) * 2 + (bold ? 1 : 0);
  if (auto it = formats_.find(key); it != formats_.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> f;
  if (FAILED(dwrite_->CreateTextFormat(family_.c_str(), nullptr,
                                       bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", &f)))
    return nullptr;
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  formats_[key] = f;
  return f.Get();
}

namespace {

// Impagina una riga di segmenti (testo DirectWrite o grafico) alla dimensione "base" in pixel.
template <class FormatFn>
LaidRow LayoutRow(IDWriteFactory* dw, FormatFn&& format, const Row& row, float base, bool bold) {
  LaidRow lr;
  for (const auto& seg : row) {
    Laid it;
    it.seg = &seg;
    if (seg.kind == Segment::Kind::Graph) {
      it.w = base * seg.scale * 3.6f;
      it.h = base * seg.scale * 0.9f;
      it.baseline = it.h;  // appoggiato sulla linea di base del testo
    } else {
      IDWriteTextFormat* fmt = format(base * seg.scale, seg.bold || bold);
      if (!fmt || FAILED(dw->CreateTextLayout(seg.text.c_str(), UINT32(seg.text.size()), fmt, 8192.0f, 2048.0f,
                                              &it.layout)))
        continue;
      DWRITE_TEXT_METRICS m;
      DWRITE_LINE_METRICS lm;
      UINT32 lines = 0;
      it.layout->GetMetrics(&m);
      it.layout->GetLineMetrics(&lm, 1, &lines);
      it.w = m.widthIncludingTrailingWhitespace;
      it.h = lm.height;
      it.baseline = lm.baseline;
    }
    lr.w += it.w;
    lr.ascent = std::max(lr.ascent, it.baseline);
    lr.descent = std::max(lr.descent, it.h - it.baseline);
    lr.items.push_back(std::move(it));
  }
  return lr;
}

// Sfondo arrotondato (e bordo neon) di un blocco.
void DrawBox(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, float base, float uiScale,
             const Profile& p) {
  const auto box = D2D1::RoundedRect(r, base * 0.3f, base * 0.3f);
  const float bgAlpha = p.bgOpacity / 100.0f;
  if (p.style != "minimal" && bgAlpha > 0) {
    br->SetColor(ToD2D(p.colors.background, bgAlpha));
    dc->FillRoundedRectangle(box, br);
  }
  if (p.style == "neon") {
    br->SetColor(ToD2D(p.colors.fps, 0.85f));
    dc->DrawRoundedRectangle(box, br, std::max(1.0f, uiScale * 1.5f));
  }
}

// Disegna una riga già impaginata con l'angolo superiore sinistro in (x, rowTop).
void DrawRow(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const LaidRow& r, float x, float rowTop, float base,
             float uiScale, const Profile& p) {
  const bool minimal = p.style == "minimal";
  const bool neon = p.style == "neon";
  float cx = x;
  for (const auto& it : r.items) {
    const Segment& seg = *it.seg;
    const float top = rowTop + r.ascent - it.baseline;
    if (seg.kind == Segment::Kind::Graph) {
      const float gx = cx + base * 0.2f, gw = it.w - base * 0.4f;
      br->SetColor(ToD2D(p.colors.text, 0.18f));
      dc->DrawLine({gx, top + it.h}, {gx + gw, top + it.h}, br, 1.0f);
      const auto& v = seg.graph;
      if (v.size() >= 2) {
        const float maxV = std::max(1.0f, *std::max_element(v.begin(), v.end()) * 1.15f);
        br->SetColor(ToD2D(seg.color, neon ? 1.0f : 0.95f));
        const float step = gw / float(v.size() - 1);
        for (size_t i = 1; i < v.size(); ++i) {
          const D2D1_POINT_2F a{gx + step * float(i - 1), top + it.h - it.h * v[i - 1] / maxV};
          const D2D1_POINT_2F b{gx + step * float(i), top + it.h - it.h * v[i] / maxV};
          dc->DrawLine(a, b, br, std::max(1.0f, uiScale * 1.2f));
        }
      }
    } else if (it.layout) {
      if (minimal) {  // ombra per leggibilità senza sfondo
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.85f));
        dc->DrawTextLayout({cx + uiScale, top + uiScale}, it.layout.Get(), br);
      } else if (neon) {  // alone
        br->SetColor(ToD2D(seg.color, 0.30f));
        for (const auto& o : {D2D1_POINT_2F{-1, 0}, D2D1_POINT_2F{1, 0}, D2D1_POINT_2F{0, -1}, D2D1_POINT_2F{0, 1}})
          dc->DrawTextLayout({cx + o.x * uiScale, top + o.y * uiScale}, it.layout.Get(), br);
      }
      br->SetColor(ToD2D(seg.color));
      dc->DrawTextLayout({cx, top}, it.layout.Get(), br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    cx += it.w;
  }
}

}  // namespace

float OverlayWindow::UiScale(const Profile& p, const RECT& anchor) const {
  // Con autoScale le dimensioni del profilo sono riferite a 1080p e seguono l'altezza
  // dell'area del gioco (720p → ×0.67, 1440p → ×1.33, 4K → ×2). Altrimenti seguono i DPI del monitor.
  const float anchorH = float(anchor.bottom - anchor.top);
  const UINT dpi = GetDpiForWindow(hwnd_);
  return p.autoScale && anchorH > 0 ? std::clamp(anchorH / float(Profile::kReferenceHeight), 0.5f, 4.0f)
                                    : (dpi ? dpi : 96) / 96.0f;
}

bool OverlayWindow::Prepare(const Profile& p) {
  if (!hwnd_ || !dwrite_) return false;
  if (!dc_ && !InitDevice()) return false;
  if (const auto fam = ResolveFamily(p.fontFamily); fam != family_) {
    family_ = fam;
    formats_.clear();
  }
  return true;
}

bool OverlayWindow::BeginFrame(int x, int y, UINT w, UINT h) {
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, int(w), int(h), SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  if (!EnsureSize(w, h)) {
    InitDevice();
    return false;
  }
  ComPtr<IDXGISurface> surface;
  const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  targetBitmap_.Reset();
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface))) ||
      FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &targetBitmap_)))
    return false;
  dc_->SetTarget(targetBitmap_.Get());
  dc_->BeginDraw();
  dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
  return true;
}

void OverlayWindow::EndFrame() {
  const HRESULT hr = dc_->EndDraw();
  dc_->SetTarget(nullptr);
  targetBitmap_.Reset();
  if (hr == D2DERR_RECREATE_TARGET) {
    LogWarn("Device grafico perso, ricreazione");
    InitDevice();
    return;
  }
  swap_->Present(1, 0);
}

void OverlayWindow::Render(const std::vector<Row>& rows, const Profile& p, const RECT& anchor) {
  if (!Prepare(p)) return;
  const float uiScale = UiScale(p, anchor);
  const float base = p.fontSize * uiScale;
  auto format = [this](float px, bool bold) { return Format(px, bold); };

  std::vector<LaidRow> laid;
  for (const auto& row : rows) {
    LaidRow lr = LayoutRow(dwrite_.Get(), format, row, base, p.bold);
    if (!lr.items.empty()) laid.push_back(std::move(lr));
  }
  if (laid.empty()) {
    Show(false);
    return;
  }

  const float pad = base * 0.45f;
  const float rowGap = base * 0.15f;
  float contentW = 0, contentH = 0;
  for (const auto& r : laid) {
    contentW = std::max(contentW, r.w);
    contentH += r.ascent + r.descent;
  }
  contentH += rowGap * float(laid.size() - 1);
  const UINT w = UINT(std::ceil(contentW + 2 * pad));
  const UINT h = UINT(std::ceil(contentH + 2 * pad));

  // Posizione rispetto al gioco (o al monitor in modalità compatta).
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
  DrawBox(dc_.Get(), brush_.Get(), D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), base, uiScale, p);
  float rowTop = pad;
  for (const auto& r : laid) {
    DrawRow(dc_.Get(), brush_.Get(), r, pad, rowTop, base, uiScale, p);
    rowTop += r.ascent + r.descent + rowGap;
  }
  EndFrame();
}

void OverlayWindow::RenderFree(const std::vector<FreeBlock>& blocks, const Profile& p, const RECT& anchor,
                               int canvasWidth) {
  if (!Prepare(p)) return;
  const float uiScale = UiScale(p, anchor);
  auto format = [this](float px, bool bold) { return Format(px, bold); };
  // La tela dell'editor (canvasWidth x 1080) viene riportata in proporzione sull'area del gioco.
  const float aw = float(anchor.right - anchor.left), ah = float(anchor.bottom - anchor.top);
  const float sx = aw / float(std::max(1, canvasWidth)), sy = ah / float(kLayoutRefH);

  struct Placed {
    LaidRow row;
    float base, pad, x, y, w, h;  // coordinate schermo
  };
  std::vector<Placed> placed;
  float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
  for (const auto& b : blocks) {
    const float base = p.fontSize * uiScale * b.scale;
    LaidRow lr = LayoutRow(dwrite_.Get(), format, b.row, base, p.bold);
    if (lr.items.empty()) continue;
    const float pad = base * 0.35f;
    const float w = std::ceil(lr.w + 2 * pad), h = std::ceil(lr.ascent + lr.descent + 2 * pad);
    float x = float(anchor.left) + std::round(b.x * sx);
    float y = float(anchor.top) + std::round(b.y * sy);
    x = std::clamp(x, float(anchor.left), std::max(float(anchor.left), float(anchor.right) - w));
    y = std::clamp(y, float(anchor.top), std::max(float(anchor.top), float(anchor.bottom) - h));
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    maxX = std::max(maxX, x + w);
    maxY = std::max(maxY, y + h);
    placed.push_back({std::move(lr), base, pad, x, y, w, h});
  }
  if (placed.empty()) {
    Show(false);
    return;
  }

  // Una sola finestra trasparente grande quanto l'insieme dei blocchi.
  const int wx = int(std::floor(minX)), wy = int(std::floor(minY));
  const UINT w = UINT(std::ceil(maxX) - wx), h = UINT(std::ceil(maxY) - wy);
  if (!BeginFrame(wx, wy, w, h)) return;
  for (const auto& pl : placed) {
    const float x = pl.x - wx, y = pl.y - wy;
    DrawBox(dc_.Get(), brush_.Get(), D2D1::RectF(x + 0.5f, y + 0.5f, x + pl.w - 0.5f, y + pl.h - 0.5f), pl.base,
            uiScale, p);
    DrawRow(dc_.Get(), brush_.Get(), pl.row, x + pl.pad, y + pl.pad, pl.base, uiScale, p);
  }
  EndFrame();
}

// ---------------------------------------------------------------- preset RTSS
std::wstring OverlayWindow::ResolveRtssFont(const std::string& face) {
  // Il font del preset se installato (es. Unispace), altrimenti un monospazio sicuro.
  const std::wstring wanted = ToWide(face);
  ComPtr<IDWriteFontCollection> coll;
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (!wanted.empty() && SUCCEEDED(dwrite_->GetSystemFontCollection(&coll)) &&
      SUCCEEDED(coll->FindFamilyName(wanted.c_str(), &index, &exists)) && exists)
    return wanted;
  return L"Consolas";
}

void OverlayWindow::RenderRtss(const std::vector<RtssDrawLayer>& layers, const RtssLayout& preset, const Profile& p,
                               const RECT& anchor, const std::vector<Row>& extra) {
  if (!Prepare(p)) return;
  if (const auto fam = ResolveRtssFont(preset.fontFace); fam != family_) {
    family_ = fam;
    formats_.clear();
  }
  const float uiScale = UiScale(p, anchor);
  const float base = p.fontSize * uiScale;
  const bool bold = p.bold || preset.fontWeight >= 600;

  // Cella della griglia a caratteri: larghezza di "0" e altezza di riga del font monospazio.
  float cellW = base * 0.6f, cellH = base * 1.2f;
  if (IDWriteTextFormat* f = Format(base, bold)) {
    ComPtr<IDWriteTextLayout> probe;
    if (SUCCEEDED(dwrite_->CreateTextLayout(L"0", 1, f, 1000.0f, 1000.0f, &probe))) {
      DWRITE_TEXT_METRICS m;
      probe->GetMetrics(&m);
      cellW = m.widthIncludingTrailingWhitespace;
      cellH = m.height;
    }
  }
  // Valori negativi = celle di testo, positivi = pixel (a 1080p, scalati come il resto).
  auto unitX = [&](int v) { return v < 0 ? -v * cellW : v * uiScale; };
  auto unitY = [&](int v) { return v < 0 ? -v * cellH : v * uiScale; };

  struct Placed {
    ComPtr<IDWriteTextLayout> layout;
    Color color;
    float x, y;
  };
  std::vector<Placed> placed;
  // Origine fissa in (0,0) come l'OSD di RTSS: spostare un testo non sposta gli altri.
  float minX = 0, minY = 0, maxX = 0, maxY = 0;
  for (const auto& l : layers) {
    if (l.text.empty()) continue;
    IDWriteTextFormat* f = Format(base * std::clamp(l.size, 10, 1000) / 100.0f, bold);
    ComPtr<IDWriteTextLayout> tl;
    if (!f || FAILED(dwrite_->CreateTextLayout(l.text.c_str(), UINT32(l.text.size()), f, 8192.0f, 2048.0f, &tl)))
      continue;
    DWRITE_TEXT_METRICS m;
    tl->GetMetrics(&m);
    const float tw = m.widthIncludingTrailingWhitespace, th = m.height;
    const float px = unitX(l.x), py = unitY(l.y);
    const float bw = l.extentX ? unitX(l.extentX) : tw;
    const float bh = l.extentY ? unitY(l.extentY) : th;
    // Allineamento nel riquadro: origin = riga*3 + colonna (0 = in alto a sinistra, 4 = centro).
    const int origin = std::clamp(l.origin, 0, 8), col = origin % 3, row = origin / 3;
    const float tx = px + (col == 0 ? 0 : col == 1 ? (bw - tw) / 2 : bw - tw);
    const float ty = py + (row == 0 ? 0 : row == 1 ? (bh - th) / 2 : bh - th);
    minX = std::min({minX, px, tx});
    minY = std::min({minY, py, ty});
    maxX = std::max({maxX, px + bw, tx + tw});
    maxY = std::max({maxY, py + bh, ty + th});
    placed.push_back({std::move(tl), l.color, tx, ty});
  }
  // Righe extra in colonna sotto il preset, allineate al suo bordo sinistro.
  auto format = [this](float px, bool b) { return Format(px, b); };
  std::vector<LaidRow> extraLaid;
  for (const auto& row : extra) {
    LaidRow lr = LayoutRow(dwrite_.Get(), format, row, base, bold);
    if (!lr.items.empty()) extraLaid.push_back(std::move(lr));
  }
  if (placed.empty() && extraLaid.empty()) {
    Show(false);
    return;
  }
  if (placed.empty()) minX = minY = maxX = maxY = 0;
  const float extraTop = placed.empty() ? 0.0f : maxY + base * 0.2f;
  float extraY = extraTop;
  for (const auto& r : extraLaid) {
    maxX = std::max(maxX, minX + r.w);
    extraY += r.ascent + r.descent;
  }
  maxY = std::max(maxY, extraY);

  const float pad = base * 0.4f;
  const UINT w = UINT(std::ceil(maxX - minX + 2 * pad)), h = UINT(std::ceil(maxY - minY + 2 * pad));
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
  DrawBox(dc_.Get(), brush_.Get(), D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), base, uiScale, p);
  const bool neon = p.style == "neon";
  for (const auto& pl : placed) {
    const D2D1_POINT_2F at{pl.x - minX + pad, pl.y - minY + pad};
    // Ombra come l'OSD di RTSS (sempre, per leggibilità su qualsiasi sfondo)
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.85f));
    dc_->DrawTextLayout({at.x + uiScale, at.y + uiScale}, pl.layout.Get(), brush_.Get());
    if (neon) {
      brush_->SetColor(ToD2D(pl.color, 0.30f));
      for (const auto& o : {D2D1_POINT_2F{-1, 0}, D2D1_POINT_2F{1, 0}, D2D1_POINT_2F{0, -1}, D2D1_POINT_2F{0, 1}})
        dc_->DrawTextLayout({at.x + o.x * uiScale, at.y + o.y * uiScale}, pl.layout.Get(), brush_.Get());
    }
    brush_->SetColor(ToD2D(pl.color));
    dc_->DrawTextLayout(at, pl.layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  }
  if (!extraLaid.empty()) {
    Profile shadowed = p;  // ombra come il testo del preset (o alone nello stile neon)
    if (!neon) shadowed.style = "minimal";
    float rowTop = extraTop - minY + pad;
    for (const auto& r : extraLaid) {
      DrawRow(dc_.Get(), brush_.Get(), r, pad, rowTop, base, uiScale, shadowed);
      rowTop += r.ascent + r.descent;
    }
  }
  EndFrame();
}

}  // namespace po
