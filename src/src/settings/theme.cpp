#include "settings/theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
// gdiplus.h usa min/max non qualificati, disattivati da NOMINMAX.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace po::ui {
namespace {

ULONG_PTR g_gdiplus = 0;
constexpr wchar_t kPropBg[] = L"po.bg";
constexpr wchar_t kPropPrimary[] = L"po.primary";
constexpr wchar_t kPropHover[] = L"po.hover";

Gdiplus::Color Gp(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

float Scale(HWND h) { return GetDpiForWindow(h) / 96.0f; }

void RoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float rad) {
  const float d = std::min(rad * 2, std::min(w, h));
  p.AddArc(x, y, d, d, 180, 90);
  p.AddArc(x + w - d, y, d, d, 270, 90);
  p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  p.AddArc(x, y + h - d, d, d, 90, 90);
  p.CloseFigure();
}

void TrackLeave(HWND h) {
  TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
  TrackMouseEvent(&t);
}

// ------------------------------------------------------------ pulsante
LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  switch (msg) {
    case WM_MOUSEMOVE:
      if (!GetPropW(h, kPropHover)) {
        SetPropW(h, kPropHover, reinterpret_cast<HANDLE>(1));
        TrackLeave(h);
        InvalidateRect(h, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      RemovePropW(h, kPropHover);
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_ERASEBKGND:
      return 1;  // disegna tutto WM_DRAWITEM: niente sfarfallio
    case WM_NCDESTROY:
      RemovePropW(h, kPropBg);
      RemovePropW(h, kPropPrimary);
      RemovePropW(h, kPropHover);
      RemoveWindowSubclass(h, ButtonSubclass, 0);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// ------------------------------------------------------------ combobox
void PaintCombo(HWND h, HDC target) {
  RECT rc;
  GetClientRect(h, &rc);
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  HBRUSH bg = CreateSolidBrush(static_cast<COLORREF>(reinterpret_cast<UINT_PTR>(GetPropW(h, kPropBg))));
  FillRect(dc, &rc, bg);
  DeleteObject(bg);

  const float s = Scale(h);
  COMBOBOXINFO ci{sizeof(ci)};
  GetComboBoxInfo(h, &ci);
  const bool editable = ci.hwndItem && ci.hwndItem != h;
  const bool enabled = IsWindowEnabled(h);
  const bool dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
  const HWND focus = GetFocus();
  const bool focused = focus == h || (editable && focus == ci.hwndItem);
  const bool hover = GetPropW(h, kPropHover) != nullptr;
  const COLORREF border = !enabled              ? RGB(48, 48, 56)
                          : dropped || focused  ? col::Accent
                          : hover               ? Blend(col::Line, col::Accent, 0.55f)
                                                : col::Line;
  FillRound(dc, rc, int(6 * s), enabled ? col::Input : RGB(32, 32, 38), border);

  if (!editable) {  // nella combobox modificabile il testo lo disegna il suo campo EDIT
    std::wstring text;
    const int sel = int(SendMessageW(h, CB_GETCURSEL, 0, 0));
    if (sel >= 0) {
      text.resize(size_t(SendMessageW(h, CB_GETLBTEXTLEN, WPARAM(sel), 0)) + 1);
      text.resize(size_t(SendMessageW(h, CB_GETLBTEXT, WPARAM(sel), reinterpret_cast<LPARAM>(text.data()))));
    }
    HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? col::Text : col::Disabled);
    RECT tr{int(9 * s), 0, rc.right - int(26 * s), rc.bottom};
    DrawTextW(dc, text.c_str(), int(text.size()), &tr,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
  }

  // Freccia: verso il basso, verso l'alto con la tendina aperta.
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::Pen pen(Gp(!enabled ? col::Disabled : dropped || hover ? col::Accent : col::Sub), 1.7f * s);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  const float cx = rc.right - 15.0f * s, cy = rc.bottom / 2.0f, dx = 4.0f * s, dy = (dropped ? -2.2f : 2.2f) * s;
  const Gdiplus::PointF pts[] = {{cx - dx, cy - dy}, {cx, cy + dy}, {cx + dx, cy - dy}};
  g.DrawLines(&pen, pts, 3);

  BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

LRESULT CALLBACK ComboSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      PaintCombo(h, dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:
      PaintCombo(h, reinterpret_cast<HDC>(wp));
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE:
      if (!GetPropW(h, kPropHover)) {
        SetPropW(h, kPropHover, reinterpret_cast<HANDLE>(1));
        TrackLeave(h);
        InvalidateRect(h, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      RemovePropW(h, kPropHover);
      InvalidateRect(h, nullptr, FALSE);
      break;
    // Dopo questi messaggi la combobox nativa si ridisegnerebbe da sola: ridisegno con il tema.
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT:
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_CAPTURECHANGED:
    case WM_COMMAND:  // notifiche della tendina (chiusura, selezione)
    case CB_SETCURSEL:
    case CB_SHOWDROPDOWN:
    case CB_RESETCONTENT:
    case CB_SELECTSTRING: {
      const LRESULT r = DefSubclassProc(h, msg, wp, lp);
      InvalidateRect(h, nullptr, FALSE);
      return r;
    }
    case WM_NCDESTROY:
      RemovePropW(h, kPropBg);
      RemovePropW(h, kPropHover);
      RemoveWindowSubclass(h, ComboSubclass, 0);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// ------------------------------------------------------------ checkbox
struct CheckState {
  bool checked = false;
  bool hover = false;
  HFONT font = nullptr;
};

void PaintCheck(HWND h, HDC target) {
  auto* st = reinterpret_cast<CheckState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  RECT rc;
  GetClientRect(h, &rc);
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);

  HBRUSH bg = CreateSolidBrush(col::Panel);
  FillRect(dc, &rc, bg);
  DeleteObject(bg);

  const float s = Scale(h);
  const bool enabled = IsWindowEnabled(h);
  const bool focus = GetFocus() == h;
  const int box = int(16 * s);
  const RECT br{0, (rc.bottom - box) / 2, box, (rc.bottom - box) / 2 + box};
  if (st->checked) {
    FillRound(dc, br, int(4 * s), enabled ? col::Accent : Blend(col::Accent, col::Panel, 0.55f),
              enabled ? col::Accent : Blend(col::Accent, col::Panel, 0.55f));
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gp(col::AccentText), 2.2f * s);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const Gdiplus::PointF pts[] = {{br.left + 4.0f * s, br.top + 8.5f * s},
                                   {br.left + 7.0f * s, br.top + 11.5f * s},
                                   {br.left + 12.0f * s, br.top + 5.0f * s}};
    g.DrawLines(&pen, pts, 3);
  } else {
    FillRound(dc, br, int(4 * s), col::Input, (st->hover || focus) && enabled ? col::Accent : RGB(88, 88, 100));
  }

  wchar_t text[256];
  GetWindowTextW(h, text, 256);
  HGDIOBJ oldFont = SelectObject(dc, st->font ? st->font : GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, enabled ? col::Text : col::Disabled);
  RECT tr{box + int(9 * s), 0, rc.right, rc.bottom};
  DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(dc, oldFont);

  BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

void Toggle(HWND h, CheckState* st) {
  st->checked = !st->checked;
  InvalidateRect(h, nullptr, FALSE);
  SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), reinterpret_cast<LPARAM>(h));
}

LRESULT CALLBACK CheckProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* st = reinterpret_cast<CheckState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new CheckState));
      break;
    case WM_NCDESTROY:
      delete st;
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      PaintCheck(h, dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SETFONT:
      st->font = reinterpret_cast<HFONT>(wp);
      if (lp) InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_GETFONT:
      return reinterpret_cast<LRESULT>(st->font);
    case WM_SETTEXT:
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
      const LRESULT r = DefWindowProcW(h, msg, wp, lp);
      InvalidateRect(h, nullptr, FALSE);
      return r;
    }
    case BM_GETCHECK:
      return st->checked ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
      st->checked = wp == BST_CHECKED;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_MOUSEMOVE:
      if (!st->hover) {
        st->hover = true;
        TrackLeave(h);
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSELEAVE:
      st->hover = false;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
      SetFocus(h);
      SetCapture(h);
      return 0;
    case WM_LBUTTONUP: {
      if (GetCapture() == h) ReleaseCapture();
      RECT rc;
      GetClientRect(h, &rc);
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (PtInRect(&rc, pt)) Toggle(h, st);
      return 0;
    }
    case WM_KEYUP:
      if (wp == VK_SPACE) Toggle(h, st);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_BUTTON;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Init(HINSTANCE inst) {
  Gdiplus::GdiplusStartupInput in;
  Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr);

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = CheckProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
  wc.lpszClassName = kCheckClass;
  RegisterClassExW(&wc);
}

void Shutdown() {
  if (g_gdiplus) Gdiplus::GdiplusShutdown(g_gdiplus);
  g_gdiplus = 0;
}

COLORREF Blend(COLORREF a, COLORREF b, float t) {
  auto mix = [t](int x, int y) { return BYTE(x + (y - x) * t + 0.5f); };
  return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, float borderWidth) {
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::GraphicsPath p;
  const float inset = borderWidth > 0 ? borderWidth / 2 : 0;
  RoundPath(p, r.left + inset, r.top + inset, float(r.right - r.left) - 2 * inset,
            float(r.bottom - r.top) - 2 * inset, float(radius));
  Gdiplus::SolidBrush b(Gp(fill));
  g.FillPath(&b, &p);
  if (borderWidth > 0) {
    Gdiplus::Pen pen(Gp(border), borderWidth);
    g.DrawPath(&pen, &p);
  }
}

void StrokeRound(HDC dc, const RECT& r, int radius, COLORREF color, float width) {
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::GraphicsPath p;
  RoundPath(p, r.left + width / 2, r.top + width / 2, float(r.right - r.left) - width,
            float(r.bottom - r.top) - width, float(radius));
  Gdiplus::Pen pen(Gp(color), width);
  g.DrawPath(&pen, &p);
}

void FillCircle(HDC dc, int cx, int cy, int radius, COLORREF fill, COLORREF border) {
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::SolidBrush b(Gp(fill));
  g.FillEllipse(&b, float(cx - radius), float(cy - radius), float(radius * 2), float(radius * 2));
  Gdiplus::Pen pen(Gp(border), 1.0f);
  g.DrawEllipse(&pen, float(cx - radius), float(cy - radius), float(radius * 2), float(radius * 2));
}

void MakeButton(HWND btn, COLORREF background, bool primary) {
  SetPropW(btn, kPropBg, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(background)));
  if (primary) SetPropW(btn, kPropPrimary, reinterpret_cast<HANDLE>(1));
  SetWindowSubclass(btn, ButtonSubclass, 0, 0);
}

void MakeCombo(HWND combo, COLORREF background) {
  SetPropW(combo, kPropBg, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(background)));
  SetWindowSubclass(combo, ComboSubclass, 0, 0);
  InvalidateRect(combo, nullptr, FALSE);
}

void DrawButton(const DRAWITEMSTRUCT* dis, const COLORREF* swatch) {
  HWND h = dis->hwndItem;
  const RECT& rc = dis->rcItem;
  const int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
  const float s = Scale(h);

  // Doppio buffer: il pulsante viene ridisegnato a ogni hover.
  HDC dc = CreateCompatibleDC(dis->hDC);
  HBITMAP bmp = CreateCompatibleBitmap(dis->hDC, w, hgt);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  const RECT r{0, 0, w, hgt};
  HBRUSH bgBrush = CreateSolidBrush(static_cast<COLORREF>(reinterpret_cast<UINT_PTR>(GetPropW(h, kPropBg))));
  FillRect(dc, &r, bgBrush);
  DeleteObject(bgBrush);

  const bool primary = GetPropW(h, kPropPrimary) != nullptr;
  const bool hover = GetPropW(h, kPropHover) != nullptr;
  const bool disabled = dis->itemState & ODS_DISABLED;
  const bool pressed = dis->itemState & ODS_SELECTED;
  const bool focus = (dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT);
  COLORREF fill, border, text;
  if (disabled) {
    fill = RGB(32, 32, 38);
    border = RGB(48, 48, 56);
    text = RGB(96, 96, 106);
  } else if (primary) {
    fill = pressed ? col::Accent : (hover ? col::AccentHover : col::Accent);
    border = fill;
    text = col::AccentText;
  } else {
    fill = pressed ? RGB(36, 36, 42) : (hover ? Blend(col::BtnBg, col::Accent, 0.12f) : col::BtnBg);
    border = hover || focus ? col::Accent : col::BtnBorder;
    text = col::Text;
  }
  FillRound(dc, r, int(6 * s), fill, border);

  wchar_t label[128];
  GetWindowTextW(h, label, 128);
  HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, text);
  RECT tr = r;
  UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
  if (swatch) {  // pulsante colore: pallino a sinistra + nome
    const int rad = int(7 * s);
    FillCircle(dc, int(14 * s) + rad, hgt / 2, rad, *swatch, Blend(*swatch, RGB(255, 255, 255), 0.35f));
    tr.left = int(14 * s) + rad * 2 + int(10 * s);
    fmt |= DT_LEFT;
  } else {
    fmt |= DT_CENTER;
  }
  DrawTextW(dc, label, -1, &tr, fmt);
  SelectObject(dc, oldFont);

  BitBlt(dis->hDC, rc.left, rc.top, w, hgt, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

}  // namespace po::ui
