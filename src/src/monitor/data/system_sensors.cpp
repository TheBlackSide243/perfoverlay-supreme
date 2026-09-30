#include "monitor/data/system_sensors.h"

#include <powerbase.h>
#include <winioctl.h>
#include <psapi.h>

#include <algorithm>
#include <cstddef>
#include <format>

#include "common/log.h"
#include "common/util.h"
#include "monitor/data/pdh_util.h"

namespace po {
namespace {

constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

// Nomi delle istanze di rete senza traffico utile (adattatori virtuali di Windows).
bool IgnoredNic(const std::wstring& n) {
  for (const wchar_t* k : {L"isatap", L"Teredo", L"Loopback", L"6to4", L"Pseudo"})
    if (n.find(k) != std::wstring::npos) return true;
  return false;
}

}  // namespace

SystemSensors::~SystemSensors() {
  if (query_) PdhCloseQuery(query_);
}

void SystemSensors::Init() {
  if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
  PdhAddEnglishCounterW(query_, L"\\PhysicalDisk(*)\\% Idle Time", 0, &diskIdle_);
  PdhAddEnglishCounterW(query_, L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", 0, &diskRead_);
  PdhAddEnglishCounterW(query_, L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", 0, &diskWrite_);
  PdhAddEnglishCounterW(query_, L"\\Network Interface(*)\\Bytes Received/sec", 0, &netRx_);
  PdhAddEnglishCounterW(query_, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &netTx_);
  if (PdhAddEnglishCounterW(query_, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &thermal_) !=
      ERROR_SUCCESS) {
    thermalTenths_ = false;
    PdhAddEnglishCounterW(query_, L"\\Thermal Zone Information(*)\\Temperature", 0, &thermal_);
  }
  PdhCollectQueryData(query_);
}

// Nome dei dischi fisici (namespace Storage di WMI): una volta al minuto.
void SystemSensors::ReadDiskInfo() {
  const ULONGLONG now = GetTickCount64();
  if (now < nextDiskInfo_) return;
  nextDiskInfo_ = now + 60000;
  if (!storage_.Connected()) {
    if (storageTried_) return;  // non disponibile: restano "Disco N"
    storageTried_ = true;
    if (!storage_.Connect(L"root\\Microsoft\\Windows\\Storage")) return;
  }
  for (auto& row : storage_.Query(L"SELECT DeviceId, FriendlyName FROM MSFT_PhysicalDisk",
                                  {L"DeviceId", L"FriendlyName"}))
    disks_[_wtoi(row[L"DeviceId"].str.c_str())].name = ToUtf8(row[L"FriendlyName"].str);
}

// Temperature del disco dal driver di archiviazione (NVMe/SATA, Windows 10+), ogni 5 s.
// Basta un handle senza diritti di lettura: non serve accedere ai dati del disco.
void SystemSensors::ReadDiskTemps(int disk, DiskInfo& d) {
  const ULONGLONG now = GetTickCount64();
  if (d.noTemp || now < d.nextTemp) return;
  d.nextTemp = now + 5000;
  HANDLE h = CreateFileW(std::format(L"\\\\.\\PhysicalDrive{}", disk).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    d.noTemp = true;
    return;
  }
  STORAGE_PROPERTY_QUERY q{};
  q.PropertyId = StorageDeviceTemperatureProperty;
  q.QueryType = PropertyStandardQuery;
  alignas(8) BYTE buf[1024] = {};
  DWORD bytes = 0;
  if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &bytes, nullptr) &&
      bytes >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)) {
    const auto* td = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
    d.tempsC.clear();
    const size_t maxInfo = (bytes - offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo)) /
                           sizeof(STORAGE_TEMPERATURE_INFO);
    for (size_t i = 0; i < std::min<size_t>(td->InfoCount, maxInfo); ++i) {
      const SHORT t = td->TemperatureInfo[i].Temperature;
      if (t > -40 && t < 150) d.tempsC.push_back(t);
    }
    if (d.tempsC.empty()) d.noTemp = true;
  } else {
    d.noTemp = true;
  }
  CloseHandle(h);
}

void SystemSensors::Collect(const CpuStats& cpu, const GpuStats& gpu, bool gpuFromNvml, const RamStats& ram,
                            const BatteryStats& bat, SensorList& out) {
  auto add = [&](std::string id, const std::string& group, std::string name, const char* unit, double v) {
    out.push_back({std::move(id), group, std::move(name), unit, v, v, v});
  };

  // ---- CPU
  const std::string cpuGroup = cpu.name.empty() ? "CPU" : cpu.name;
  if (cpu.usage) add("cpu:usage", cpuGroup, "Utilizzo totale", "%", *cpu.usage);
  if (cpu.topCoreUsage) add("cpu:topcore", cpuGroup, "Core più carico", "%", *cpu.topCoreUsage);
  if (cpu.freqGHz) add("cpu:freq", cpuGroup, "Frequenza media", "MHz", *cpu.freqGHz * 1000.0);
  if (cpu.maxFreqGHz) add("cpu:maxfreq", cpuGroup, "Frequenza massima", "MHz", *cpu.maxFreqGHz * 1000.0);
  for (const auto& [core, u] : cpu.coreUsage)
    add(std::format("cpu:core{}:usage", core), cpuGroup, std::format("Core {} utilizzo", core), "%", u);
  for (const auto& [core, ghz] : cpu.coreGHz)
    add(std::format("cpu:core{}:clock", core), cpuGroup, std::format("Core {} frequenza", core), "MHz", ghz * 1000.0);

  // ---- GPU (da Windows; per le NVIDIA i dati completi arrivano da NVML)
  if (!gpuFromNvml && !gpu.name.empty()) {
    if (gpu.usage) add("gpu:usage", gpu.name, "Utilizzo", "%", *gpu.usage);
    if (gpu.vramUsedGB) add("gpu:vramused", gpu.name, "VRAM usata", "GB", *gpu.vramUsedGB);
    if (gpu.vramTotalGB) add("gpu:vramtotal", gpu.name, "VRAM totale", "GB", *gpu.vramTotalGB);
    if (gpu.tempC) add("gpu:temp", gpu.name, "Temperatura", "°C", *gpu.tempC);
    if (gpu.clockMHz) add("gpu:clock", gpu.name, "Frequenza", "MHz", *gpu.clockMHz);
    if (gpu.powerW) add("gpu:power", gpu.name, "Consumo", "W", *gpu.powerW);
  }

  // ---- Memoria
  const std::string memGroup = "Memoria di sistema";
  MEMORYSTATUSEX ms{sizeof(ms)};
  if (GlobalMemoryStatusEx(&ms)) {
    add("ram:used", memGroup, "RAM usata", "GB", (ms.ullTotalPhys - ms.ullAvailPhys) / kGB);
    add("ram:free", memGroup, "RAM disponibile", "GB", ms.ullAvailPhys / kGB);
    add("ram:total", memGroup, "RAM totale", "GB", ms.ullTotalPhys / kGB);
    add("ram:load", memGroup, "RAM usata %", "%", ms.dwMemoryLoad);
    add("ram:commit", memGroup, "Memoria impegnata", "GB", (ms.ullTotalPageFile - ms.ullAvailPageFile) / kGB);
    add("ram:commitlimit", memGroup, "Limite memoria impegnata", "GB", ms.ullTotalPageFile / kGB);
  }
  if (ram.speedMTs > 0) add("ram:speed", memGroup, "Velocità RAM", "MT/s", ram.speedMTs);

  if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
    // ---- Dischi: istanze "0 C:", "1 D: E:"
    ReadDiskInfo();
    const auto idle = PdhReadArray(diskIdle_), rd = PdhReadArray(diskRead_), wr = PdhReadArray(diskWrite_);
    auto find = [](const auto& arr, const std::wstring& inst) -> std::optional<double> {
      for (const auto& [n, v] : arr)
        if (n == inst) return v;
      return std::nullopt;
    };
    for (const auto& [inst, idlePct] : idle) {
      if (inst == L"_Total") continue;
      const int num = _wtoi(inst.c_str());
      const auto space = inst.find(L' ');
      const std::string letters = space != std::wstring::npos ? ToUtf8(inst.substr(space + 1)) : "";
      DiskInfo& di = disks_[num];
      ReadDiskTemps(num, di);
      std::string group = std::format("Disco {}", num);
      if (!di.name.empty()) group += " · " + di.name;
      if (!letters.empty()) group += " (" + letters + ")";
      const std::string pre = std::format("disk:{}:", num);
      for (size_t t = 0; t < di.tempsC.size(); ++t)
        add(pre + (t == 0 ? std::string("temp") : std::format("temp{}", t)), group,
            t == 0 ? std::string("Temperatura") : std::format("Temperatura sensore {}", t), "°C", di.tempsC[t]);
      add(pre + "active", group, "Attività", "%", std::clamp(100.0 - idlePct, 0.0, 100.0));
      if (auto v = find(rd, inst)) add(pre + "read", group, "Lettura", "MB/s", *v / (1024.0 * 1024.0));
      if (auto v = find(wr, inst)) add(pre + "write", group, "Scrittura", "MB/s", *v / (1024.0 * 1024.0));
    }

    // ---- Rete (solo le schede che hanno avuto traffico)
    const auto tx = PdhReadArray(netTx_);
    for (const auto& [inst, rx] : PdhReadArray(netRx_)) {
      if (IgnoredNic(inst)) continue;
      const double sent = find(tx, inst).value_or(0);
      if (rx > 0 || sent > 0) activeNics_.insert(inst);
      if (!activeNics_.contains(inst)) continue;
      const std::string group = "Rete · " + ToUtf8(inst), pre = "net:" + ToUtf8(inst) + ":";
      add(pre + "rx", group, "Download", "Mbit/s", rx * 8.0 / 1e6);
      add(pre + "tx", group, "Upload", "Mbit/s", sent * 8.0 / 1e6);
    }

    // ---- Zone termiche ACPI (su molti desktop non esistono o sono fisse)
    for (const auto& [inst, raw] : PdhReadArray(thermal_)) {
      const double c = (thermalTenths_ ? raw / 10.0 : raw) - 273.15;
      if (c <= 0 || c > 150) continue;
      std::string name = ToUtf8(inst);
      if (const auto dot = name.rfind('.'); dot != std::string::npos) name = name.substr(dot + 1);
      add("tz:" + ToUtf8(inst), "Zone termiche ACPI", "Zona " + name, "°C", c);
    }
  }

  // ---- Batteria
  if (bat.present) {
    const std::string g = "Batteria";
    if (bat.percent >= 0) add("bat:pct", g, "Livello", "%", bat.percent);
    if (bat.rateW) add("bat:rate", g, bat.charging ? "Potenza di carica" : "Consumo", "W", *bat.rateW);
    if (bat.remainMin) add("bat:remain", g, "Autonomia", "min", *bat.remainMin);
    SYSTEM_BATTERY_STATE st{};
    if (CallNtPowerInformation(SystemBatteryState, nullptr, 0, &st, sizeof(st)) == 0 && st.BatteryPresent) {
      if (st.RemainingCapacity) add("bat:capacity", g, "Carica residua", "mWh", st.RemainingCapacity);
      if (st.MaxCapacity) add("bat:maxcapacity", g, "Capacità piena", "mWh", st.MaxCapacity);
    }
  }

  // ---- Sistema
  PERFORMANCE_INFORMATION pi{sizeof(pi)};
  if (GetPerformanceInfo(&pi, sizeof(pi))) {
    add("sys:processes", "Sistema", "Processi", "", pi.ProcessCount);
    add("sys:threads", "Sistema", "Thread", "", pi.ThreadCount);
    add("sys:handles", "Sistema", "Handle", "", pi.HandleCount);
  }
  add("sys:uptime", "Sistema", "Tempo di attività", "h", GetTickCount64() / 3600000.0);
}

}  // namespace po
