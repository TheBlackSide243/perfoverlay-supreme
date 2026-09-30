#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "common/config.h"

namespace po {

// Importa un preset dell'OverlayEditor di RivaTuner: .ovl (testo) oppure .ovx
// (contenitore compresso: intestazione "UDC0" + LZW, dentro l'.ovl in chiaro).
struct RtssImportResult {
  bool ok = false;
  std::string error;
  RtssLayout layout;
  std::vector<std::string> unsupported;  // sorgenti senza un dato corrispondente in PerfOverlay
};
RtssImportResult ImportRtssPreset(const std::filesystem::path& file);

// Dati di PerfOverlay utilizzabili dalle sorgenti RTSS (chiave → descrizione).
std::string RtssMetricForSource(const std::string& name, const std::string& id, const std::string& reading);

}  // namespace po
