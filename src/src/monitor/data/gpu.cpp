#include "monitor/data/gpu.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "common/log.h"
#include "common/util.h"
#include "monitor/data/pdh_util.h"

using Microsoft::WRL::ComPtr;

namespace po {
namespace {
constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

// "pid_1234_luid_0x00000000_0x0000C4E6_phys_0_eng_3_engtype_3D"
bool ParseEngine(const std::wstring& n, unsigned& pid, unsigned& hi, unsigned& lo, unsigned& eng) {
  unsigned phys = 0;
  return swscanf_s(n.c_str(), L"pid_%u_luid_0x%x_0x%x_phys_%u_eng_%u", &pid, &hi, &lo, &phys, &eng) == 5;
}

// "luid_0x00000000_0x0000C4E6_phys_0"
bool ParseAdapterMem(const std::wstring& n, unsigned& hi, unsigned& lo) {
  unsigned phys = 0;
  return swscanf_s(n.c_str(), L"luid_0x%x_0x%x_phys_%u", &hi, &lo, &phys) == 3;
}
}  // namespace

GpuMonitor::~GpuMonitor() {
  if (query_) PdhCloseQuery(query_);
}

bool GpuMonitor::Init() {
  ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
      DXGI_ADAPTER_DESC1 d;
      if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
      Adapter ad;
      ad.luidHi = uint32_t(d.AdapterLuid.HighPart);
      ad.luidLo = d.AdapterLuid.LowPart;
      ad.name = Trim(ToUtf8(d.Description));
      ad.vendorId = d.VendorId;
      ad.deviceId = d.DeviceId;
      ad.subSysId = d.SubSysId;
      ad.revision = d.Revision;
      ad.dedicatedBytes = d.DedicatedVideoMemory;
      adapters_.push_back(std::move(ad));
    }
  }

  if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
    if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engineUtil_) != ERROR_SUCCESS)
      LogWarn("Contatore 'GPU Engine' non disponibile: utilizzo GPU assente");
    PdhAddEnglishCounterW(query_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &dedicatedUsage_);
    PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &processDedicated_);
    PdhCollectQueryData(query_);
  }

  RemoveVirtualAdapters();
  for (const auto& ad : adapters_)
    LogInfo("GPU: {} (vendor 0x{:04X}, {:.1f} GB, LUID {:08X}:{:08X})", ad.name, ad.vendorId,
            ad.dedicatedBytes / kGB, ad.luidHi, ad.luidLo);
  if (adapters_.empty()) LogWarn("Nessun adattatore DXGI hardware trovato");

  // GPU predefinita: quella con più VRAM dedicata (tipicamente la discreta).
  for (size_t i = 0; i < adapters_.size(); ++i)
    if (selected_ < 0 || adapters_[i].dedicatedBytes > adapters_[size_t(selected_)].dedicatedBytes)
      selected_ = int(i);
  return !adapters_.empty();
}

// I driver di display indiretto (display virtuali di Sunshine, Parsec, SudoMaker, ...) creano
// adattatori DXGI con LUID propri ma con la descrizione della GPU che renderizza: la stessa
// scheda comparirebbe più volte. Gli adattatori reali hanno contatori "GPU Adapter Memory";
// quelli virtuali no.
void GpuMonitor::RemoveVirtualAdapters() {
  std::vector<std::pair<unsigned, unsigned>> real;
  for (const auto& [inst, v] : PdhReadArray(dedicatedUsage_)) {
    unsigned hi, lo;
    if (ParseAdapterMem(inst, hi, lo)) real.emplace_back(hi, lo);
  }
  auto isReal = [&](const Adapter& a) {
    return std::find(real.begin(), real.end(), std::pair<unsigned, unsigned>(a.luidHi, a.luidLo)) != real.end();
  };
  if (!real.empty()) {
    std::erase_if(adapters_, [&](const Adapter& a) {
      if (isReal(a)) return false;
      LogInfo("GPU ignorata (adattatore virtuale senza contatori): {} LUID {:08X}:{:08X}", a.name, a.luidHi, a.luidLo);
      return true;
    });
    return;
  }
  // Contatori assenti: una sola voce per modello di scheda (due schede identiche reali verrebbero
  // unite, ma senza contatori non potremmo comunque distinguerne l'utilizzo).
  std::vector<Adapter> unique;
  for (auto& a : adapters_) {
    const bool dup = std::any_of(unique.begin(), unique.end(), [&](const Adapter& u) {
      return u.vendorId == a.vendorId && u.deviceId == a.deviceId && u.subSysId == a.subSysId &&
             u.revision == a.revision;
    });
    if (dup)
      LogInfo("GPU duplicata ignorata: {} LUID {:08X}:{:08X}", a.name, a.luidHi, a.luidLo);
    else
      unique.push_back(std::move(a));
  }
  adapters_ = std::move(unique);
}

const GpuMonitor::Adapter* GpuMonitor::FindAdapter(uint32_t hi, uint32_t lo) const {
  for (const auto& a : adapters_)
    if (a.luidHi == hi && a.luidLo == lo) return &a;
  return nullptr;
}

GpuStats GpuMonitor::Sample(DWORD gamePid, DWORD watchPid, ProcessGpu* watch) {
  GpuStats s;
  if (adapters_.empty()) return s;

  using Luid = std::pair<uint32_t, uint32_t>;
  std::map<std::pair<Luid, unsigned>, double> perEngine;  // somma su tutti i processi
  std::map<Luid, double> gameUse;                          // uso del solo processo del gioco
  std::map<std::pair<Luid, unsigned>, double> watch3D;     // motori 3D del processo osservato
  if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
    for (const auto& [inst, v] : PdhReadArray(engineUtil_)) {
      unsigned pid, hi, lo, eng;
      if (!ParseEngine(inst, pid, hi, lo, eng)) continue;
      perEngine[{{hi, lo}, eng}] += v;
      if (gamePid && pid == gamePid) gameUse[{hi, lo}] += v;
      if (watchPid && pid == watchPid && inst.find(L"engtype_3D") != std::wstring::npos)
        watch3D[{{hi, lo}, eng}] += v;
    }
  }

  // GPU multipla: l'adattatore su cui il gioco consuma di più è quello che lo renderizza.
  if (!gameUse.empty()) {
    const auto best = std::max_element(gameUse.begin(), gameUse.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    if (best->second > 0.5)
      for (size_t i = 0; i < adapters_.size(); ++i)
        if (adapters_[i].luidHi == best->first.first && adapters_[i].luidLo == best->first.second)
          selected_ = int(i);
  }
  const Adapter& ad = adapters_[size_t(std::max(selected_, 0))];
  s.name = ad.name;
  s.vendorId = ad.vendorId;
  if (ad.dedicatedBytes > 0) s.vramTotalGB = ad.dedicatedBytes / kGB;

  if (engineUtil_) {
    double maxEngine = 0;
    for (const auto& [key, v] : perEngine)
      if (key.first.first == ad.luidHi && key.first.second == ad.luidLo) maxEngine = std::max(maxEngine, v);
    s.usage = std::clamp(maxEngine, 0.0, 100.0);
  }
  for (const auto& [inst, v] : PdhReadArray(dedicatedUsage_)) {
    unsigned hi, lo;
    if (ParseAdapterMem(inst, hi, lo) && hi == ad.luidHi && lo == ad.luidLo) s.vramUsedGB = v / kGB;
  }

  if (watch) {
    *watch = {};
    watch->pid = watchPid;
    if (watchPid && engineUtil_) {
      watch->valid = true;
      for (const auto& [key, v] : watch3D) watch->usage3D = std::max(watch->usage3D, std::clamp(v, 0.0, 100.0));
      for (const auto& [inst, v] : PdhReadArray(processDedicated_)) {
        unsigned pid = 0, hi, lo, phys;
        if (swscanf_s(inst.c_str(), L"pid_%u_luid_0x%x_0x%x_phys_%u", &pid, &hi, &lo, &phys) == 4 && pid == watchPid)
          watch->vramMB += v / (1024.0 * 1024.0);
      }
    }
  }
  return s;
}

}  // namespace po
