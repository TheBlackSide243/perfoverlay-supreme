#include "common/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/branding.h"

namespace po {
namespace fs = std::filesystem;

namespace {

// Primo avvio: copia config, lista giochi e profili da PerfOverlay (se presente), con
// l'avvio automatico spento, per non far partire entrambe le app all'accensione.
void ImportLegacyData(const fs::path& legacy, const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(legacy / L"config.json", ec)) return;
  fs::copy(legacy / L"games.json", dir / L"games.json", fs::copy_options::skip_existing, ec);
  fs::copy(legacy / L"profiles", dir / L"profiles", fs::copy_options::recursive | fs::copy_options::skip_existing,
           ec);
  std::ifstream in(legacy / L"config.json", std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  auto j = nlohmann::json::parse(ss.str(), nullptr, false, true);
  if (j.is_object()) {
    j["autostart"] = false;
    std::ofstream(dir / L"config.json", std::ios::binary) << j.dump(2);
  }
}

}  // namespace

fs::path AppDataDir() {
  static const fs::path dir = [] {
    fs::path base;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) base = p;
    CoTaskMemFree(p);
    if (base.empty()) base = fs::temp_directory_path();
    auto d = base / kAppDataDirName;
    std::error_code ec;
    const bool firstRun = !fs::exists(d, ec);
    fs::create_directories(d / L"profiles", ec);
    if (firstRun) ImportLegacyData(base / kLegacyAppDataDirName, d);
    return d;
  }();
  return dir;
}

fs::path ProfilesDir() { return AppDataDir() / L"profiles"; }
fs::path ConfigPath() { return AppDataDir() / L"config.json"; }
fs::path GamesPath() { return AppDataDir() / L"games.json"; }
fs::path LogPath() { return AppDataDir() / L"overlay.log"; }
fs::path PresetsDir() {
  std::error_code ec;
  fs::create_directories(AppDataDir() / L"presets" / L"fonts", ec);
  return AppDataDir() / L"presets";
}

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(std::wstring(buf, n)).parent_path();
}

}  // namespace po
