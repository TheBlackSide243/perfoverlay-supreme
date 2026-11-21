#pragma once
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/sensor_feed.h"

// Sensori della scheda video per le schede non NVIDIA (quelle NVIDIA usano NVML, vedi nvml.h):
// - Windows (WDDM 2.4 o successivo): temperatura e ventola di qualsiasi scheda, la stessa lettura di Gestione attività;
// - AMD (ADL, atiadlxx.dll installata dai driver): temperatura, consumo in watt, frequenza, ventola e altri sensori.
namespace po {

struct GpuExtra {
  std::optional<double> tempC, hotspotC, clockMHz, powerW, fanRpm, fanPct;
  std::optional<double> powerPct;  // consumo in % del massimo (Windows non dà i watt)
};

// Lettura di Windows per l'adattatore con quel LUID. I campi che il driver non fornisce restano vuoti.
GpuExtra ReadWddmGpu(uint32_t luidHi, uint32_t luidLo);

class AdlReader {
 public:
  ~AdlReader();
  bool Init();  // false se non ci sono schede AMD o la libreria manca
  // Scheda AMD con quel nome (quello di DXGI); con una sola scheda AMD vale comunque quella.
  bool Read(const std::string& adapterName, GpuExtra& out);
  void ReadAll(SensorList& out);  // tutti i sensori di tutte le schede AMD, per la finestra dei sensori

 private:
  struct Dev {
    int index = 0;  // indice ADL
    int bus = 0;
    std::string name;
  };
  bool Query(const Dev& d, std::vector<std::pair<int, int>>& values);  // (tipo di sensore, valore) dei soli supportati

  HMODULE mod_ = nullptr;
  void* ctx_ = nullptr;
  int (*destroy_)(void*) = nullptr;
  int (*pmlog_)(void*, int, void*) = nullptr;
  std::vector<Dev> devs_;
};

}  // namespace po
