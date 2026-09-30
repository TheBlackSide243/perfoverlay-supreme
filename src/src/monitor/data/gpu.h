#pragma once
#include <windows.h>
#include <pdh.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace po {

struct GpuStats {
  std::string name;
  uint32_t vendorId = 0;
  std::optional<double> usage;        // % (engine più carico dell'adattatore)
  std::optional<double> tempC;
  std::optional<double> clockMHz;
  std::optional<double> powerW;
  std::optional<double> vramUsedGB;
  std::optional<double> vramTotalGB;
};

// Carico GPU di un singolo processo (per riconoscere i giochi senza bisogno degli FPS).
struct ProcessGpu {
  DWORD pid = 0;
  bool valid = false;  // contatori disponibili per quel processo
  double usage3D = 0;  // % del motore 3D più usato dal processo
  double vramMB = 0;   // VRAM dedicata occupata dal processo
};

// DXGI per elenco adattatori e VRAM totale; contatori "GPU Engine" / "GPU Adapter Memory"
// per utilizzo e VRAM usata. Con più GPU sceglie quella su cui gira il processo del gioco.
class GpuMonitor {
 public:
  ~GpuMonitor();
  bool Init();
  GpuStats Sample(DWORD gamePid, DWORD watchPid = 0, ProcessGpu* watch = nullptr);

 private:
  struct Adapter {
    uint32_t luidHi = 0, luidLo = 0;
    std::string name;
    uint32_t vendorId = 0, deviceId = 0, subSysId = 0, revision = 0;
    uint64_t dedicatedBytes = 0;
  };
  void RemoveVirtualAdapters();
  const Adapter* FindAdapter(uint32_t hi, uint32_t lo) const;

  std::vector<Adapter> adapters_;
  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER engineUtil_ = nullptr, dedicatedUsage_ = nullptr, processDedicated_ = nullptr;
  int selected_ = -1;
};

}  // namespace po
