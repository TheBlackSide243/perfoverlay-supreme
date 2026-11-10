#pragma once
#include <windows.h>

#include <optional>
#include <string>

namespace po {

// Lettura della temperatura (e potenza) della CPU dal driver ADTReport (AMD e Intel moderni).
// ADTReport espone le temperature CPU che Windows non mostra senza un driver: si carica dinamicamente
// ADTReport.sys da %SystemRoot%\System32\drivers e apre \.\ADTReport. Se il driver non c'è, Init()
// restituisce false e la CPU torna alla temperatura di HWiNFO/LibreHardwareMonitor.
class AdtReport {
 public:
  ~AdtReport();
  bool Init();
  // Temperatura CPU in °C e potenza in W. Restituisce false se la lettura fallisce.
  bool Read(std::optional<double>& tempC, std::optional<double>& powerW);

 private:
  // Legge il registro di sistema (offset 0x200) come fa ADTReport.
  bool ReadSysReg(int reg, double& out);
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace po
