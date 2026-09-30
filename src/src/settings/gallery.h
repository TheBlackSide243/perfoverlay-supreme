#pragma once
#include <windows.h>

#include "common/config.h"

namespace po {

// "Galleria overlay": overlay base già pronti (inclusi nell'exe) e overlay da scaricare
// (TroyMetrics Benchmark da GitHub, preset RTSS avanzati). Applica quello scelto al profilo.
// Modale; restituisce true se il profilo è stato modificato.
bool RunGallery(HWND owner, HINSTANCE inst, HFONT uiFont, Profile& profile);

}  // namespace po
