#pragma once
#include <string>
#include <vector>

#include "common/config.h"
#include "monitor/frame_source.h"
#include "monitor/overlay_window.h"
#include "monitor/stats.h"

namespace po {

struct ContentInput {
  const Profile* profile = nullptr;
  const FrameStats* frames = nullptr;     // nullptr in modalità compatta desktop
  const SystemSnapshot* system = nullptr;
  bool fpsOnly = false;
  bool frameSourceAvailable = true;
  std::wstring toast;                     // messaggio temporaneo (es. cambio profilo)
};

// Traduce le metriche in righe di segmenti. Ordine: FPS, frame time, CPU, GPU, RAM, batteria.
std::vector<Row> BuildRows(const ContentInput& in);

// Layout libero: il contenuto di un singolo blocco ("fps", "graph", "cpu", "sensor:<id>", ...). Vuoto se nascosto.
Row BuildElementRow(const std::string& element, const ContentInput& in);

// Una riga per ogni sensore scelto nel profilo (per il layout "Preset RTSS").
std::vector<Row> BuildSensorRows(const ContentInput& in);

}  // namespace po
