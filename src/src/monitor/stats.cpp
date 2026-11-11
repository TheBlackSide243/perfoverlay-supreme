#include "monitor/stats.h"

#include <objbase.h>

#include <unordered_map>

#include "common/log.h"
#include "monitor/data/nvml.h"
#include "monitor/data/sensors.h"
#include "monitor/data/system_sensors.h"
#include "monitor/data/wmi.h"

namespace po {

void StatsCollector::Start(int intervalMs) {
  interval_ = intervalMs;
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void StatsCollector::Stop() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

const SensorEntry* FindSensor(const SystemSnapshot& s, const std::string& id) {
  if (!s.sensors) return nullptr;
  for (const auto& e : *s.sensors)
    if (e.id == id) return &e;
  return nullptr;
}

SystemSnapshot StatsCollector::Get() const {
  std::lock_guard lock(mu_);
  return snap_;
}

void StatsCollector::Loop() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                       nullptr, EOAC_NONE, nullptr);
  {
    CpuMonitor cpu;
    cpu.Init();
    GpuMonitor gpu;
    gpu.Init();
    SensorHub sensors;
    NvmlReader nvml;
    const bool hasNvml = nvml.Init();

    int ramSpeed = 0;
    {
      Wmi cimv2;
      if (cimv2.Connect(L"root\\cimv2")) ramSpeed = ReadRamSpeed(cimv2);
      LogInfo("RAM: {} MT/s", ramSpeed);
    }

    SystemSensors system;
    system.Init();
    SensorPublisher publisher;
    std::shared_ptr<const SensorList> allSensors;
    std::unordered_map<std::string, std::pair<double, double>> minMax;  // min/max dall'avvio (sensori integrati)

    SensorReadings readings;
    ULONGLONG nextSensorRead = 0;
    std::string lastSource = "?";

    while (true) {
      SystemSnapshot s;
      s.cpu = cpu.Sample();

      const ULONGLONG now = GetTickCount64();
      const bool refreshSensors = now >= nextSensorRead;
      if (refreshSensors) {  // sensori esterni al massimo 1 volta al secondo
        readings = sensors.Read();
        nextSensorRead = now + 1000;
        if (readings.source != lastSource) {
          LogInfo("Sorgente temperature: {}", readings.source.empty() ? "nessuna (dato omesso)" : readings.source);
          lastSource = readings.source;
        }
      }
      s.cpuTempC = readings.cpuTempC;
      s.cpuPowerW = readings.cpuPowerW;
      s.sensorSource = readings.source;

      s.gpu = gpu.Sample(gamePid_, watchPid_, &s.foreground);
      if (const auto* g = MatchGpu(readings, s.gpu.name)) {
        s.gpu.tempC = g->tempC;
        s.gpu.clockMHz = g->clockMHz;
        s.gpu.powerW = g->powerW;
      }
      if (hasNvml && s.gpu.vendorId == 0x10DE && (!s.gpu.tempC || !s.gpu.clockMHz || !s.gpu.powerW))
        nvml.Read(s.gpu.name, s.gpu.tempC, s.gpu.clockMHz, s.gpu.powerW);

      s.ram = ReadRam();
      s.ram.speedMTs = ramSpeed;
      s.battery = ReadBattery();

      if (refreshSensors) {
        // Elenco completo: integrati (CPU, GPU, memoria, dischi, rete...), NVML, poi HWiNFO / LibreHardwareMonitor.
        auto list = std::make_shared<SensorList>();
        system.Collect(s.cpu, s.gpu, hasNvml && s.gpu.vendorId == 0x10DE, s.ram, s.battery, *list);
        if (hasNvml) nvml.ReadAll(*list);
        list->insert(list->end(), readings.all.begin(), readings.all.end());
        for (auto& e : *list) {
          if (e.id.starts_with("hwi:") || e.id.starts_with("lhm:")) continue;  // min/max già forniti
          auto [it, fresh] = minMax.try_emplace(e.id, e.value, e.value);
          if (!fresh) {
            it->second.first = std::min(it->second.first, e.value);
            it->second.second = std::max(it->second.second, e.value);
          }
          e.min = it->second.first;
          e.max = it->second.second;
        }
        publisher.Publish(*list, readings.source);
        allSensors = std::move(list);
      }
      s.sensors = allSensors;
      s.ready = true;

      std::unique_lock lock(mu_);
      snap_ = std::move(s);
      if (cv_.wait_for(lock, std::chrono::milliseconds(interval_.load()), [this] { return stop_; })) break;
    }
  }
  if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace po
