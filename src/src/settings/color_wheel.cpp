#include "settings/color_wheel.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "settings/theme.h"

namespace po::ui {
namespace {

constexpr float kPi = 3.14159265f;

struct Wheel {
  float h = 210, s = 0.77f, v = 1;  // tonalità in gradi, saturazione e luminosità 0..1
  int drag = 0;                     // 0 nessuno, 1 anello, 2 quadrato
  HBITMAP cache = nullptr;          // anello + quadrato già disegnati
  int cacheSize = 0;
  float cacheHue = -1;
  COLORREF cacheBg = 0;
};

COLORREF Hsv(float h, float s, float v) {
  h = std::fmod(h < 0 ? h + 360 : h, 360.0f) / 60.0f;
  const int i = int(h);
  const float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r, g, b;
  switch (i) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
  }
  return RGB(BYTE(r * 255 + 0.5f), BYTE(g * 255 + 0.5f), BYTE(b * 255 + 0.5f));
}

void ToHsv(COLORREF c, Wheel& w) {
  const float r = GetRValue(c) / 255.0f, g = GetGValue(c) / 255.0f, b = GetBValue(c) / 255.0f;
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  w.v = mx;
  w.s = mx > 0 ? d / mx : 0;
  if (d > 0) {  // con un grigio la tonalità resta quella di prima
    float h = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
    h *= 60;
    w.h = h < 0 ? h + 360 : h;
  }
}

float ScaleOf(HWND h) {
  if (const auto p = reinterpret_cast<uintptr_t>(GetPropW(GetAncestor(h, GA_ROOT), kScaleProp))) return p / 1000.0f;
  return GetDpiForWindow(h) / 96.0f;
}

// Geometria: anello esterno, foro interno e semi-lato del quadrato, in pixel della finestra.
struct Geo {
  float cx, cy, rOut, rIn, half;
};
Geo GeoOf(int size) {
  Geo g;
  g.cx = g.cy = size / 2.0f;
  g.rOut = size / 2.0f - 6;  // margine per l'anello di focus
  g.rIn = g.rOut - std::max(10.0f, size * 0.12f);
  g.half = g.rIn * 0.62f;
  return g;
}

void Render(Wheel& w, int size, COLORREF bg) {
  if (w.cache && w.cacheSize == size && w.cacheHue == w.h && w.cacheBg == bg) return;
  if (w.cache) DeleteObject(w.cache);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
  void* bits = nullptr;
  w.cache = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  w.cacheSize = size;
  w.cacheHue = w.h;
  w.cacheBg = bg;
  if (!w.cache) return;
  auto* px = static_cast<uint32_t*>(bits);
  const Geo g = GeoOf(size);
  const float br = GetRValue(bg), bgg = GetGValue(bg), bb = GetBValue(bg);
  auto put = [&](int x, int y, COLORREF c, float a) {
    const float r = br + (GetRValue(c) - br) * a, gg = bgg + (GetGValue(c) - bgg) * a, b = bb + (GetBValue(c) - bb) * a;
    px[y * size + x] = (uint32_t(r + 0.5f) << 16) | (uint32_t(gg + 0.5f) << 8) | uint32_t(b + 0.5f);
  };
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const float dx = x + 0.5f - g.cx, dy = y + 0.5f - g.cy, d = std::sqrt(dx * dx + dy * dy);
      // Anello: bordi sfumati di mezzo pixel per non vedere scalini.
      const float a = std::clamp(d - g.rIn + 0.5f, 0.0f, 1.0f) * std::clamp(g.rOut - d + 0.5f, 0.0f, 1.0f);
      if (a > 0) {
        const float hue = std::atan2(-dy, dx) * 180 / kPi;
        put(x, y, Hsv(hue, 1, 1), a);
      } else if (std::fabs(dx) <= g.half && std::fabs(dy) <= g.half) {
        const float s = (dx + g.half) / (2 * g.half), v = 1 - (dy + g.half) / (2 * g.half);
        put(x, y, Hsv(w.h, s, v), 1);
      } else {
        put(x, y, bg, 1);
      }
    }
}

void Notify(HWND h) {
  SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), kColorWheelChanged), reinterpret_cast<LPARAM>(h));
}

void Track(HWND h, Wheel& w, int x, int y) {
  RECT rc;
  GetClientRect(h, &rc);
  const Geo g = GeoOf(std::min(rc.right, rc.bottom));
  const float dx = x - g.cx, dy = y - g.cy;
  if (w.drag == 1) {
    w.h = std::atan2(-dy, dx) * 180 / kPi;
    if (w.h < 0) w.h += 360;
  } else if (w.drag == 2) {
    w.s = std::clamp((dx + g.half) / (2 * g.half), 0.0f, 1.0f);
    w.v = std::clamp(1 - (dy + g.half) / (2 * g.half), 0.0f, 1.0f);
  }
  InvalidateRect(h, nullptr, FALSE);
  Notify(h);
}

void Paint(HWND h, Wheel& w) {
  PAINTSTRUCT ps;
  HDC target = BeginPaint(h, &ps);
  RECT rc;
  GetClientRect(h, &rc);
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  HBRUSH bg = CreateSolidBrush(col::Panel);
  FillRect(dc, &rc, bg);
  DeleteObject(bg);

  const int size = std::min(rc.right, rc.bottom);
  Render(w, size, col::Panel);
  if (w.cache) {
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ o = SelectObject(src, w.cache);
    BitBlt(dc, 0, 0, size, size, src, 0, 0, SRCCOPY);
    SelectObject(src, o);
    DeleteDC(src);
  }
  const Geo g = GeoOf(size);
  const float sc = ScaleOf(h);
  // Cursore sull'anello (colore della tonalità) e nel quadrato (colore scelto), con bordo bianco e nero.
  const float mid = (g.rIn + g.rOut) / 2, ang = w.h * kPi / 180;
  const int hx = int(g.cx + mid * std::cos(ang)), hy = int(g.cy - mid * std::sin(ang));
  const int hr = int((g.rOut - g.rIn) / 2 + 2 * sc);
  FillCircle(dc, hx, hy, hr + 1, RGB(20, 20, 24), RGB(20, 20, 24));
  FillCircle(dc, hx, hy, hr, RGB(255, 255, 255), RGB(255, 255, 255));
  FillCircle(dc, hx, hy, hr - int(3 * sc), Hsv(w.h, 1, 1), Hsv(w.h, 1, 1));
  const int sx = int(g.cx - g.half + w.s * 2 * g.half), sy = int(g.cy - g.half + (1 - w.v) * 2 * g.half);
  const int sr = int(8 * sc);
  FillCircle(dc, sx, sy, sr + 1, RGB(20, 20, 24), RGB(20, 20, 24));
  FillCircle(dc, sx, sy, sr, RGB(255, 255, 255), RGB(255, 255, 255));
  FillCircle(dc, sx, sy, sr - int(3 * sc), Hsv(w.h, w.s, w.v), Hsv(w.h, w.s, w.v));
  // Focus da tastiera: anello d'accento attorno alla ruota.
  if (GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
    const RECT fr{0, 0, size, size};
    StrokeRound(dc, fr, size / 2, col::Accent, 2.0f * sc);
  }

  BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
  EndPaint(h, &ps);
}

LRESULT CALLBACK WheelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* w = reinterpret_cast<Wheel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new Wheel));
      break;
    case WM_NCDESTROY:
      if (w) {
        if (w->cache) DeleteObject(w->cache);
        delete w;
      }
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint(h, *w);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_LBUTTONDOWN: {
      SetFocus(h);
      RECT rc;
      GetClientRect(h, &rc);
      const Geo g = GeoOf(std::min(rc.right, rc.bottom));
      const float dx = GET_X_LPARAM(lp) - g.cx, dy = GET_Y_LPARAM(lp) - g.cy;
      const float d = std::sqrt(dx * dx + dy * dy);
      w->drag = d >= g.rIn - 4 && d <= g.rOut + 4                       ? 1
                : std::fabs(dx) <= g.half + 4 && std::fabs(dy) <= g.half + 4 ? 2
                                                                               : 0;
      if (w->drag) {
        SetCapture(h);
        Track(h, *w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (w->drag && GetCapture() == h) Track(h, *w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      w->drag = 0;
      if (msg == WM_LBUTTONUP && GetCapture() == h) ReleaseCapture();
      return 0;
    case WM_KEYDOWN: {
      bool changed = true;
      switch (wp) {
        case VK_LEFT: w->h = std::fmod(w->h + 357, 360.0f); break;
        case VK_RIGHT: w->h = std::fmod(w->h + 3, 360.0f); break;
        case VK_UP: w->v = std::min(1.0f, w->v + 0.03f); break;
        case VK_DOWN: w->v = std::max(0.0f, w->v - 0.03f); break;
        case VK_PRIOR: w->s = std::min(1.0f, w->s + 0.05f); break;
        case VK_NEXT: w->s = std::max(0.0f, w->s - 0.05f); break;
        default: changed = false;
      }
      if (changed) {
        InvalidateRect(h, nullptr, FALSE);
        Notify(h);
        return 0;
      }
      break;
    }
    case kMsgWheelSetColor:
      ToHsv(COLORREF(wp), *w);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case kMsgWheelGetColor:
      return LRESULT(Hsv(w->h, w->s, w->v));
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void RegisterColorWheel(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WheelProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
  wc.lpszClassName = kColorWheelClass;
  RegisterClassExW(&wc);
}

}  // namespace po::ui
