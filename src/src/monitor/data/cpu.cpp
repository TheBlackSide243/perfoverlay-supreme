#include "monitor/data/cpu.h"

#include <powerbase.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#include "common/log.h"
#include "common/util.h"
#include "monitor/data/pdh_util.h"

namespace po {
namespace {
// Non esposta dagli header dell'SDK (documentata in CallNtPowerInformation).
struct ProcessorPowerInformation {
  ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};

std::string ReadCpuName() {
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return {};
  return Trim(ToUtf8(buf));
}

bool IsTotalInstance(const std::wstring& n) { return n.find(L"_Total") != std::wstring::npos; }
}  // namespace

CpuMonitor::~CpuMonitor() {
  if (query_) PdhCloseQuery(query_);
}

bool CpuMonitor::Init() {
  name_ = ReadCpuName();
  if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
    LogWarn("PDH non disponibile per la CPU");
    return false;
  }
  // "% Processor Utility" corrisponde al Task Manager; fallback a "% Processor Time" su sistemi vecchi.
  if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Utility", 0, &totalUtil_) !=
      ERROR_SUCCESS) {
    PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Time", 0, &totalUtil_);
    PdhAddEnglishCounterW(query_, L"\\Processor Information(*)\\% Processor Time", 0, &coreUtil_);
  } else {
    PdhAddEnglishCounterW(query_, L"\\Processor Information(*)\\% Processor Utility", 0, &coreUtil_);
  }
  PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Performance", 0, &totalPerf_);
  PdhAddEnglishCounterW(query_, L"\\Processor Information(*)\\% Processor Performance", 0, &corePerf_);
  PdhCollectQueryData(query_);

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  std::vector<ProcessorPowerInformation> ppi(std::max<DWORD>(n, si.dwNumberOfProcessors));
  if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(),
                             ULONG(ppi.size() * sizeof(ProcessorPowerInformation))) == 0) {
    for (const auto& p : ppi) baseMhz_ = std::max(baseMhz_, double(p.MaxMhz));
  }
  LogInfo("CPU: {} (clock base {:.0f} MHz)", name_, baseMhz_);
  return true;
}

CpuStats CpuMonitor::Sample() {
  CpuStats s;
  s.name = name_;
  if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return s;

  if (auto u = PdhRead(totalUtil_)) s.usage = std::clamp(*u, 0.0, 100.0);
  if (auto p = PdhRead(totalPerf_); p && baseMhz_ > 0) s.freqGHz = baseMhz_ * *p / 100.0 / 1000.0;

  int index = 0;
  for (const auto& [inst, v] : PdhReadArray(coreUtil_)) {
    if (IsTotalInstance(inst)) continue;
    // Istanze "gruppo,core": il numero dopo la virgola è il core logico nel gruppo.
    const auto comma = inst.find(L',');
    const int core = comma != std::wstring::npos ? _wtoi(inst.c_str() + comma + 1) : index;
    const double u = std::clamp(v, 0.0, 100.0);
    s.coreUsage.emplace_back(core, u);
    if (!s.topCoreUsage || u > *s.topCoreUsage) {
      s.topCoreUsage = u;
      s.topCore = core;
    }
    ++index;
  }
  if (baseMhz_ > 0) {
    for (const auto& [inst, v] : PdhReadArray(corePerf_)) {
      if (IsTotalInstance(inst)) continue;
      const double ghz = baseMhz_ * v / 100.0 / 1000.0;
      const auto comma = inst.find(L',');
      s.coreGHz.emplace_back(comma != std::wstring::npos ? _wtoi(inst.c_str() + comma + 1) : int(s.coreGHz.size()), ghz);
      if (!s.maxFreqGHz || ghz > *s.maxFreqGHz) s.maxFreqGHz = ghz;
    }
  }
  std::sort(s.coreUsage.begin(), s.coreUsage.end());
  std::sort(s.coreGHz.begin(), s.coreGHz.end());
  return s;
}

}  // namespace po
