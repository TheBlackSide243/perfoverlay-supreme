#include "monitor/data/memory.h"

#include <windows.h>

#include <algorithm>

#include "monitor/data/wmi.h"

namespace po {

RamStats ReadRam() {
  RamStats r;
  MEMORYSTATUSEX ms{sizeof(ms)};
  if (GlobalMemoryStatusEx(&ms)) {
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;
    r.totalGB = ms.ullTotalPhys / kGB;
    r.usedGB = (ms.ullTotalPhys - ms.ullAvailPhys) / kGB;
  }
  return r;
}

int ReadRamSpeed(Wmi& cimv2) {
  int speed = 0;
  for (auto& row : cimv2.Query(L"SELECT ConfiguredClockSpeed, Speed FROM Win32_PhysicalMemory",
                               {L"ConfiguredClockSpeed", L"Speed"})) {
    const auto& c = row[L"ConfiguredClockSpeed"];
    const auto& s = row[L"Speed"];
    const int v = c.isNum && c.num > 0 ? int(c.num) : (s.isNum ? int(s.num) : 0);
    speed = std::max(speed, v);
  }
  return speed;
}

}  // namespace po
