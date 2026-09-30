#include "monitor/data/nvml.h"

#include "common/log.h"
#include "common/util.h"
#include "common/i18n.h"

namespace po {
namespace {
constexpr int kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;
}  // namespace

NvmlReader::~NvmlReader() {
  if (shutdown_) shutdown_();
  if (mod_) FreeLibrary(mod_);
}

bool NvmlReader::Init() {
  // Solo percorsi di sistema/driver: non cerchiamo la DLL nella cartella corrente.
  mod_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!mod_) {
    wchar_t pf[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH)) {
      const std::wstring p = std::wstring(pf) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
      mod_ = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
  }
  if (!mod_) return false;

  auto init = reinterpret_cast<Fn0>(GetProcAddress(mod_, "nvmlInit_v2"));
  auto count = reinterpret_cast<FnCount>(GetProcAddress(mod_, "nvmlDeviceGetCount_v2"));
  auto handle = reinterpret_cast<FnHandle>(GetProcAddress(mod_, "nvmlDeviceGetHandleByIndex_v2"));
  auto name = reinterpret_cast<FnName>(GetProcAddress(mod_, "nvmlDeviceGetName"));
  shutdown_ = reinterpret_cast<Fn0>(GetProcAddress(mod_, "nvmlShutdown"));
  temp_ = reinterpret_cast<FnTemp>(GetProcAddress(mod_, "nvmlDeviceGetTemperature"));
  clock_ = reinterpret_cast<FnClock>(GetProcAddress(mod_, "nvmlDeviceGetClockInfo"));
  power_ = reinterpret_cast<FnPower>(GetProcAddress(mod_, "nvmlDeviceGetPowerUsage"));  // mW, opzionale
  // Solo per la finestra dei sensori, tutte opzionali.
  powerLimit_ = reinterpret_cast<FnPower>(GetProcAddress(mod_, "nvmlDeviceGetEnforcedPowerLimit"));
  fanSpeed_ = reinterpret_cast<FnPower>(GetProcAddress(mod_, "nvmlDeviceGetFanSpeed"));
  numFans_ = reinterpret_cast<FnCountDev>(GetProcAddress(mod_, "nvmlDeviceGetNumFans"));
  fanSpeed2_ = reinterpret_cast<FnFan2>(GetProcAddress(mod_, "nvmlDeviceGetFanSpeed_v2"));
  util_ = reinterpret_cast<FnUtil>(GetProcAddress(mod_, "nvmlDeviceGetUtilizationRates"));
  mem_ = reinterpret_cast<FnMem>(GetProcAddress(mod_, "nvmlDeviceGetMemoryInfo"));
  pstate_ = reinterpret_cast<FnPstate>(GetProcAddress(mod_, "nvmlDeviceGetPerformanceState"));
  pcie_ = reinterpret_cast<FnPcie>(GetProcAddress(mod_, "nvmlDeviceGetPcieThroughput"));
  enc_ = reinterpret_cast<FnCodec>(GetProcAddress(mod_, "nvmlDeviceGetEncoderUtilization"));
  dec_ = reinterpret_cast<FnCodec>(GetProcAddress(mod_, "nvmlDeviceGetDecoderUtilization"));
  if (!init || !count || !handle || !name || !temp_ || !clock_ || init() != kNvmlSuccess) {
    shutdown_ = nullptr;
    FreeLibrary(mod_);
    mod_ = nullptr;
    return false;
  }
  unsigned n = 0;
  count(&n);
  for (unsigned i = 0; i < n; ++i) {
    void* h = nullptr;
    char buf[96] = {};
    if (handle(i, &h) == kNvmlSuccess && name(h, buf, sizeof(buf)) == kNvmlSuccess) devs_.push_back({h, buf});
  }
  LogInfo("NVML: {} GPU NVIDIA", devs_.size());
  return !devs_.empty();
}

bool NvmlReader::Read(const std::string& adapterName, std::optional<double>& tempC,
                      std::optional<double>& clockMHz, std::optional<double>& powerW) {
  const Dev* dev = nullptr;
  for (const auto& d : devs_)
    if (IContains(adapterName, d.name) || IContains(d.name, adapterName)) dev = &d;
  if (!dev && devs_.size() == 1) dev = &devs_[0];
  if (!dev) return false;
  unsigned v = 0;
  if (!tempC && temp_(dev->handle, kNvmlTemperatureGpu, &v) == kNvmlSuccess) tempC = v;
  if (!clockMHz && clock_(dev->handle, kNvmlClockGraphics, &v) == kNvmlSuccess) clockMHz = v;
  if (!powerW && power_ && power_(dev->handle, &v) == kNvmlSuccess) powerW = v / 1000.0;
  return true;
}

void NvmlReader::ReadAll(SensorList& out) {
  for (size_t i = 0; i < devs_.size(); ++i) {
    void* h = devs_[i].handle;
    const std::string pre = "nv:" + std::to_string(i) + ":";
    auto add = [&](const std::string& key, const std::string& name, const char* unit, double v) {
      out.push_back({pre + key, devs_[i].name, name, unit, v, v, v});
    };
    unsigned v = 0;
    if (temp_(h, kNvmlTemperatureGpu, &v) == kNvmlSuccess) add("temp", TU("Temperatura GPU"), "°C", v);
    unsigned fans = 0;
    if (numFans_ && fanSpeed2_ && numFans_(h, &fans) == kNvmlSuccess && fans > 0) {
      for (unsigned f = 0; f < fans && f < 8; ++f)
        if (fanSpeed2_(h, f, &v) == kNvmlSuccess)
          add("fan" + std::to_string(f), TU("Ventola ") + std::to_string(f + 1), "%", v);
    } else if (fanSpeed_ && fanSpeed_(h, &v) == kNvmlSuccess) {
      add("fan", TU("Ventola"), "%", v);
    }
    static const std::pair<int, const char*> kClocks[] = {
        {0, "Clock core"}, {1, "Clock SM"}, {2, TU("Clock memoria")}, {3, "Clock video"}};
    for (const auto& [type, name] : kClocks)
      if (clock_(h, type, &v) == kNvmlSuccess) add("clock" + std::to_string(type), name, "MHz", v);
    if (power_ && power_(h, &v) == kNvmlSuccess) add("power", TU("Consumo scheda"), "W", v / 1000.0);
    if (powerLimit_ && powerLimit_(h, &v) == kNvmlSuccess) add("powerlimit", TU("Limite di consumo"), "W", v / 1000.0);
    unsigned util[2] = {};
    if (util_ && util_(h, util) == kNvmlSuccess) {
      add("util", TU("Utilizzo GPU"), "%", util[0]);
      add("utilmem", TU("Utilizzo controller memoria"), "%", util[1]);
    }
    unsigned long long mem[3] = {};
    if (mem_ && mem_(h, mem) == kNvmlSuccess && mem[0] > 0) {
      add("vramused", TU("VRAM usata"), "GB", mem[2] / 1073741824.0);
      add("vramfree", TU("VRAM libera"), "GB", mem[1] / 1073741824.0);
      add("vramtotal", TU("VRAM totale"), "GB", mem[0] / 1073741824.0);
      add("vrampct", TU("VRAM usata %"), "%", 100.0 * double(mem[2]) / double(mem[0]));
    }
    int ps = 0;
    if (pstate_ && pstate_(h, &ps) == kNvmlSuccess && ps >= 0 && ps < 32) add("pstate", "P-state", "", ps);
    unsigned period = 0;
    if (enc_ && enc_(h, &v, &period) == kNvmlSuccess) add("enc", TU("Utilizzo encoder video"), "%", v);
    if (dec_ && dec_(h, &v, &period) == kNvmlSuccess) add("dec", TU("Utilizzo decoder video"), "%", v);
    // Throughput PCIe (KB/s): contatori 0 = TX, 1 = RX.
    if (pcie_ && pcie_(h, 1, &v) == kNvmlSuccess) add("pcierx", TU("PCIe ricezione"), "MB/s", v / 1024.0);
    if (pcie_ && pcie_(h, 0, &v) == kNvmlSuccess) add("pcietx", TU("PCIe invio"), "MB/s", v / 1024.0);
  }
}

}  // namespace po
