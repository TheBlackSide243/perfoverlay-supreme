#include "settings/overlay_preview.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "common/i18n.h"
#include "common/sensor_feed.h"
#include "monitor/data/battery.h"
#include "monitor/overlay_content.h"
#include "monitor/overlay_window.h"
#include "settings/theme.h"

using Microsoft::WRL::ComPtr;

namespace po::ui {
namespace {

struct Preview {
  Profile profile;
  SystemSnapshot system;
  FrameStats frames;
  ComPtr<ID2D1DCRenderTarget> rt;
};

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;

bool Factories() {
  if (!g_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf()))) return false;
  if (!g_dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()))))
    return false;
  return true;
}

D2D1_COLOR_F Rgb(COLORREF c, float a = 1.0f) {
  return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
}

// Valori d'esempio: una partita a circa 117 FPS su un PC di fascia media.
void FillSample(Preview& pv) {
  FrameStats& f = pv.frames;
  f.valid = true;
  f.fps = 117;
  f.fpsMin = 96;
  f.fpsMax = 131;
  f.fpsAvg = 115;
  f.frameTimeMs = 8.5;
  f.p95Ms = 10.2;
  f.p99Ms = 12.4;
  f.stutterPct = 0.4;
  f.history.clear();
  for (int i = 0; i < 60; ++i) {
    const float dip = i > 34 && i < 40 ? -14.0f : 0.0f;
    f.history.push_back(114.0f + 9.0f * std::sin(i * 0.33f) + 5.0f * std::sin(i * 1.27f) + dip);
  }
  SystemSnapshot& s = pv.system;
  s.ready = true;
  s.cpu.usage = 34;
  s.cpu.topCoreUsage = 71;
  s.cpu.topCore = 3;
  s.cpu.freqGHz = 4.35;
  s.cpu.maxFreqGHz = 5.10;
  s.cpuTempC = 62;
  s.gpu.usage = 97;
  s.gpu.tempC = 68;
  s.gpu.clockMHz = 2100;
  s.gpu.vramUsedGB = 6.2;
  s.gpu.vramTotalGB = 8.0;
  s.gpu.gameVramGB = 5.1;
  s.ram.usedGB = 14.8;
  s.ram.totalGB = 32.0;
  s.ram.speedMTs = 6000;
}

float Scale(HWND h) {
  const auto v = reinterpret_cast<uintptr_t>(GetPropW(GetParent(h), kScaleProp));
  return v ? float(v) / 1000.0f : float(GetDpiForWindow(h)) / 96.0f;
}

void DrawLabel(ID2D1RenderTarget* rt, const std::wstring& text, const D2D1_RECT_F& r, float px,
               DWRITE_TEXT_ALIGNMENT h, DWRITE_PARAGRAPH_ALIGNMENT v, const D2D1_COLOR_F& color) {
  ComPtr<IDWriteTextFormat> fmt;
  if (FAILED(g_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, px, L"", &fmt)))
    return;
  fmt->SetTextAlignment(h);
  fmt->SetParagraphAlignment(v);
  ComPtr<ID2D1SolidColorBrush> br;
  if (FAILED(rt->CreateSolidColorBrush(color, &br))) return;
  rt->DrawText(text.c_str(), UINT32(text.size()), fmt.Get(), r, br.Get());
}

// Finta scena di gioco: sfumatura con qualche alone chiaro, così si vede quanto è trasparente lo sfondo.
void DrawScene(ID2D1RenderTarget* rt, float w, float h) {
  const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0x1F3A5C)}, {0.55f, D2D1::ColorF(0x3B3A6B)},
                                      {1.0f, D2D1::ColorF(0x7A3E5E)}};
  ComPtr<ID2D1GradientStopCollection> coll;
  ComPtr<ID2D1LinearGradientBrush> grad;
  if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, &coll)) &&
      SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {w, h}), coll.Get(), &grad)))
    rt->FillRectangle(D2D1::RectF(0, 0, w, h), grad.Get());
  struct Glow {
    float x, y, r;
    D2D1_COLOR_F c;
  };
  const Glow glows[] = {{0.16f, 0.25f, 0.95f, D2D1::ColorF(0x78C8FF, 0.16f)},
                        {0.56f, 1.15f, 1.25f, D2D1::ColorF(0xFFAA5A, 0.22f)},
                        {0.88f, 0.05f, 0.85f, D2D1::ColorF(0xFFF0C8, 0.15f)}};
  ComPtr<ID2D1SolidColorBrush> br;
  if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &br))) return;
  for (const Glow& g : glows) {
    br->SetColor(g.c);
    rt->FillEllipse(D2D1::Ellipse({w * g.x, h * g.y}, h * g.r, h * g.r), br.Get());
  }
}

void Draw(HWND hwnd, Preview& pv, const RECT& rc) {
  ID2D1RenderTarget* rt = pv.rt.Get();
  const float w = float(rc.right), h = float(rc.bottom), ui = Scale(hwnd);
  rt->Clear(Rgb(col::Panel));

  // La scena ha gli angoli arrotondati come gli altri riquadri.
  ComPtr<ID2D1RoundedRectangleGeometry> clip;
  ComPtr<ID2D1Layer> layer;
  const bool clipped =
      SUCCEEDED(g_d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), 8 * ui, 8 * ui), &clip)) &&
      SUCCEEDED(rt->CreateLayer(&layer));
  if (clipped) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
  DrawScene(rt, w, h);

  const Profile& p = pv.profile;
  const D2D1_COLOR_F soft = D2D1::ColorF(D2D1::ColorF::White, 0.72f);
  const float inset = 10 * ui;
  const D2D1_RECT_F inner = D2D1::RectF(inset, inset * 0.6f, w - inset, h - inset * 0.6f);
  if (p.layout != "bar" && p.layout != "vertical") {
    DrawLabel(rt, p.layout == "rtss" ? T(L"Preset RTSS: occupa tutto lo schermo, lo vedi nell'Editor o con la prova dell'overlay.")
                                     : T(L"Layout libero: occupa tutto lo schermo, lo vedi e lo sistemi nell'Editor."),
              inner, 12.5f * ui, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, soft);
  } else {
    ContentInput in;
    in.profile = &p;
    in.frames = &pv.frames;
    in.system = &pv.system;
    const std::vector<Row> rows = BuildRows(in);
    const float margin = std::max(0.0f, float(p.margin)) * ui;
    D2D1_SIZE_F size = DrawRowsPreview(nullptr, g_dwrite.Get(), rows, p, ui, 0, 0);
    if (size.width <= 0) {
      DrawLabel(rt, T(L"Nessun elemento visibile: accendine almeno uno in Elementi e sensori."), inner, 12.5f * ui,
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, soft);
    } else {
      // Se non ci sta (colonna verticale, barra molto lunga) si rimpicciolisce quanto basta.
      const float fit = std::min({1.0f, (w - 2 * margin) / size.width, (h - 2 * margin) / size.height});
      const float scale = ui * std::max(0.2f, fit);
      if (fit < 1.0f) size = DrawRowsPreview(nullptr, g_dwrite.Get(), rows, p, scale, 0, 0);
      const float m = margin * std::max(0.2f, fit);
      const bool right = p.position == "top-right" || p.position == "bottom-right";
      const bool bottom = p.position == "bottom-left" || p.position == "bottom-right";
      float x = right ? w - size.width - m : m, y = bottom ? h - size.height - m : m;
      if (p.position == "custom") {  // X/Y sono riferiti allo schermo intero: qui la posizione è indicativa
        x = w * float(p.x) / 1920.0f;
        y = h * float(p.y) / float(Profile::kReferenceHeight);
      }
      x = std::clamp(x, 0.0f, std::max(0.0f, w - size.width));
      y = std::clamp(y, 0.0f, std::max(0.0f, h - size.height));
      DrawRowsPreview(rt, g_dwrite.Get(), rows, p, scale, std::floor(x), std::floor(y));

      // Didascalia nell'angolo opposto a quello dell'overlay.
      const std::wstring caption = fit < 0.995f ? TF(L"Ridotta al {}% per starci", int(std::lround(fit * 100)))
                                   : p.autoScale ? std::wstring(T(L"Grandezza reale a 1080p"))
                                                 : std::wstring(T(L"Grandezza reale"));
      const bool capRight = x + size.width / 2 < w / 2, capBottom = y + size.height / 2 < h / 2;
      DrawLabel(rt, caption, inner, 11.5f * ui,
                capRight ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING,
                capBottom ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                D2D1::ColorF(D2D1::ColorF::White, 0.6f));
    }
  }
  if (clipped) rt->PopLayer();
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* pv = reinterpret_cast<Preview*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* fresh = new Preview();
      FillSample(*fresh);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fresh));
      break;
    }
    case WM_NCDESTROY:
      delete pv;
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      bool drawn = false;
      if (pv && rc.right > 0 && rc.bottom > 0 && Factories()) {
        if (!pv->rt) {
          const auto props = D2D1::RenderTargetProperties(
              D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
              96.0f, 96.0f);  // unità = pixel, come nell'overlay
          if (SUCCEEDED(g_d2d->CreateDCRenderTarget(&props, &pv->rt)))
            pv->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        if (pv->rt && SUCCEEDED(pv->rt->BindDC(dc, &rc))) {
          pv->rt->BeginDraw();
          Draw(h, *pv, rc);
          if (FAILED(pv->rt->EndDraw())) pv->rt.Reset();  // dispositivo perso: si ricrea al prossimo disegno
          else drawn = true;
        }
      }
      if (!drawn) {
        HBRUSH br = CreateSolidBrush(col::Panel);
        FillRect(dc, &rc, br);
        DeleteObject(br);
      }
      EndPaint(h, &ps);
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void RegisterOverlayPreview(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = Proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kOverlayPreviewClass;
  RegisterClassExW(&wc);
}

void SetOverlayPreview(HWND preview, const Profile& profile) {
  auto* pv = reinterpret_cast<Preview*>(GetWindowLongPtrW(preview, GWLP_USERDATA));
  if (!pv) return;
  pv->profile = profile;
  pv->system.battery = ReadBattery();  // la batteria compare solo sui portatili, come nell'overlay
  // I sensori scelti mostrano il valore vero se l'overlay è in esecuzione, altrimenti un trattino.
  if (auto feed = ReadSensorFeed())
    pv->system.sensors = std::make_shared<const SensorList>(std::move(feed->sensors));
  else
    pv->system.sensors.reset();
  InvalidateRect(preview, nullptr, FALSE);
}

}  // namespace po::ui
