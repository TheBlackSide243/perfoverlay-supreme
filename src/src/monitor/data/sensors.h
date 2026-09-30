#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "common/sensor_feed.h"
#include "monitor/data/wmi.h"

namespace po {

struct SensorReadings {
  struct Gpu {
    std::string name;
    std::optional<double> tempC;
    std::optional<double> clockMHz;
    std::optional<double> powerW;
  };
  std::optional<double> cpuTempC;
  std::optional<double> cpuPowerW;
  std::vector<Gpu> gpus;
  std::string source;  // "HWiNFO", "LibreHardwareMonitor", "OpenHardwareMonitor" o vuoto
  SensorList all;      // tutti i sensori della sorgente, per la finestra "Sensori di sistema"
};

// Temperature/clock da programmi di monitoraggio esterni, in ordine di preferenza:
// 1) HWiNFO (shared memory, "Shared Memory Support" attivo)
// 2) LibreHardwareMonitor / OpenHardwareMonitor (WMI, programma in esecuzione)
// Se nessuno è disponibile restituisce valori vuoti (l'overlay omette il dato).
// Va usato dallo stesso thread, con COM inizializzato.
class SensorHub {
 public:
  ~SensorHub();
  SensorReadings Read();

 private:
  bool ReadHwinfo(SensorReadings& out);
  bool ReadWmi(SensorReadings& out);
  bool ReadLhmHttp(SensorReadings& out);  // server web di LibreHardwareMonitor (localhost:8085)
  ULONGLONG nextHttpTry_ = 0;

  HANDLE hwiMap_ = nullptr;
  const BYTE* hwiView_ = nullptr;
  SIZE_T hwiSize_ = 0;
  ULONGLONG nextHwiTry_ = 0;

  Wmi wmi_;
  std::string wmiName_;
  ULONGLONG nextWmiTry_ = 0;
};

// Trova la GPU dei sensori che corrisponde al nome dell'adattatore DXGI.
const SensorReadings::Gpu* MatchGpu(const SensorReadings& r, const std::string& adapterName);

}  // namespace po
