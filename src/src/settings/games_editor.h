#pragma once
#include <windows.h>

#include "common/config.h"

namespace po {

// Finestra "Lista giochi": i programmi su cui compare l'overlay (giochi, appresi in automatico, esclusi).
// Si aggiungono scegliendoli dai programmi aperti, dai giochi installati (Steam, Epic, GOG) o da un .exe.
// Modale; restituisce true con OK (games aggiornata, da salvare).
bool RunGamesEditor(HWND owner, HINSTANCE inst, HFONT uiFont, GameList& games);

}  // namespace po
