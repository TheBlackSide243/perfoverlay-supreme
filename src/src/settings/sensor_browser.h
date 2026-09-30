#pragma once
#include <windows.h>

#include "common/config.h"

namespace po {

// Finestra "Sensori di sistema": tutti i sensori letti da PerfOverlay Supreme (integrati, NVIDIA NVML,
// HWiNFO / LibreHardwareMonitor) aggiornati dal vivo, con una casella per mostrarli nell'overlay e
// l'etichetta da usare. Modale; restituisce true con OK (profile.sensors aggiornato).
bool RunSensorBrowser(HWND owner, HINSTANCE inst, HFONT uiFont, Profile& profile);

}  // namespace po
