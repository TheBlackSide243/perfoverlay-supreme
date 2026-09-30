#pragma once
#include <windows.h>

// Tema grafico condiviso con le altre app dell'autore (Decky Manager, SteamImporter):
// sfondo quasi nero, pannelli arrotondati, testo chiaro, un colore d'accento per app.
namespace po::ui {

namespace col {
constexpr COLORREF Bg = RGB(15, 15, 17);
constexpr COLORREF Panel = RGB(28, 28, 32);
constexpr COLORREF Line = RGB(52, 52, 60);
constexpr COLORREF Input = RGB(38, 38, 44);
constexpr COLORREF Text = RGB(242, 242, 246);
constexpr COLORREF Sub = RGB(150, 150, 162);
constexpr COLORREF Disabled = RGB(104, 104, 114);
constexpr COLORREF Accent = RGB(59, 158, 255);       // PerfOverlay: blu (Decky arancione, SteamImporter viola)
constexpr COLORREF AccentHover = RGB(107, 182, 255);
constexpr COLORREF AccentText = RGB(8, 18, 32);
constexpr COLORREF BtnBg = RGB(44, 44, 52);
constexpr COLORREF BtnBorder = RGB(78, 78, 92);
constexpr COLORREF Good = RGB(74, 214, 124);
constexpr COLORREF Bad = RGB(240, 96, 80);
}  // namespace col

inline constexpr wchar_t kCheckClass[] = L"POCheck";

void Init(HINSTANCE inst);  // GDI+ e classi finestra
void Shutdown();

COLORREF Blend(COLORREF a, COLORREF b, float t);  // t=0 → a, t=1 → b
void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, float borderWidth = 1.0f);
void StrokeRound(HDC dc, const RECT& r, int radius, COLORREF color, float width);
void FillCircle(HDC dc, int cx, int cy, int radius, COLORREF fill, COLORREF border);

// Pulsante owner-draw (BS_OWNERDRAW): stato hover gestito qui, disegno con DrawButton da WM_DRAWITEM.
void MakeButton(HWND btn, COLORREF background, bool primary);
void DrawButton(const DRAWITEMSTRUCT* dis, const COLORREF* swatch = nullptr);

}  // namespace po::ui
