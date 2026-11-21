#pragma once
#include <windows.h>

#include "common/config.h"

// Anteprima dell'overlay nelle impostazioni: una finta scena di gioco con sopra la barra (o la colonna)
// disegnata dallo stesso codice dell'overlay vero, con valori d'esempio. Si aggiorna mentre si cambiano
// font, dimensione, opacità, colori, elementi e posizione. I layout "libero" e "preset RTSS" occupano
// tutto lo schermo e non stanno in una striscia: per quelli rimanda all'Editor.
namespace po::ui {

inline constexpr wchar_t kOverlayPreviewClass[] = L"POOverlayPreview";

void RegisterOverlayPreview(HINSTANCE inst);
void SetOverlayPreview(HWND preview, const Profile& profile);  // ridisegna con questo profilo

}  // namespace po::ui
