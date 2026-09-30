#pragma once
#include <windows.h>

#include <atomic>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/sensor_feed.h"
#include "monitor/data/battery.h"
#include "monitor/data/cpu.h"
#include "monitor/data/gpu.h"
#include "monitor/data/memory.h"

namespace po {

struct SystemSnapshot {
  bool ready = false;
  CpuStats cpu;
  std::optional<double> cpuTempC;
  std::optional<double> cpuPowerW;
  GpuStats gpu;
  RamStats ram;
  BatteryStats battery;
  std::string sensorSource;
  ProcessGpu foreground;  // carico GPU della finestra in primo piano
  std::shared_ptr<const SensorList> sensors;  // tutti i sensori (aggiornati ogni secondo)
};

// Sensore per id (quelli scelti nella finestra "Sensori di sistema"); nullptr se non c'è.
const SensorEntry* FindSensor(const SystemSnapshot& s, const std::string& id);

// Raccoglie le metriche di sistema su un thread dedicato (PDH e WMI possono impiegare decine di ms).
class StatsCollector {
 public:
  ~StatsCollector() { Stop(); }
  void Start(int intervalMs);
  void Stop();
  void SetInterval(int ms) { interval_ = ms; }
  void SetGamePid(DWORD pid) { gamePid_ = pid; }
  void SetWatchPid(DWORD pid) { watchPid_ = pid; }
  SystemSnapshot Get() const;

 private:
  void Loop();

  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<int> interval_{500};
  std::atomic<DWORD> gamePid_{0};
  std::atomic<DWORD> watchPid_{0};
  SystemSnapshot snap_;
};

}  // namespace po
