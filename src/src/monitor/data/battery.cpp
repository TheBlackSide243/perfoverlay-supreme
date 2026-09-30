#include "monitor/data/battery.h"

#include <windows.h>
#include <powerbase.h>

#include <cstdlib>

namespace po {

BatteryStats ReadBattery() {
  BatteryStats b;
  SYSTEM_POWER_STATUS ps;
  if (!GetSystemPowerStatus(&ps)) return b;
  // 128 = nessuna batteria, 255 = stato sconosciuto
  if (ps.BatteryFlag == 255 || (ps.BatteryFlag & 128)) return b;
  b.present = true;
  b.percent = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : -1;
  b.charging = ps.ACLineStatus == 1 || (ps.BatteryFlag & 8);
  if (ps.BatteryLifeTime != DWORD(-1)) b.remainMin = ps.BatteryLifeTime / 60.0;
  SYSTEM_BATTERY_STATE st{};
  if (CallNtPowerInformation(SystemBatteryState, nullptr, 0, &st, sizeof(st)) == 0 && st.BatteryPresent) {
    // Rate: mW, negativo in scarica. 0x80000000 = sconosciuto.
    if (st.Rate != 0x80000000) b.rateW = std::abs(static_cast<LONG>(st.Rate)) / 1000.0;
    if (!b.remainMin && st.EstimatedTime != DWORD(-1) && st.Discharging) b.remainMin = st.EstimatedTime / 60.0;
  }
  return b;
}

}  // namespace po
