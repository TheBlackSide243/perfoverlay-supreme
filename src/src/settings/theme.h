#pragma once
#include <windows.h>

#include <string>

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
// Colore d'accento scelto dall'utente (Impostazioni › Colore interfaccia): si cambia con SetAccent,
// che ricava anche hover e testo sopra l'accento. Predefinito: blu (Decky arancione, SteamImporter viola).
constexpr COLORREF DefaultAccent = RGB(59, 158, 255);
inline COLORREF Accent = DefaultAccent;
inline COLORREF AccentHover = RGB(107, 182, 255);
inline COLORREF AccentText = RGB(8, 18, 32);
constexpr COLORREF BtnBg = RGB(44, 44, 52);
// Bordi dei controlli (campi, pulsanti, caselle): almeno 3:1 su Panel e Bg, come chiede WCAG 1.4.11.
// Line resta per pannelli e separatori, che sono solo decorativi.
constexpr COLORREF Field = RGB(106, 106, 120);
constexpr COLORREF FieldHover = RGB(150, 150, 166);  // mouse sopra; il focus invece usa Accent e bordo doppio
constexpr COLORREF BtnBorder = Field;
constexpr COLORREF Good = RGB(74, 214, 124);
constexpr COLORREF Bad = RGB(240, 96, 80);
}  // namespace col

inline constexpr wchar_t kCheckClass[] = L"POCheck";
// Proprietà della finestra principale: scala dei controlli × 1000 (1000 = 96 DPI). Se manca si usano i DPI.
inline constexpr wchar_t kScaleProp[] = L"po.scale";

void Init(HINSTANCE inst);  // GDI+ e classi finestra
void Shutdown();

COLORREF Blend(COLORREF a, COLORREF b, float t);  // t=0 → a, t=1 → b

// Imposta il colore d'accento. Un colore troppo scuro viene schiarito quanto basta per restare leggibile
// sullo sfondo (contrasto almeno 4.5:1); il testo sopra l'accento diventa scuro o chiaro a seconda del colore.
void SetAccent(COLORREF accent);
float Contrast(COLORREF a, COLORREF b);  // rapporto di contrasto WCAG (1..21)
// "#RRGGBB" ↔ COLORREF. ParseHexColor restituisce false se la stringa non è valida.
bool ParseHexColor(const std::string& s, COLORREF& out);
std::string HexColor(COLORREF c);
void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, float borderWidth = 1.0f);
void StrokeRound(HDC dc, const RECT& r, int radius, COLORREF color, float width);
void FillCircle(HDC dc, int cx, int cy, int radius, COLORREF fill, COLORREF border);

// Pulsante owner-draw (BS_OWNERDRAW): stato hover gestito qui, disegno con DrawButton da WM_DRAWITEM.
void MakeButton(HWND btn, COLORREF background, bool primary);
bool IsHovered(HWND btn);  // il mouse è sopra un pulsante creato con MakeButton
void DrawButton(const DRAWITEMSTRUCT* dis, const COLORREF* swatch = nullptr);

// Combobox con lo stesso aspetto dei campi di testo: cornice arrotondata, bordo d'accento con hover,
// focus o tendina aperta, freccia disegnata. background = colore dietro il controllo.
void MakeCombo(HWND combo, COLORREF background);

}  // namespace po::ui
