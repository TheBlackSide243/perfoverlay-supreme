#pragma once
#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace po {

// Un sensore del sistema (temperatura, clock, ventola, consumo, ...). L'id è stabile tra un avvio
// e l'altro: è quello che i profili salvano per mostrare il sensore nell'overlay.
struct SensorEntry {
  std::string id;     // es. "hwi:4026532096.0:16777216", "lhm:/amdcpu/0/temperature/2", "nv:0:temp"
  std::string group;  // hardware: "NVIDIA GeForce RTX 3080", "Disco Samsung SSD 980", ...
  std::string name;   // "GPU Temperature", "Core 3 frequenza", ...
  std::string unit;   // "°C", "MHz", "W", "%", "RPM", "V", "GB", "MB/s", ...
  double value = 0, min = 0, max = 0;
};
using SensorList = std::vector<SensorEntry>;

// "65 °C", "1.25 V", "2610 MHz": cifre decimali scelte in base all'unità e al valore.
std::wstring FormatSensorValue(double v, std::string_view unit, bool withUnit = true);
std::string SensorCategory(const SensorEntry& e);  // "cpu" | "gpu" | "ram" | "battery" | "" (per il colore)

// Il monitor (amministratore) pubblica l'elenco completo dei sensori in una memoria condivisa leggibile
// dalle impostazioni (utente normale), che lo mostrano nella finestra "Sensori di sistema".
class SensorPublisher {
 public:
  ~SensorPublisher();
  void Publish(const SensorList& sensors, const std::string& source);

 private:
  HANDLE map_ = nullptr;
  BYTE* view_ = nullptr;
  bool failed_ = false;
};

struct SensorFeed {
  SensorList sensors;
  std::string source;      // "HWiNFO", "LibreHardwareMonitor", ... o vuoto (solo sensori integrati)
  ULONGLONG ageMs = 0;     // da quanto è stato scritto
};
// nullopt se il monitor non è in esecuzione.
std::optional<SensorFeed> ReadSensorFeed();

}  // namespace po
