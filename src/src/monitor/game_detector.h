#pragma once
#include <windows.h>

#include <string>

#include "common/config.h"

namespace po {

struct ForegroundInfo {
  bool valid = false;
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::string exe;    // minuscolo, es. "cyberpunk2077.exe"; vuoto se il processo non è interrogabile
  std::string path;   // minuscolo
  std::string title;
  std::string windowClass;
  RECT client{};      // area client in coordinate schermo
  RECT monitor{};     // monitor che contiene la finestra
  RECT work{};        // area di lavoro di quel monitor
  bool minimized = false;
  bool fullscreen = false;  // la finestra copre l'intero monitor (fullscreen o borderless)
  double ramMB = 0;         // working set del processo
};

ForegroundInfo QueryForeground();

// Segnali di "rendering in tempo reale" del processo in primo piano.
struct GameSignals {
  double presentRate = 0;  // FPS da ETW (0 se non disponibili)
  bool gpuValid = false;   // contatori GPU per processo disponibili
  double gpu3D = 0;        // % motore 3D usato dal processo
  double vramMB = 0;       // VRAM dedicata del processo
};

// Decide se la finestra in primo piano è un gioco:
// esclusioni → lista giochi noti/appresi → euristica (auto-apprendimento).
// L'euristica richiede che il processo stia davvero renderizzando (FPS ≥ 30 oppure GPU 3D ≥ 20%)
// e poi pesa VRAM, RAM, schermo intero e cartella di installazione.
class GameDetector {
 public:
  bool IsGame(const ForegroundInfo& fg, const GameSignals& sig, GameList& games, bool autoLearn,
              bool& listChanged);

 private:
  int HeuristicScore(const ForegroundInfo& fg, const GameSignals& sig) const;
  DWORD candidatePid_ = 0;
  ULONGLONG candidateSince_ = 0;
  ULONGLONG candidateLastOk_ = 0;
};

}  // namespace po
