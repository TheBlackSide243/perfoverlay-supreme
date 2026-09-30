#pragma once
#include <windows.h>

#include "common/config.h"

namespace po {

// Editor del layout libero in stile OverlayEditor di RTSS: tela 1920x1080 con griglia in pixel,
// blocchi trascinabili uno per uno, frecce per spostamenti di 1 px, snap alla griglia.
// Finestra modale; restituisce true se l'utente conferma (profile viene aggiornato).
bool RunLayoutEditor(HWND owner, HINSTANCE inst, HFONT uiFont, Profile& profile);

}  // namespace po
