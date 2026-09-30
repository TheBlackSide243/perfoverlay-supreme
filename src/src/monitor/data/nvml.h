#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "common/sensor_feed.h"

namespace po {

// Lettura opzionale di temperatura e clock da NVIDIA NVML (nvml.dll installata col driver).
// Caricata dinamicamente: se manca, Init() restituisce false e non succede nulla.
class NvmlReader {
 public:
  ~NvmlReader();
  bool Init();
  bool Read(const std::string& adapterName, std::optional<double>& tempC, std::optional<double>& clockMHz,
            std::optional<double>& powerW);
  // Tutti i sensori delle GPU NVIDIA (temperatura, ventole, clock, consumo, limiti, utilizzo, VRAM, PCIe...).
  void ReadAll(SensorList& out);

 private:
  using Fn0 = int (*)();
  using FnCount = int (*)(unsigned*);
  using FnHandle = int (*)(unsigned, void**);
  using FnName = int (*)(void*, char*, unsigned);
  using FnTemp = int (*)(void*, int, unsigned*);
  using FnClock = int (*)(void*, int, unsigned*);

  HMODULE mod_ = nullptr;
  Fn0 shutdown_ = nullptr;
  FnTemp temp_ = nullptr;
  FnClock clock_ = nullptr;
  using FnPower = int (*)(void*, unsigned*);
  FnPower power_ = nullptr;
  FnPower powerLimit_ = nullptr;
  FnPower fanSpeed_ = nullptr;
  using FnCountDev = int (*)(void*, unsigned*);
  using FnFan2 = int (*)(void*, unsigned, unsigned*);
  FnCountDev numFans_ = nullptr;
  FnFan2 fanSpeed2_ = nullptr;
  using FnUtil = int (*)(void*, unsigned*);  // nvmlUtilization_t = {gpu, memory}
  FnUtil util_ = nullptr;
  using FnMem = int (*)(void*, unsigned long long*);  // nvmlMemory_t = {total, free, used}
  FnMem mem_ = nullptr;
  using FnPstate = int (*)(void*, int*);
  FnPstate pstate_ = nullptr;
  using FnPcie = int (*)(void*, int, unsigned*);
  FnPcie pcie_ = nullptr;
  using FnCodec = int (*)(void*, unsigned*, unsigned*);
  FnCodec enc_ = nullptr, dec_ = nullptr;
  struct Dev {
    void* handle;
    std::string name;
  };
  std::vector<Dev> devs_;
};

}  // namespace po
