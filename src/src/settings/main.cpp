#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>

#include "common/config.h"
#include "common/log.h"
#include "common/rtss_preset.h"
#include "common/util.h"
#include "common/winutil.h"
#include "settings/settings_window.h"


namespace {

// PerfOverlaySupreme-Settings.exe --import-rtss "preset.ovx" [profilo]
// Importa un preset RTSS nel profilo indicato (default: "default") senza aprire la finestra.
int ImportRtssFromCommandLine(const std::wstring& file, const std::string& profileName) {
  const auto r = po::ImportRtssPreset(file);
  if (!r.ok) {
    po::LogError("--import-rtss: {}", r.error);
    return 1;
  }
  auto profiles = po::LoadProfiles();
  po::Profile* target = nullptr;
  for (auto& p : profiles)
    if (po::IEquals(p.name, profileName)) target = &p;
  if (!target) {
    po::LogError("--import-rtss: profilo '{}' inesistente", profileName);
    return 1;
  }
  target->rtss = r.layout;
  target->layout = "rtss";
  if (!po::SaveProfile(*target)) return 1;
  po::NotifyMonitorReload();
  po::LogInfo("--import-rtss: {} applicato al profilo {}", r.layout.name, target->name);
  return 0;
}

}  // namespace

namespace po {

// Modalità impostazioni dell'exe unico: --settings, oppure --import-rtss "file" [profilo].
int RunSettingsMain(HINSTANCE inst, int show) {
  po::LogInit("settings");
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i + 1 < argc; ++i) {
    if (std::wstring(argv[i]) == L"--import-rtss") {
      const std::wstring file = argv[i + 1];
      const std::string profile = i + 2 < argc ? po::ToUtf8(argv[i + 2]) : "default";
      LocalFree(argv);
      return ImportRtssFromCommandLine(file, profile);
    }
  }
  if (argv) LocalFree(argv);

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  return po::RunSettingsWindow(inst, show);
}

}  // namespace po
