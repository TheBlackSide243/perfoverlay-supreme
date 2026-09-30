#pragma once
#include <windows.h>
#include <pdh.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace po {

struct CpuStats {
  std::string name;
  std::optional<double> usage;        // % totale
  std::optional<double> topCoreUsage; // % del core più carico ("core principale")
  int topCore = -1;
  std::optional<double> freqGHz;      // frequenza effettiva media
  std::optional<double> maxFreqGHz;   // frequenza del core più veloce
  std::vector<std::pair<int, double>> coreUsage;  // (core logico, %) per la finestra dei sensori
  std::vector<std::pair<int, double>> coreGHz;    // (core logico, GHz)
};

// Performance Counters: "% Processor Utility" e "% Processor Performance" × clock base.
class CpuMonitor {
 public:
  ~CpuMonitor();
  bool Init();
  CpuStats Sample();

 private:
  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER totalUtil_ = nullptr, totalPerf_ = nullptr, coreUtil_ = nullptr, corePerf_ = nullptr;
  double baseMhz_ = 0;
  std::string name_;
};

}  // namespace po
