#include "common/sensor_feed.h"

#include <sddl.h>

#include <cmath>
#include <cstring>
#include <format>

#include <nlohmann/json.hpp>

#include "common/branding.h"
#include "common/util.h"

namespace po {
namespace {

constexpr DWORD kFeedSize = 4 * 1024 * 1024;
constexpr DWORD kFeedMagic = 0x53534F50;  // "POSS"

struct FeedHeader {
  DWORD magic;
  volatile LONG seq;  // dispari = scrittura in corso
  DWORD length;       // byte di JSON dopo l'intestazione
  DWORD reserved;
  ULONGLONG writtenTick;
};

std::wstring FeedName() { return std::wstring(L"Local\\") + kAppId + L"Sensors"; }

}  // namespace

std::wstring FormatSensorValue(double v, std::string_view unit, bool withUnit) {
  const double a = std::abs(v);
  int dec = a >= 100 ? 0 : a >= 10 ? 1 : 2;
  if (unit == "°C" || unit == "°F" || unit == "%" || unit == "MHz" || unit == "RPM" || unit == "MT/s" ||
      unit == "KB/s" || unit == "MB" || unit == "min" || unit == "mWh" || unit == "dBA" || unit.empty() && a >= 10)
    dec = 0;
  else if (unit == "W")
    dec = a >= 10 ? 0 : 1;
  else if (unit == "V")
    dec = 3;
  else if (unit == "A")
    dec = 2;
  else if (unit == "GB" || unit == "ms" || unit == "x" || unit == "GHz")
    dec = unit == "GHz" ? 2 : 1;
  if (a < 1e9 && std::abs(v - std::round(v)) < 1e-9 && unit != "V" && unit != "A") dec = 0;  // valori interi
  std::wstring s = std::format(L"{:.{}f}", v, dec);
  if (s == L"-0") s = L"0";
  if (withUnit && !unit.empty()) s += (unit == "%" ? L"" : L" ") + ToWide(unit);
  return s;
}

std::string SensorCategory(const SensorEntry& e) {
  const std::string& g = e.group;
  if (e.id.rfind("cpu:", 0) == 0 || IContains(g, "CPU") || IContains(g, "Ryzen") || IContains(g, "Intel Core"))
    return "cpu";
  if (e.id.rfind("nv:", 0) == 0 || e.id.rfind("gpu:", 0) == 0 || IContains(g, "GPU") || IContains(g, "GeForce") ||
      IContains(g, "Radeon"))
    return "gpu";
  if (e.id.rfind("ram:", 0) == 0 || IContains(g, "Memoria") || IContains(g, "Memory") || IContains(g, "DIMM"))
    return "ram";
  if (e.id.rfind("bat:", 0) == 0 || IContains(g, "Batter")) return "battery";
  return "";
}

// ------------------------------------------------------------------ pubblicazione (monitor)
SensorPublisher::~SensorPublisher() {
  if (view_) UnmapViewOfFile(view_);
  if (map_) CloseHandle(map_);
}

void SensorPublisher::Publish(const SensorList& sensors, const std::string& source) {
  if (failed_) return;
  if (!view_) {
    // Lettura per tutti (le impostazioni girano senza privilegi), scrittura solo per SYSTEM/amministratori;
    // etichetta di integrità bassa perché un processo non elevato possa aprirla.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)S:(ML;;NW;;;LW)",
                                                              SDDL_REVISION_1, &sd, nullptr)) {
      failed_ = true;
      return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, kFeedSize, FeedName().c_str());
    LocalFree(sd);
    if (map_) view_ = static_cast<BYTE*>(MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, kFeedSize));
    if (!view_) {
      if (map_) CloseHandle(map_);
      map_ = nullptr;
      failed_ = true;
      return;
    }
  }

  nlohmann::json j;
  j["source"] = source;
  auto& arr = j["sensors"] = nlohmann::json::array();
  for (const auto& s : sensors) {
    if (!std::isfinite(s.value)) continue;
    const double mn = std::isfinite(s.min) ? s.min : s.value, mx = std::isfinite(s.max) ? s.max : s.value;
    arr.push_back({s.id, s.group, s.name, s.unit, s.value, mn, mx});
  }
  std::string text;
  try {
    text = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  } catch (...) {
    return;
  }
  if (text.size() > kFeedSize - sizeof(FeedHeader)) return;

  auto* h = reinterpret_cast<FeedHeader*>(view_);
  InterlockedIncrement(&h->seq);
  h->magic = kFeedMagic;
  memcpy(view_ + sizeof(FeedHeader), text.data(), text.size());
  h->length = DWORD(text.size());
  h->writtenTick = GetTickCount64();
  InterlockedIncrement(&h->seq);
}

// ------------------------------------------------------------------ lettura (impostazioni)
std::optional<SensorFeed> ReadSensorFeed() {
  HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, FeedName().c_str());
  if (!map) return std::nullopt;
  const auto* view = static_cast<const BYTE*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
  std::optional<SensorFeed> out;
  if (view) {
    const auto* h = reinterpret_cast<const FeedHeader*>(view);
    std::string text;
    ULONGLONG tick = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
      const LONG before = h->seq;
      if (before & 1) {
        Sleep(1);
        continue;
      }
      MemoryBarrier();
      const DWORD len = h->length;
      if (h->magic != kFeedMagic || len > kFeedSize - sizeof(FeedHeader)) break;
      text.assign(reinterpret_cast<const char*>(view + sizeof(FeedHeader)), len);
      tick = h->writtenTick;
      MemoryBarrier();
      if (h->seq == before) break;
      text.clear();
    }
    if (!text.empty()) {
      try {
        const auto j = nlohmann::json::parse(text);
        SensorFeed f;
        f.source = j.value("source", "");
        for (const auto& a : j.at("sensors")) {
          SensorEntry e;
          e.id = a.at(0).get<std::string>();
          e.group = a.at(1).get<std::string>();
          e.name = a.at(2).get<std::string>();
          e.unit = a.at(3).get<std::string>();
          e.value = a.at(4).get<double>();
          e.min = a.at(5).get<double>();
          e.max = a.at(6).get<double>();
          f.sensors.push_back(std::move(e));
        }
        const ULONGLONG now = GetTickCount64();
        f.ageMs = now > tick ? now - tick : 0;
        out = std::move(f);
      } catch (...) {
      }
    }
    UnmapViewOfFile(view);
  }
  CloseHandle(map);
  return out;
}

}  // namespace po
