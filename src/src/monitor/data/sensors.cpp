#include "monitor/data/sensors.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>

#include <winhttp.h>

#include <nlohmann/json.hpp>

#include "common/lhm.h"
#include "common/log.h"
#include "common/util.h"

namespace po {
namespace {

// Layout pubblico della shared memory di HWiNFO ("HWiNFO_SENS_SM2").
#pragma pack(push, 1)
struct HwiHeader {
  DWORD signature, version, revision;
  __int64 pollTime;
  DWORD sensorOffset, sensorSize, sensorCount;
  DWORD readingOffset, readingSize, readingCount;
};
struct HwiSensor {
  DWORD id, instance;
  char nameOrig[128], nameUser[128];
};
struct HwiReading {
  DWORD type, sensorIndex, id;
  char labelOrig[128], labelUser[128], unit[16];
  double value, valueMin, valueMax, valueAvg;
};
#pragma pack(pop)
constexpr DWORD kHwiTemp = 1, kHwiPower = 5, kHwiClock = 6;
constexpr DWORD kHwiDead = 0x44414544;  // 'DEAD': HWiNFO chiuso o limite SHM scaduto

std::string CStr(const char* s, size_t max) { return std::string(s, strnlen(s, max)); }

// Le stringhe di HWiNFO sono nella codepage ANSI ("°C" = 0xB0).
std::string Ansi(const char* s, size_t max) {
  const int n = int(strnlen(s, max));
  if (n == 0) return {};
  std::wstring w(size_t(MultiByteToWideChar(CP_ACP, 0, s, n, nullptr, 0)), L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, n, w.data(), int(w.size()));
  return ToUtf8(w);
}

// Unità delle letture di LibreHardwareMonitor / OpenHardwareMonitor per tipo di sensore.
const char* LhmUnit(const std::wstring& type, double& scale) {
  scale = 1.0;
  if (type == L"Temperature") return "°C";
  if (type == L"Voltage") return "V";
  if (type == L"Current") return "A";
  if (type == L"Power") return "W";
  if (type == L"Clock") return "MHz";
  if (type == L"Load" || type == L"Control" || type == L"Level" || type == L"Humidity") return "%";
  if (type == L"Frequency") return "Hz";
  if (type == L"Fan") return "RPM";
  if (type == L"Flow") return "L/h";
  if (type == L"Data") return "GB";
  if (type == L"SmallData") return "MB";
  if (type == L"Throughput") {
    scale = 1.0 / (1024.0 * 1024.0);
    return "MB/s";
  }
  if (type == L"TimeSpan") return "s";
  if (type == L"Energy") return "mWh";
  if (type == L"Noise") return "dBA";
  return "";
}

// Priorità delle etichette per la temperatura CPU (più bassa = migliore).
int CpuTempRank(std::string_view label) {
  static const char* kOrder[] = {"CPU Package", "CPU (Tctl/Tdie)", "Core (Tctl/Tdie)", "CPU Die (average)",
                                 "Core Max", "CPU Core", "CPU"};
  for (int i = 0; i < int(std::size(kOrder)); ++i)
    if (IEquals(label, kOrder[i])) return i;
  return 1000;
}

}  // namespace

SensorHub::~SensorHub() {
  if (hwiView_) UnmapViewOfFile(hwiView_);
  if (hwiMap_) CloseHandle(hwiMap_);
}

SensorReadings SensorHub::Read() {
  SensorReadings r;
  if (ReadHwinfo(r)) return r;
  r = {};
  if (ReadWmi(r)) return r;
  r = {};
  ReadLhmHttp(r);
  return r;
}

// LibreHardwareMonitor con il server web attivo: albero JSON di hardware e sensori (/data.json).
bool SensorHub::ReadLhmHttp(SensorReadings& out) {
  const ULONGLONG now = GetTickCount64();
  if (now < nextHttpTry_) return false;
  std::string body;
  {
    HINTERNET s = WinHttpOpen(L"PerfOverlaySupreme", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"127.0.0.1", INTERNET_PORT(kLhmPort), 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/data.json", nullptr, nullptr, nullptr, 0) : nullptr;
    if (s) WinHttpSetTimeouts(s, 300, 300, 500, 800);
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(r, chunk.data(), avail, &read) || read == 0) break;
        body.append(chunk.data(), read);
      }
    }
    for (HINTERNET h : {r, c, s})
      if (h) WinHttpCloseHandle(h);
  }
  if (body.empty()) {
    nextHttpTry_ = now + 5000;  // LibreHardwareMonitor non in esecuzione: riprova tra un po'
    return false;
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(body);
  } catch (...) {
    nextHttpTry_ = now + 5000;
    return false;
  }

  int bestCpuRank = 1000;
  std::map<std::string, SensorReadings::Gpu> gpus;
  // Visita: i nodi hardware hanno "HardwareId", i sensori "SensorId" / "Type" / "RawValue".
  auto walk = [&](auto&& self, const nlohmann::json& n, const std::string& hwId, const std::string& hwName) -> void {
    std::string id = hwId, name = hwName;
    if (n.contains("HardwareId") && n["HardwareId"].is_string()) {
      id = n["HardwareId"].get<std::string>();
      name = n.value("Text", "");
    }
    if (n.contains("SensorId") && n["SensorId"].is_string()) {
      const std::string type = n.value("Type", "");
      double v = 0;
      bool ok = false;
      if (n.contains("RawValue") && n["RawValue"].is_number()) {
        v = n["RawValue"].get<double>();
        ok = true;
      } else if (n.contains("Value") && n["Value"].is_string()) {
        std::string s = n["Value"].get<std::string>();
        std::replace(s.begin(), s.end(), ',', '.');
        try {
          v = std::stod(s);
          ok = true;
        } catch (...) {
        }
      }
      if (ok) {
        double scale = 1.0;
        const char* unit = LhmUnit(ToWide(type), scale);
        const std::string label = n.value("Text", "");
        v *= scale;
        out.all.push_back({"lhm:" + n["SensorId"].get<std::string>(), name, label, unit, v, v, v});
        const bool cpu = id.find("cpu") != std::string::npos, gpu = id.find("gpu") != std::string::npos;
        if (cpu && type == "Temperature") {
          const int rank = CpuTempRank(label);
          if (rank < bestCpuRank) {
            bestCpuRank = rank;
            out.cpuTempC = v;
          }
        }
        if (cpu && type == "Power" && (IEquals(label, "Package") || IEquals(label, "CPU Package"))) out.cpuPowerW = v;
        if (gpu && IEquals(label, "GPU Core") && (type == "Temperature" || type == "Clock")) {
          auto& g = gpus[id];
          g.name = name;
          (type == "Temperature" ? g.tempC : g.clockMHz) = v;
        }
        if (gpu && type == "Power" && (IEquals(label, "GPU Package") || IEquals(label, "GPU Power"))) {
          auto& g = gpus[id];
          g.name = name;
          g.powerW = v;
        }
      }
    }
    if (n.contains("Children") && n["Children"].is_array())
      for (const auto& c : n["Children"]) self(self, c, id, name);
  };
  walk(walk, root, "", "");
  for (auto& [id, g] : gpus) out.gpus.push_back(std::move(g));
  out.source = "LibreHardwareMonitor";
  return !out.all.empty();
}

bool SensorHub::ReadHwinfo(SensorReadings& out) {
  const ULONGLONG now = GetTickCount64();
  if (!hwiView_) {
    if (now < nextHwiTry_) return false;
    nextHwiTry_ = now + 10000;
    hwiMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    if (!hwiMap_) return false;
    hwiView_ = static_cast<const BYTE*>(MapViewOfFile(hwiMap_, FILE_MAP_READ, 0, 0, 0));
    MEMORY_BASIC_INFORMATION mbi{};
    if (!hwiView_ || !VirtualQuery(hwiView_, &mbi, sizeof(mbi))) {
      if (hwiView_) UnmapViewOfFile(hwiView_);
      CloseHandle(hwiMap_);
      hwiView_ = nullptr;
      hwiMap_ = nullptr;
      return false;
    }
    hwiSize_ = mbi.RegionSize;
    LogInfo("Sensori: HWiNFO shared memory trovata");
  }

  const auto* h = reinterpret_cast<const HwiHeader*>(hwiView_);
  auto inBounds = [&](uint64_t off, uint64_t size, uint64_t count) {
    return off + size * count <= hwiSize_ && size > 0;
  };
  if (hwiSize_ < sizeof(HwiHeader) || h->signature == kHwiDead ||
      !inBounds(h->sensorOffset, h->sensorSize, h->sensorCount) ||
      !inBounds(h->readingOffset, h->readingSize, h->readingCount) || h->sensorSize < sizeof(HwiSensor) ||
      h->readingSize < sizeof(HwiReading)) {
    // Mappatura non più valida: rilascia e riprova più tardi.
    UnmapViewOfFile(hwiView_);
    CloseHandle(hwiMap_);
    hwiView_ = nullptr;
    hwiMap_ = nullptr;
    return false;
  }

  auto sensorName = [&](DWORD idx) -> std::string {
    if (idx >= h->sensorCount) return {};
    const auto* s = reinterpret_cast<const HwiSensor*>(hwiView_ + h->sensorOffset + uint64_t(idx) * h->sensorSize);
    return CStr(s->nameOrig, sizeof(s->nameOrig));
  };

  auto sensorAt = [&](DWORD idx) {
    return reinterpret_cast<const HwiSensor*>(hwiView_ + h->sensorOffset + uint64_t(idx) * h->sensorSize);
  };

  int bestCpuRank = 1000;
  std::map<DWORD, SensorReadings::Gpu> gpus;  // per indice sensore
  out.all.reserve(h->readingCount);
  for (DWORD i = 0; i < h->readingCount; ++i) {
    const auto* rd =
        reinterpret_cast<const HwiReading*>(hwiView_ + h->readingOffset + uint64_t(i) * h->readingSize);
    const std::string label = CStr(rd->labelOrig, sizeof(rd->labelOrig));
    if (rd->type != 0 && rd->sensorIndex < h->sensorCount) {  // elenco completo (nomi personalizzati dall'utente)
      const auto* sn = sensorAt(rd->sensorIndex);
      std::string group = Ansi(sn->nameUser, sizeof(sn->nameUser));
      if (group.empty()) group = Ansi(sn->nameOrig, sizeof(sn->nameOrig));
      std::string name = Ansi(rd->labelUser, sizeof(rd->labelUser));
      if (name.empty()) name = Ansi(rd->labelOrig, sizeof(rd->labelOrig));
      out.all.push_back({std::format("hwi:{}.{}:{}", sn->id, sn->instance, rd->id), std::move(group), std::move(name),
                         Ansi(rd->unit, sizeof(rd->unit)), rd->value, rd->valueMin, rd->valueMax});
    }
    if (rd->type == kHwiTemp) {
      const std::string sensor = sensorName(rd->sensorIndex);
      if (IContains(sensor, "CPU")) {
        const int rank = CpuTempRank(label);
        if (rank < bestCpuRank) {
          bestCpuRank = rank;
          out.cpuTempC = rd->value;
        }
      }
      if (IEquals(label, "GPU Temperature")) {
        auto& g = gpus[rd->sensorIndex];
        g.name = sensor;
        g.tempC = rd->value;
      }
    } else if (rd->type == kHwiPower) {
      const std::string sensor = sensorName(rd->sensorIndex);
      if (IEquals(label, "CPU Package Power") || (IContains(sensor, "CPU") && IEquals(label, "Package Power")))
        out.cpuPowerW = rd->value;
      if (IEquals(label, "GPU Power") || IEquals(label, "Total Board Power") || IEquals(label, "GPU Board Power")) {
        auto& g = gpus[rd->sensorIndex];
        g.name = sensor;
        g.powerW = rd->value;
      }
    } else if (rd->type == kHwiClock && IEquals(label, "GPU Clock")) {
      auto& g = gpus[rd->sensorIndex];
      g.name = sensorName(rd->sensorIndex);
      g.clockMHz = rd->value;
    }
  }
  for (auto& [idx, g] : gpus) out.gpus.push_back(std::move(g));
  out.source = "HWiNFO";
  return out.cpuTempC || !out.gpus.empty() || !out.all.empty();
}

bool SensorHub::ReadWmi(SensorReadings& out) {
  const ULONGLONG now = GetTickCount64();
  if (!wmi_.Connected()) {
    if (now < nextWmiTry_) return false;
    nextWmiTry_ = now + 30000;
    if (wmi_.Connect(L"root\\LibreHardwareMonitor")) {
      wmiName_ = "LibreHardwareMonitor";
    } else if (wmi_.Connect(L"root\\OpenHardwareMonitor")) {
      wmiName_ = "OpenHardwareMonitor";
    } else {
      return false;
    }
    LogInfo("Sensori: {} via WMI", wmiName_);
  }

  std::map<std::wstring, std::string> hwNames;  // Identifier → Name
  for (auto& row : wmi_.Query(L"SELECT Identifier, Name FROM Hardware", {L"Identifier", L"Name"}))
    hwNames[row[L"Identifier"].str] = ToUtf8(row[L"Name"].str);

  int bestCpuRank = 1000;
  std::map<std::wstring, SensorReadings::Gpu> gpus;
  const auto rows = wmi_.Query(L"SELECT Identifier, Name, SensorType, Value, Min, Max, Parent FROM Sensor",
                               {L"Identifier", L"Name", L"SensorType", L"Value", L"Min", L"Max", L"Parent"});
  for (auto row : rows) {
    const std::string name = ToUtf8(row[L"Name"].str);
    const std::wstring& parent = row[L"Parent"].str;
    if (row[L"Value"].isNum) {  // elenco completo
      double scale = 1.0;
      const char* unit = LhmUnit(row[L"SensorType"].str, scale);
      const double v = row[L"Value"].num * scale;
      out.all.push_back({"lhm:" + ToUtf8(row[L"Identifier"].str), hwNames[parent], name, unit, v,
                         row[L"Min"].isNum ? row[L"Min"].num * scale : v,
                         row[L"Max"].isNum ? row[L"Max"].num * scale : v});
    }
    const std::wstring& type = row[L"SensorType"].str;
    if (type != L"Temperature" && type != L"Clock" && type != L"Power") continue;
    const bool isTemp = row[L"SensorType"].str == L"Temperature";
    const bool isPower = row[L"SensorType"].str == L"Power";
    const auto& v = row[L"Value"];
    if (!v.isNum) continue;
    if (isPower) {
      if (parent.find(L"cpu") != std::wstring::npos && IEquals(name, "CPU Package")) out.cpuPowerW = v.num;
      if (parent.find(L"gpu") != std::wstring::npos && (IEquals(name, "GPU Package") || IEquals(name, "GPU Power"))) {
        auto& g = gpus[parent];
        g.name = hwNames[parent];
        g.powerW = v.num;
      }
      continue;
    }
    if (parent.find(L"cpu") != std::wstring::npos && isTemp) {
      const int rank = CpuTempRank(name);
      if (rank < bestCpuRank) {
        bestCpuRank = rank;
        out.cpuTempC = v.num;
      }
    } else if (parent.find(L"gpu") != std::wstring::npos && IEquals(name, "GPU Core")) {
      auto& g = gpus[parent];
      g.name = hwNames[parent];
      (isTemp ? g.tempC : g.clockMHz) = v.num;
    }
  }
  for (auto& [id, g] : gpus) out.gpus.push_back(std::move(g));
  out.source = wmiName_;
  return out.cpuTempC || !out.gpus.empty() || !out.all.empty();
}

const SensorReadings::Gpu* MatchGpu(const SensorReadings& r, const std::string& adapterName) {
  const SensorReadings::Gpu* best = nullptr;
  int bestScore = -1;
  // Toglie il prefisso del produttore: "NVIDIA GeForce RTX 4070" → "GeForce RTX 4070".
  std::string key = adapterName;
  for (const char* prefix : {"NVIDIA ", "AMD ", "Intel(R) "})
    if (key.rfind(prefix, 0) == 0) key = key.substr(strlen(prefix));
  for (const auto& g : r.gpus) {
    const int score = IContains(g.name, key) ? 2 : (IContains(g.name, "GPU") ? 1 : 0);
    if (score > bestScore) {
      bestScore = score;
      best = &g;
    }
  }
  // Con una sola GPU nei sensori la usiamo comunque; con più GPU serve una corrispondenza di nome.
  if (r.gpus.size() > 1 && bestScore < 2) return nullptr;
  return best;
}

}  // namespace po
