#include <windows.h>
#include <shellapi.h>

#include <string>

#include "common/branding.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/winutil.h"
#include "monitor/app.h"

// Common Controls v6: icona nitida nella tray (LoadIconMetric) e controlli delle impostazioni.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace po {
int RunSettingsMain(HINSTANCE inst, int show);
int RunOverlayTest(HINSTANCE inst, const wchar_t* cmd, int show);
}  // namespace po

namespace {

bool IsElevated() {
  HANDLE token = nullptr;
  TOKEN_ELEVATION e{};
  DWORD size = 0;
  const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                  GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
  if (token) CloseHandle(token);
  return ok;
}

bool HasArg(const wchar_t* cmd, const wchar_t* arg) { return cmd && wcsstr(cmd, arg) != nullptr; }

int RunMonitor(HINSTANCE inst) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // Istanza singola: una seconda esecuzione chiede solo di ricaricare la configurazione.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PerfOverlaySupreme.Monitor");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    po::NotifyMonitorReload();
    return 0;
  }
  po::LogInit("monitor");
  po::App app;
  const int rc = app.Run(inst);
  if (mutex) CloseHandle(mutex);
  return rc;
}

}  // namespace

// Un solo eseguibile:
//   (nessun argomento)  overlay; se è già in esecuzione apre le impostazioni. Chiede l'amministratore
//                       (serve per leggere gli FPS dagli eventi di Windows).
//   --settings          impostazioni          --import-rtss "file" [profilo]   importa un preset RTSS
//   --test [dx11]       finestra di prova DirectX 11/12 per vedere l'overlay
//   --monitor           overlay (usato dopo la richiesta di elevazione)
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
  if (HasArg(cmd, L"--test")) return po::RunOverlayTest(inst, cmd, show);
  if (HasArg(cmd, L"--settings") || HasArg(cmd, L"--import-rtss")) return po::RunSettingsMain(inst, show);
  if (HasArg(cmd, L"--monitor")) return RunMonitor(inst);

  // Doppio clic sull'exe: se l'overlay è già attivo apri le impostazioni, altrimenti avvialo.
  if (FindWindowW(po::kMonitorWndClass, nullptr)) return po::RunSettingsMain(inst, show);
  if (IsElevated()) return RunMonitor(inst);
  wchar_t self[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.lpVerb = L"runas";
  sei.lpFile = self;
  sei.lpParameters = L"--monitor";
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) {
    // Richiesta amministratore annullata: l'overlay non può leggere gli FPS; apri le impostazioni.
    return po::RunSettingsMain(inst, show);
  }
  return 0;
}
