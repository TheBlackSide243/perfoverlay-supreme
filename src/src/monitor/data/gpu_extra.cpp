#include "monitor/data/gpu_extra.h"

#include <winternl.h>

#include <d3dkmthk.h>

#include <cstdlib>
#include <cstring>
#include <format>

#include "common/i18n.h"
#include "common/log.h"
#include "common/util.h"

namespace po {

// ------------------------------------------------------------------ Windows (WDDM)
GpuExtra ReadWddmGpu(uint32_t luidHi, uint32_t luidLo) {
  GpuExtra out;
  D3DKMT_OPENADAPTERFROMLUID open{};
  open.AdapterLuid.HighPart = LONG(luidHi);
  open.AdapterLuid.LowPart = luidLo;
  if (D3DKMTOpenAdapterFromLuid(&open) != 0) return out;

  D3DKMT_ADAPTER_PERFDATA perf{};
  D3DKMT_QUERYADAPTERINFO query{};
  query.hAdapter = open.hAdapter;
  query.Type = KMTQAITYPE_ADAPTERPERFDATA;
  query.pPrivateDriverData = &perf;
  query.PrivateDriverDataSize = sizeof(perf);
  if (D3DKMTQueryAdapterInfo(&query) == 0) {
    // Temperatura in decimi di grado, consumo in decimi di punto percentuale. Zero = il driver non lo fornisce.
    if (perf.Temperature > 0 && perf.Temperature < 1500) out.tempC = perf.Temperature / 10.0;
    if (perf.FanRPM > 0 && perf.FanRPM < 20000) out.fanRpm = perf.FanRPM;
    if (perf.Power > 0 && perf.Power <= 2000) out.powerPct = perf.Power / 10.0;
  }
  D3DKMT_CLOSEADAPTER close{open.hAdapter};
  D3DKMTCloseAdapter(&close);
  return out;
}

// ------------------------------------------------------------------ AMD (ADL)
namespace {

constexpr int kAdlOk = 0;
constexpr int kAdlMaxPath = 256;
constexpr int kAdlMaxSensors = 256;
constexpr int kAmdVendor = 1002;  // ADL scrive l'identificativo PCI 0x1002 come numero decimale "1002"

// Disposizione dei campi dell'SDK ADL per Windows.
struct AdlAdapterInfo {
  int size;
  int adapterIndex;
  char udid[kAdlMaxPath];
  int busNumber, deviceNumber, functionNumber;
  int vendorId;
  char adapterName[kAdlMaxPath];
  char displayName[kAdlMaxPath];
  int present;
  int exist;
  char driverPath[kAdlMaxPath];
  char driverPathExt[kAdlMaxPath];
  char pnpString[kAdlMaxPath];
  int osDisplayIndex;
};
struct AdlSensor {
  int supported;
  int value;
};
struct AdlPmLogOutput {
  int size;
  AdlSensor sensors[kAdlMaxSensors];
};

// Tipi di sensore di ADL2_New_QueryPMLogData_Get usati qui (enum ADL_PMLOG_*).
enum AdlSensorType {
  kGfxClock = 1,
  kMemClock = 2,
  kTempEdge = 8,
  kTempMem = 9,
  kFanRpm = 14,
  kFanPct = 15,
  kSocPower = 17,
  kActivityGfx = 19,
  kGfxVoltage = 21,
  kAsicPower = 23,
  kTempHotspot = 27,
  kTempGfx = 28,
  kTempSoc = 29,
  kGfxPower = 30,
};
struct SensorInfo {
  int type;
  const char* key;
  const char* name;  // in italiano, tradotto con TU()
  const char* unit;
  double scale;
  double max;  // valori più alti sono letture non valide
};
const SensorInfo kSensors[] = {
    {kTempEdge, "temp", "Temperatura GPU", "°C", 1, 150},
    {kTempHotspot, "hotspot", "Temperatura punto più caldo", "°C", 1, 150},
    {kTempGfx, "tempgfx", "Temperatura grafica", "°C", 1, 150},
    {kTempSoc, "tempsoc", "Temperatura SoC", "°C", 1, 150},
    {kTempMem, "tempmem", "Temperatura memoria", "°C", 1, 150},
    {kAsicPower, "power", "Consumo ASIC", "W", 1, 1000},  // scheda dedicata: l'intera scheda; integrata: il processore
    {kGfxPower, "powergfx", "Consumo linea grafica", "W", 1, 1000},
    {kSocPower, "powersoc", "Consumo SoC", "W", 1, 1000},
    {kGfxClock, "clock", "Clock core", "MHz", 1, 10000},
    {kMemClock, "clockmem", "Clock memoria", "MHz", 1, 10000},
    {kFanRpm, "fanrpm", "Ventola", "RPM", 1, 20000},
    {kFanPct, "fan", "Ventola", "%", 1, 100},
    {kActivityGfx, "util", "Utilizzo GPU", "%", 1, 100},
    {kGfxVoltage, "voltage", "Tensione core", "V", 0.001, 3},
};

void* __stdcall AdlAlloc(int size) { return std::malloc(size_t(size)); }

}  // namespace

AdlReader::~AdlReader() {
  if (ctx_ && destroy_) destroy_(ctx_);
  if (mod_) FreeLibrary(mod_);
}

bool AdlReader::Init() {
  mod_ = LoadLibraryExW(L"atiadlxx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!mod_) return false;
  using FnCreate = int (*)(void* (__stdcall*)(int), int, void**);
  using FnCount = int (*)(void*, int*);
  using FnInfo = int (*)(void*, AdlAdapterInfo*, int);
  const auto create = reinterpret_cast<FnCreate>(GetProcAddress(mod_, "ADL2_Main_Control_Create"));
  const auto count = reinterpret_cast<FnCount>(GetProcAddress(mod_, "ADL2_Adapter_NumberOfAdapters_Get"));
  const auto info = reinterpret_cast<FnInfo>(GetProcAddress(mod_, "ADL2_Adapter_AdapterInfo_Get"));
  destroy_ = reinterpret_cast<int (*)(void*)>(GetProcAddress(mod_, "ADL2_Main_Control_Destroy"));
  pmlog_ = reinterpret_cast<int (*)(void*, int, void*)>(GetProcAddress(mod_, "ADL2_New_QueryPMLogData_Get"));
  if (!create || !count || !info || !pmlog_ || create(&AdlAlloc, 1, &ctx_) != kAdlOk || !ctx_) {
    LogInfo("ADL: libreria AMD presente ma non utilizzabile");
    return false;
  }
  int n = 0;
  if (count(ctx_, &n) != kAdlOk || n <= 0 || n > 64) return false;
  std::vector<AdlAdapterInfo> infos(size_t(n), AdlAdapterInfo{});
  if (info(ctx_, infos.data(), int(sizeof(AdlAdapterInfo) * infos.size())) != kAdlOk) return false;
  // ADL elenca una voce per uscita video: una scheda compare più volte con lo stesso bus PCI.
  for (const auto& a : infos) {
    if (a.vendorId != kAmdVendor || !a.present) continue;
    bool known = false;
    for (const auto& d : devs_) known = known || d.bus == a.busNumber;
    if (known) continue;
    Dev d;
    d.index = a.adapterIndex;
    d.bus = a.busNumber;
    d.name = Trim(std::string(a.adapterName, strnlen(a.adapterName, kAdlMaxPath)));
    std::vector<std::pair<int, int>> values;
    if (!Query(d, values) || values.empty()) continue;  // scheda senza sensori leggibili
    std::string list;  // tipo=valore di ogni sensore fornito dai driver: utile se un valore sembra sbagliato
    for (const auto& [type, value] : values) list += std::format(" {}={}", type, value);
    LogInfo("ADL: {} (bus {}), {} sensori:{}", d.name, d.bus, values.size(), list);
    devs_.push_back(std::move(d));
  }
  return !devs_.empty();
}

bool AdlReader::Query(const Dev& d, std::vector<std::pair<int, int>>& values) {
  AdlPmLogOutput data{};
  data.size = int(sizeof(data));
  if (!pmlog_ || pmlog_(ctx_, d.index, &data) != kAdlOk) return false;
  for (int i = 0; i < kAdlMaxSensors; ++i)
    if (data.sensors[i].supported) values.emplace_back(i, data.sensors[i].value);
  return true;
}

bool AdlReader::Read(const std::string& adapterName, GpuExtra& out) {
  const Dev* dev = nullptr;
  for (const auto& d : devs_)
    if (IContains(adapterName, d.name) || IContains(d.name, adapterName)) dev = &d;
  if (!dev && devs_.size() == 1) dev = &devs_[0];
  std::vector<std::pair<int, int>> values;
  if (!dev || !Query(*dev, values)) return false;
  auto get = [&](int type, double max) -> std::optional<double> {
    for (const auto& [t, v] : values)
      if (t == type && v > 0 && v <= max) return double(v);
    return std::nullopt;
  };
  // Schede dedicate: temperatura "edge" e consumo dell'intera scheda. Sulle grafiche integrate (senza sensore
  // "edge") i consumi di ADL sono quelli del processore intero, non della grafica: lì il consumo si omette.
  const bool dedicated = get(kTempEdge, 150).has_value();
  out.tempC = get(kTempEdge, 150);
  if (!out.tempC) out.tempC = get(kTempGfx, 150);
  out.hotspotC = get(kTempHotspot, 150);
  if (!out.tempC) out.tempC = out.hotspotC;
  if (dedicated) {
    out.powerW = get(kAsicPower, 1000);
    if (!out.powerW) out.powerW = get(kGfxPower, 1000);
  }
  out.clockMHz = get(kGfxClock, 10000);
  out.fanRpm = get(kFanRpm, 20000);
  out.fanPct = get(kFanPct, 100);
  return true;
}

void AdlReader::ReadAll(SensorList& out) {
  for (const auto& d : devs_) {
    std::vector<std::pair<int, int>> values;
    if (!Query(d, values)) continue;
    const std::string pre = "adl:" + std::to_string(d.bus) + ":";
    for (const auto& s : kSensors)
      for (const auto& [type, raw] : values) {
        if (type != s.type) continue;
        const double v = raw * s.scale;
        if (raw < 0 || v > s.max) continue;
        out.push_back({pre + s.key, d.name, TU(s.name), s.unit, v, v, v});
      }
  }
}

}  // namespace po
