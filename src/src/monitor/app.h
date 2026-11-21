#pragma once
#include <windows.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/benchmark.h"
#include "common/config.h"
#include "settings/updater.h"
#include "monitor/bottleneck.h"
#include "monitor/frame_source.h"
#include "monitor/game_detector.h"
#include "monitor/overlay_window.h"
#include "monitor/rtss_engine.h"
#include "monitor/stats.h"

namespace po {

class App {
 public:
  int Run(HINSTANCE inst, bool justUpdated = false);  // justUpdated: avviato dopo un aggiornamento automatico
  bool RestartAfterUpdate() const { return restartAfterUpdate_; }  // exe sostituito: va riavviato

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

  void LoadAll();
  void ApplyAutostart();
  void RegisterHotkeys();
  void UnregisterHotkeys();
  void Tick();
  void AddTray();
  void RemoveTray();
  void ShowTrayMenu();
  void ToggleVisibility();
  void ToggleFpsOnly();
  void CycleProfile();
  void ForceProfile(const std::string& name);
  void ShowToast(const std::wstring& text, ULONGLONG ms = 0);  // ms = 0: durata normale
  void UpdateRecorders(const ForegroundInfo& fg, bool isGame, const SystemSnapshot& sys, const FrameStats& fs,
                       Limit limit);
  double RefreshHz(HWND game);  // frequenza del monitor su cui sta il gioco (riletta ogni pochi secondi)
  void UpdateBenchmarkHotkey();  // la hotkey del benchmark è attiva solo con un gioco in primo piano
  void ToggleBenchmark();
  void StopBenchmark();
  void EndSession(bool notify);
  RunSummary MakeRunMeta(const char* kind, const ForegroundInfo& fg, const SystemSnapshot& sys) const;
  void CheckExclusiveFullscreen(const ForegroundInfo& fg);
  void ShowTrayBalloon(const std::wstring& title, const std::wstring& text);
  void OpenSettings();
  void Launch(const wchar_t* args);
  void StartAutoUpdate();   // controllo e download in background
  void TryInstallUpdate();  // installa l'aggiornamento scaricato e riavvia l'overlay, se non c'è un gioco

  HINSTANCE inst_ = nullptr;
  HWND wnd_ = nullptr;
  UINT taskbarCreated_ = 0;
  HICON trayIcon_ = nullptr;
  bool hotkeysActive_ = false;
  bool autostartApplied_ = false;

  GlobalConfig cfg_;
  GameList games_;
  std::vector<Profile> profiles_;

  FrameSource frames_;
  bool framesOk_ = false;
  StatsCollector stats_;
  OverlayWindow overlay_;
  RtssEngine rtss_;                    // preset RTSS avanzati (formule, immagini, grafici)
  const RtssLayout* rtssLoaded_ = nullptr;
  GameDetector detector_;

  bool hiddenByUser_ = false;  // hotkey/menu mostra-nascondi
  std::wstring toast_;
  ULONGLONG toastUntil_ = 0;
  DWORD lastGamePid_ = 0;
  std::set<std::string> fullscreenHinted_;  // giochi già avvisati del fullscreen esclusivo (per sessione)
  std::string lastProfile_;
  bool inTick_ = false;

  LimitDetector limit_;           // che cosa limita gli FPS del gioco in primo piano
  Limit loggedLimit_ = Limit::Unknown;
  ULONGLONG limitLoggedAt_ = 0;
  std::string loggedFrameGen_;
  double refreshHz_ = 0;
  ULONGLONG refreshReadAt_ = 0;

  RunRecorder bench_;             // benchmark avviato con la hotkey
  DWORD benchPid_ = 0;
  bool benchHotkeyOn_ = false;
  RunRecorder session_;           // riepilogo della partita in corso
  DWORD sessionPid_ = 0;
  HANDLE sessionProc_ = nullptr;  // per accorgersi che il gioco si è chiuso
  ULONGLONG sessionSeen_ = 0;     // ultima volta in cui il gioco era in primo piano

  bool updateBusy_ = false;              // controllo/download in corso
  bool updateFailed_ = false;            // installazione impossibile (cartella protetta): non si riprova
  bool restartAfterUpdate_ = false;
  std::optional<UpdateInfo> pendingUpdate_;  // scaricato e verificato, in attesa che non ci sia un gioco
};

}  // namespace po
