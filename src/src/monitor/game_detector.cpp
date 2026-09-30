#include "monitor/game_detector.h"

#include <algorithm>

#include <psapi.h>

#include "common/log.h"
#include "common/util.h"

namespace po {
namespace {

constexpr double kMinGameFps = 30.0;
constexpr double kMinGpu3D = 20.0;        // % motore 3D: sotto questa soglia il processo non sta "giocando"
constexpr double kHighGpu3D = 40.0;
constexpr double kMinVramMB = 400.0;
constexpr double kMinRamMB = 800.0;
constexpr ULONGLONG kLearnDelayMs = 6000;  // l'euristica deve reggere per 6 s prima di aggiungere il gioco
constexpr ULONGLONG kDipToleranceMs = 2000;  // brevi cali (caricamenti, menu) non azzerano il conteggio
constexpr int kLearnThreshold = 5;

bool IsShellClass(std::string_view cls) {
  return cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd" || cls == "Shell_SecondaryTrayWnd" ||
         cls == "Windows.UI.Core.CoreWindow";
}

const std::string& WindowsDirLower() {
  static const std::string dir = [] {
    wchar_t buf[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    return ToLowerAscii(ToUtf8(std::wstring_view(buf, n))) + "\\";
  }();
  return dir;
}

}  // namespace

ForegroundInfo QueryForeground() {
  ForegroundInfo fg;
  fg.hwnd = GetForegroundWindow();
  if (!fg.hwnd) return fg;
  GetWindowThreadProcessId(fg.hwnd, &fg.pid);
  if (!fg.pid || fg.pid == GetCurrentProcessId()) return fg;

  wchar_t buf[512];
  const int tn = GetWindowTextW(fg.hwnd, buf, int(std::size(buf)));
  fg.title = ToUtf8(std::wstring_view(buf, size_t(std::max(tn, 0))));
  const int cn = GetClassNameW(fg.hwnd, buf, int(std::size(buf)));
  fg.windowClass = ToUtf8(std::wstring_view(buf, size_t(std::max(cn, 0))));

  // Alcuni processi protetti (anti-cheat) negano anche QUERY_LIMITED_INFORMATION:
  // in quel caso resta solo il titolo finestra (voci "*" + window nella lista giochi).
  if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fg.pid)) {
    wchar_t path[MAX_PATH * 2];
    DWORD len = DWORD(std::size(path));
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
      fg.path = ToLowerAscii(ToUtf8(std::wstring_view(path, len)));
      const auto slash = fg.path.find_last_of('\\');
      fg.exe = slash == std::string::npos ? fg.path : fg.path.substr(slash + 1);
    }
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    if (GetProcessMemoryInfo(proc, &pmc, sizeof(pmc))) fg.ramMB = pmc.WorkingSetSize / (1024.0 * 1024.0);
    CloseHandle(proc);
  }

  fg.minimized = IsIconic(fg.hwnd) != FALSE;
  GetClientRect(fg.hwnd, &fg.client);
  POINT tl{fg.client.left, fg.client.top}, br{fg.client.right, fg.client.bottom};
  ClientToScreen(fg.hwnd, &tl);
  ClientToScreen(fg.hwnd, &br);
  fg.client = {tl.x, tl.y, br.x, br.y};

  MONITORINFO mi{sizeof(mi)};
  if (GetMonitorInfoW(MonitorFromWindow(fg.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    fg.monitor = mi.rcMonitor;
    fg.work = mi.rcWork;
    RECT wr;
    GetWindowRect(fg.hwnd, &wr);
    fg.fullscreen = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
                    wr.bottom >= mi.rcMonitor.bottom;
  }
  fg.valid = true;
  return fg;
}

int GameDetector::HeuristicScore(const ForegroundInfo& fg, const GameSignals& sig) const {
  const bool rendering = sig.presentRate >= kMinGameFps || (sig.gpuValid && sig.gpu3D >= kMinGpu3D);
  if (!rendering) return 0;                               // requisito: sta renderizzando in tempo reale
  if (fg.path.rfind(WindowsDirLower(), 0) == 0) return 0;  // componenti di sistema
  int score = 2;
  if (sig.gpu3D >= kHighGpu3D) score += 1;
  if (sig.vramMB >= kMinVramMB) score += 1;
  if (fg.ramMB >= kMinRamMB) score += 1;
  if (fg.fullscreen) score += 2;
  static const char* kGamePaths[] = {"\\steamapps\\common\\", "\\epic games\\", "\\gog galaxy\\games\\",
                                     "\\gog games\\",        "\\xboxgames\\",   "\\riot games\\",
                                     "\\ubisoft game launcher\\games\\", "\\ea games\\", "\\battle.net\\"};
  for (const char* p : kGamePaths)
    if (fg.path.find(p) != std::string::npos) {
      score += 2;
      break;
    }
  // Nomi tipici degli eseguibili generati da Unreal / build a 64 bit.
  if (fg.exe.find("-win64-shipping") != std::string::npos || fg.path.find("\\binaries\\win64\\") != std::string::npos)
    score += 2;
  return score;
}

bool GameDetector::IsGame(const ForegroundInfo& fg, const GameSignals& sig, GameList& games, bool autoLearn,
                          bool& listChanged) {
  listChanged = false;
  if (!fg.valid || fg.minimized || IsShellClass(fg.windowClass)) {
    candidatePid_ = 0;
    return false;
  }
  if (!fg.exe.empty() && std::find(games.exclude.begin(), games.exclude.end(), fg.exe) != games.exclude.end())
    return false;

  // La finestra di prova DirectX inclusa (PerfOverlayTest.exe) è sempre un gioco.
  if (fg.exe == "perfoverlaytest.exe" || fg.windowClass == "PerfOverlayTestWindow") return true;

  for (const auto* list : {&games.known, &games.learned})
    for (const auto& e : *list)
      if (MatchesEntry(e, fg.exe, fg.title)) return true;

  if (!autoLearn || fg.exe.empty()) return false;
  const ULONGLONG now = GetTickCount64();
  if (HeuristicScore(fg, sig) < kLearnThreshold) {
    if (candidatePid_ == fg.pid && now - candidateLastOk_ <= kDipToleranceMs) return false;
    candidatePid_ = 0;
    return false;
  }
  candidateLastOk_ = now;
  if (candidatePid_ != fg.pid) {
    candidatePid_ = fg.pid;
    candidateSince_ = now;
    return false;
  }
  if (now - candidateSince_ < kLearnDelayMs) return false;

  games.learned.push_back({fg.exe, ""});
  listChanged = true;
  candidatePid_ = 0;
  LogInfo("Nuovo gioco appreso: {} (fps {:.0f}, GPU 3D {:.0f}%, VRAM {:.0f} MB, RAM {:.0f} MB, fullscreen={}) — {}",
          fg.exe, sig.presentRate, sig.gpu3D, sig.vramMB, fg.ramMB, fg.fullscreen, fg.path);
  return true;
}

}  // namespace po
