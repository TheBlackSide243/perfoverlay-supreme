#pragma once

#include <optional>

namespace po {

struct BatteryStats {
  bool present = false;  // false su desktop: l'elemento non viene mostrato
  int percent = -1;
  bool charging = false;
  std::optional<double> rateW;       // potenza assorbita/erogata (valore assoluto)
  std::optional<double> remainMin;   // autonomia stimata
};

BatteryStats ReadBattery();

}  // namespace po
