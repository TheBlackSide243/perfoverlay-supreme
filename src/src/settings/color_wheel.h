#pragma once
#include <windows.h>

// Ruota colori: anello della tonalità e, al centro, il quadrato di saturazione (orizzontale) e
// luminosità (verticale). Si usa col mouse (trascinando) o con la tastiera: frecce sinistra/destra
// la tonalità, su/giù la luminosità, PagSu/PagGiù la saturazione.
// A ogni cambio dall'utente manda al padre WM_COMMAND con codice kColorWheelChanged.
namespace po::ui {

inline constexpr wchar_t kColorWheelClass[] = L"POColorWheel";
inline constexpr WORD kColorWheelChanged = 1;
inline constexpr UINT kMsgWheelSetColor = WM_USER + 40;  // wParam = COLORREF (non notifica il padre)
inline constexpr UINT kMsgWheelGetColor = WM_USER + 41;  // restituisce il COLORREF attuale

void RegisterColorWheel(HINSTANCE inst);

}  // namespace po::ui
