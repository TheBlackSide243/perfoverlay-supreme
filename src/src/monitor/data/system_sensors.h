#pragma once
#include <windows.h>
#include <pdh.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/sensor_feed.h"
#include "monitor/data/battery.h"
#include "monitor/data/cpu.h"
#include "monitor/data/gpu.h"
#include "monitor/data/memory.h"
#include "monitor/data/wmi.h"

namespace po {

// Sensori disponibili senza programmi esterni: CPU (totale e per core), GPU (da Windows), memoria,
// dischi (attività, velocità e temperatura SMART), rete, zone termiche ACPI, batteria, sistema.
// Temperature CPU/scheda madre e ventole richiedono HWiNFO o LibreHardwareMonitor.
// Va usato dal thread delle statistiche (COM inizializzato).
class SystemSensors {
 public:
  ~SystemSensors();
  void Init();
  void Collect(const CpuStats& cpu, const GpuStats& gpu, bool gpuFromNvml, const RamStats& ram,
               const BatteryStats& bat, SensorList& out);

 private:
  struct DiskInfo {
    std::string name;
    std::vector<double> tempsC;  // [0] = temperatura principale (composita), poi i singoli sensori
    ULONGLONG nextTemp = 0;
    bool noTemp = false;         // il disco non la fornisce (es. chiavette USB)
  };
  void ReadDiskInfo();
  void ReadDiskTemps(int disk, DiskInfo& d);

  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER diskIdle_ = nullptr, diskRead_ = nullptr, diskWrite_ = nullptr;
  PDH_HCOUNTER netRx_ = nullptr, netTx_ = nullptr, thermal_ = nullptr;
  bool thermalTenths_ = true;  // "High Precision Temperature" è in decimi di kelvin

  Wmi storage_;
  bool storageTried_ = false;
  ULONGLONG nextDiskInfo_ = 0;
  std::map<int, DiskInfo> disks_;  // numero disco fisico → info
  std::set<std::wstring> activeNics_;
};

}  // namespace po
