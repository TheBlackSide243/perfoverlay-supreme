#pragma once

namespace po {

class Wmi;

struct RamStats {
  double usedGB = 0;
  double totalGB = 0;
  int speedMTs = 0;  // 0 = sconosciuta
};

RamStats ReadRam();
int ReadRamSpeed(Wmi& cimv2);  // Win32_PhysicalMemory.ConfiguredClockSpeed (letta una volta)

}  // namespace po
