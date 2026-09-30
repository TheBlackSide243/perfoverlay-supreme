#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "common/config.h"
#include "monitor/frame_source.h"
#include "monitor/game_detector.h"
#include "monitor/overlay_window.h"
#include "monitor/rtss_engine.h"
#include "monitor/stats.h"

namespace po {

class App {
 public:
  int Run(HINSTANCE inst);

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
  void ShowToast(const std::wstring& text);
  void OpenSettings();

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
  std::string lastProfile_;
};

}  // namespace po
