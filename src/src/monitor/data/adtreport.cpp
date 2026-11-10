#include "monitor/data/adtreport.h"

#include <cstdint>
#include <vector>

#include "common/log.h"
#include "common/util.h"

namespace po {
namespace {

// ADTReport espone la temperatura (e, su AMD, la potenza) della CPU tramite un registro di memoria
// mappato in spazio utente (\.\ADTReport), senza bisogno di HWiNFO/LibreHardwareMonitor.
// Il driver scrive un buffer con firma "ADTR" seguita da una serie di valori a 32 bit: le temperature
// sono in centiKelvin (valore / 100 − 273,15 = °C), le potenze in centiWatt. Scansiamo il buffer in
// ricerca dei valori plausibili così non dipendiamo da offset fissi che cambiano tra le versioni.

constexpr char kMagic[4] = {'A', 'D', 'T', 'R'};

bool IsTempC(int32_t v) {
  const double c = double(v) / 100.0 - 273.15;
  return c >= 20.0 && c <= 110.0;
}
bool IsPowerW(int32_t v) {
  const double w = double(v) / 100.0;
  return w >= 1.0 && w <= 250.0;
}

}  // namespace

AdtReport::~AdtReport() {
  if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
}

bool AdtReport::Init() {
  // Il driver si installa da %SystemRoot%\System32\drivers\ADTReport.sys. Non lo carichiamo:
  // l'utente lo avvia (es. tramite LibreHardwareMonitor); noi apriamo solo il device \.\ADTReport.
  h_ = CreateFileW(L"\.\ADTReport", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h_ == INVALID_HANDLE_VALUE) {
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH)) {
      const std::wstring path = std::wstring(sys) + L"\drivers\ADTReport.sys";
      if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Il driver non parte da solo: proviamo ad avviarlo (richiede privilegi).
        std::wstring svc = L"sc create ADTReport binPath= \"" + path + L"\" type= kernel";
        std::ignore = _wsystem(svc.c_str());
        std::ignore = _wsystem(L"sc start ADTReport");
        h_ = CreateFileW(L"\.\ADTReport", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
      }
    }
  }
  if (h_ == INVALID_HANDLE_VALUE) return false;

  std::vector<char> buf(8192);
  DWORD read = 0;
  if (!ReadFile(h_, buf.data(), UINT(buf.size()), &read, nullptr) || read < 16) return false;

  // Cerchiamo la firma "ADTR" nel buffer (può essere all'inizio o dopo un offset).
  const char* p = buf.data();
  const char* end = p + read - 4;
  const char* magic = nullptr;
  for (; p < end; ++p)
    if (std::memcmp(p, kMagic, 4) == 0) {
      magic = p;
      break;
    }
  if (!magic) {
    LogWarn("ADTReport: firma 'ADTR' non trovata");
    return false;
  }
  LogInfo("ADTReport: temperatura CPU disponibile");
  return true;
}

bool AdtReport::Read(std::optional<double>& tempC, std::optional<double>& powerW) {
  if (h_ == INVALID_HANDLE_VALUE) return false;

  std::vector<char> buf(8192);
  DWORD read = 0;
  if (!ReadFile(h_, buf.data(), UINT(buf.size()), &read, nullptr) || read < 16) return false;

  const char* p = buf.data();
  const char* end = p + read - sizeof(int32_t);
  bool foundTemp = false, foundPower = false;
  // Temperatura CPU = primo valore plausibile (centiKelvin); potenza = primo dopo la temperatura.
  for (; p < end; p += sizeof(int32_t)) {
    const int32_t v = *reinterpret_cast<const int32_t*>(p);
    if (!foundTemp && IsTempC(v)) {
      *tempC = double(v) / 100.0 - 273.15;
      foundTemp = true;
    } else if (foundTemp && !foundPower && IsPowerW(v)) {
      *powerW = double(v) / 100.0;
      foundPower = true;
    }
    if (foundTemp && foundPower) break;
  }

  if (!foundTemp && !foundPower) return false;
  if (tempC && *tempC <= 20.0) *tempC = 0.0;  // valore residuo non valido
  return true;
}

}  // namespace po
