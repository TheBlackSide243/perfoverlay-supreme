#include "monitor/app.h"

#include "common/lhm.h"

#include <commctrl.h>

#include <algorithm>
#include <format>
#include <optional>
#include <shellapi.h>

#include "common/branding.h"
#include "common/darkmode.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/resources.h"
#include "common/util.h"
#include "common/winutil.h"
#include "monitor/overlay_content.h"
#include "monitor/rtss_text.h"

namespace po {
namespace {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT_PTR kTimerTick = 1;
constexpr ULONGLONG kToastMs = 2000;

enum HotkeyId { kHkToggle = 1, kHkCycle, kHkFpsOnly };
enum MenuId : UINT {
  kMenuToggle = 100,
  kMenuFpsOnly,
  kMenuSettings,
  kMenuTest,
  kMenuLog,
  kMenuExit,
  kMenuAuto = 200,
  kMenuProfileBase = 201,  // + indice profilo
};

}  // namespace

int App::Run(HINSTANCE inst) {
  inst_ = inst;
  EnableDarkModeForApp();  // menu della tray in tema scuro, come la UI impostazioni
  LoadAll();

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = &App::WndProc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  wc.lpszClassName = kMonitorWndClass;
  RegisterClassExW(&wc);
  // Finestra top-level mai mostrata: riceve tray, hotkey, timer e notifiche dalla UI impostazioni.
  wnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMonitorWndClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                         inst, this);
  if (!wnd_ || !overlay_.Create(inst)) {
    LogError("Inizializzazione finestre fallita");
    return 1;
  }
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  // PerfOverlay gira come amministratore: Windows (UIPI) bloccherebbe i messaggi inviati da
  // Explorer (tray) e dalla UI impostazioni, che girano senza privilegi. Li autorizziamo uno a uno.
  for (UINT m : {kMsgTray, kMsgReload, kMsgSuspendHotkeys, kMsgQuit, taskbarCreated_})
    ChangeWindowMessageFilterEx(wnd_, m, MSGFLT_ALLOW, nullptr);

  if (LhmInstalled() && !LhmRunning()) StartLhm();  // temperature CPU / scheda madre e ventole
  framesOk_ = frames_.Start();
  stats_.Start(cfg_.refreshMs);
  ApplyAutostart();
  RegisterHotkeys();
  AddTray();
  SetTimer(wnd_, kTimerTick, UINT(cfg_.refreshMs), nullptr);
  LogInfo("PerfOverlay Supreme avviato (refresh {} ms, FPS {})", cfg_.refreshMs, framesOk_ ? "attivi" : "non disponibili");

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  KillTimer(wnd_, kTimerTick);
  UnregisterHotkeys();
  RemoveTray();
  overlay_.Show(false);
  stats_.Stop();
  frames_.Stop();
  LogInfo("PerfOverlay Supreme chiuso");
  return 0;
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->wnd_ = hwnd;
  }
  auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      if (wp == kTimerTick) Tick();
      return 0;
    case WM_HOTKEY:
      if (wp == kHkToggle) ToggleVisibility();
      if (wp == kHkCycle) CycleProfile();
      if (wp == kHkFpsOnly) ToggleFpsOnly();
      return 0;
    case kMsgTray:
      if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
      if (LOWORD(lp) == WM_LBUTTONDBLCLK) OpenSettings();
      return 0;
    case kMsgQuit:
      LogInfo("Chiusura richiesta da un'altra applicazione");
      DestroyWindow(wnd_);
      return 0;
    case kMsgSuspendHotkeys:
      if (wp)
        UnregisterHotkeys();
      else
        RegisterHotkeys();
      return 0;
    case kMsgReload: {
      LogInfo("Ricarico configurazione e profili");
      const int oldRefresh = cfg_.refreshMs;
      UnregisterHotkeys();
      LoadAll();
      RegisterHotkeys();
      stats_.SetInterval(cfg_.refreshMs);
      if (cfg_.autostart != autostartApplied_) ApplyAutostart();
      if (oldRefresh != cfg_.refreshMs) SetTimer(wnd_, kTimerTick, UINT(cfg_.refreshMs), nullptr);
      lastProfile_.clear();
      Tick();
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      if (msg == taskbarCreated_ && taskbarCreated_) {  // Explorer riavviato: ricrea l'icona
        AddTray();
        return 0;
      }
      return DefWindowProcW(wnd_, msg, wp, lp);
  }
}

void App::LoadAll() {
  cfg_ = LoadConfig();
  games_ = LoadGames();
  profiles_ = LoadProfiles();
  rtssLoaded_ = nullptr;  // i profili sono nuovi: il motore RTSS va ricaricato
  if (!cfg_.forcedProfile.empty() && !FindProfile(profiles_, cfg_.forcedProfile)) {
    LogWarn("Profilo forzato '{}' inesistente, torno alla selezione automatica", cfg_.forcedProfile);
    cfg_.forcedProfile.clear();
  }
  LogInfo("Caricati {} profili, {} giochi noti, {} appresi", profiles_.size(), games_.known.size(),
          games_.learned.size());
}

void App::ApplyAutostart() {
  // Ricreata a ogni avvio: se la cartella di PerfOverlay è stata spostata, il percorso resta giusto.
  const bool ok = SetAutostart(cfg_.autostart, ExeDir() / kMonitorExe);
  autostartApplied_ = cfg_.autostart;
  LogInfo("Avvio con Windows {}{}", cfg_.autostart ? "attivo (attività pianificata)" : "disattivato",
          ok ? "" : " - ERRORE, vedi sopra");
}

void App::RegisterHotkeys() {
  if (hotkeysActive_) return;
  hotkeysActive_ = true;
  const std::pair<int, const std::string*> keys[] = {
      {kHkToggle, &cfg_.hotkeyToggle}, {kHkCycle, &cfg_.hotkeyCycleProfile}, {kHkFpsOnly, &cfg_.hotkeyFpsOnly}};
  for (const auto& [id, text] : keys) {
    if (text->empty()) continue;
    unsigned mods, vk;
    if (!ParseHotkey(*text, mods, vk)) {
      LogWarn("Hotkey non valida: '{}'", *text);
      continue;
    }
    if (!RegisterHotKey(wnd_, id, mods | MOD_NOREPEAT, vk))
      LogWarn("Hotkey '{}' già usata da un altro programma", *text);
  }
}

void App::UnregisterHotkeys() {
  hotkeysActive_ = false;
  for (int id : {kHkToggle, kHkCycle, kHkFpsOnly}) UnregisterHotKey(wnd_, id);
}

void App::Tick() {
  const ForegroundInfo fg = QueryForeground();
  stats_.SetWatchPid(fg.valid ? fg.pid : 0);
  const SystemSnapshot sys = stats_.Get();
  GameSignals sig;
  sig.presentRate = (fg.valid && framesOk_) ? frames_.PresentRate(fg.pid) : 0.0;
  if (sys.foreground.pid == fg.pid && sys.foreground.valid) {  // il campione GPU è della finestra attuale
    sig.gpuValid = true;
    sig.gpu3D = sys.foreground.usage3D;
    sig.vramMB = sys.foreground.vramMB;
  }
  bool learned = false;
  const bool isGame = detector_.IsGame(fg, sig, games_, cfg_.autoLearnGames, learned);
  if (learned) SaveGames(games_);

  if (isGame && fg.pid != lastGamePid_) {
    LogInfo("Gioco in primo piano: {} (pid {}, \"{}\", {}x{}{})", fg.exe.empty() ? "?" : fg.exe, fg.pid, fg.title,
            fg.client.right - fg.client.left, fg.client.bottom - fg.client.top, fg.fullscreen ? ", fullscreen" : "");
  } else if (!isGame && lastGamePid_) {
    LogInfo("Uscita dal gioco (pid {})", lastGamePid_);
  }
  lastGamePid_ = isGame ? fg.pid : 0;
  stats_.SetGamePid(lastGamePid_);

  enum class Mode { None, Game, Compact } mode = Mode::None;
  if (isGame)
    mode = Mode::Game;
  else if (cfg_.outOfGame == "compact")
    mode = Mode::Compact;
  if (hiddenByUser_) mode = Mode::None;
  if (mode == Mode::None) {
    overlay_.Show(false);
    return;
  }

  // Sul desktop: il profilo selezionato nelle impostazioni (o scelto dalla tray); in gioco: automatico/forzato.
  const Profile* desktop = isGame ? nullptr : FindProfile(profiles_, cfg_.desktopProfile);
  const Profile& profile = isGame    ? SelectProfile(profiles_, fg.exe, fg.title, cfg_.forcedProfile)
                           : desktop ? *desktop
                                     : SelectProfile(profiles_, "", "", cfg_.forcedProfile);
  if (profile.name != lastProfile_) {
    LogInfo("Profilo attivo: {}", profile.name);
    lastProfile_ = profile.name;
  }

  FrameStats fs;
  if (mode == Mode::Game && framesOk_) fs = frames_.Query(fg.pid, cfg_.graphSeconds);

  ContentInput in;
  in.profile = &profile;
  in.frames = mode == Mode::Game ? &fs : nullptr;
  in.system = &sys;
  in.fpsOnly = cfg_.fpsOnly && mode == Mode::Game;
  in.frameSourceAvailable = framesOk_;
  if (GetTickCount64() < toastUntil_) in.toast = toast_;

  RECT anchor = fg.valid ? (mode == Mode::Game ? fg.client : fg.work) : RECT{};
  if (!fg.valid || anchor.right <= anchor.left) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    anchor = mi.rcWork;
  }
  if (profile.layout == "rtss" && profile.rtss.advanced && !profile.rtss.layers.empty()) {
    // Preset avanzato (es. TroyMetrics): motore completo con formule, immagini e grafici.
    if (rtssLoaded_ != &profile.rtss) {
      rtss_.Load(profile.rtss);
      rtssLoaded_ = &profile.rtss;
      LogInfo("Preset RTSS avanzato: {} ({} layer, {} sorgenti)", profile.rtss.name, profile.rtss.layers.size(),
              profile.rtss.defs.size());
    }
    rtss_.Update(sys, in.frames);
    overlay_.RenderRtssAdvanced(rtss_, profile, anchor, BuildSensorRows(in));
    overlay_.Show(true);
    return;
  }
  if (profile.layout == "rtss" && !profile.rtss.layers.empty()) {
    // Preset importato da RTSS: stessi layer e posizioni, dati di PerfOverlay.
    std::vector<RtssDrawLayer> layers;
    layers.reserve(profile.rtss.layers.size());
    for (const auto& l : profile.rtss.layers)
      layers.push_back({ExpandRtssText(l.text, profile.rtss, sys, in.frames), l.x, l.y, l.extentX, l.extentY,
                        l.origin, l.size, l.color});
    overlay_.RenderRtss(layers, profile.rtss, profile, anchor, BuildSensorRows(in));
    overlay_.Show(true);
    return;
  }

  if (profile.layout == "free") {
    // Layout libero: ogni elemento è un blocco con la sua posizione sulla tela 1920x1080.
    std::vector<FreeBlock> blocks;
    int topX = 20, topY = kLayoutRefH;
    const double aspect = anchor.bottom > anchor.top
                              ? double(anchor.right - anchor.left) / double(anchor.bottom - anchor.top)
                              : 16.0 / 9.0;
    int canvasWidth = 1920;
    const auto& items = profile.LayoutFor(aspect, &canvasWidth);
    for (const auto& item : items) {
      Row row = BuildElementRow(item.element, in);
      if (row.empty()) continue;
      if (item.y < topY) {
        topX = item.x;
        topY = item.y;
      }
      blocks.push_back({std::move(row), item.x, item.y, item.scale / 100.0f});
    }
    if (!in.toast.empty() && !blocks.empty()) {  // messaggi temporanei sopra il blocco più in alto
      Row toast{Segment{Segment::Kind::Text, in.toast, profile.colors.text, 0.8f}};
      blocks.push_back({std::move(toast), topX, std::max(0, topY - 40), 1.0f});
    }
    if (blocks.empty()) {
      overlay_.Show(false);
      return;
    }
    overlay_.RenderFree(blocks, profile, anchor, canvasWidth);
    overlay_.Show(true);
    return;
  }

  const auto rows = BuildRows(in);
  if (rows.empty()) {  // tutti gli elementi disattivati
    overlay_.Show(false);
    return;
  }
  overlay_.Render(rows, profile, anchor);
  overlay_.Show(true);
}

void App::AddTray() {
  NOTIFYICONDATAW nid{sizeof(nid)};
  nid.hWnd = wnd_;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = kMsgTray;
  if (!trayIcon_) LoadIconMetric(inst_, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &trayIcon_);
  nid.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(nid.szTip, kAppName);
  Shell_NotifyIconW(NIM_ADD, &nid);
}

void App::RemoveTray() {
  NOTIFYICONDATAW nid{sizeof(nid)};
  nid.hWnd = wnd_;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void App::ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  HMENU prof = CreatePopupMenu();
  AppendMenuW(prof, MF_STRING | (cfg_.forcedProfile.empty() ? MF_CHECKED : 0), kMenuAuto, L"Automatico (per gioco)");
  AppendMenuW(prof, MF_SEPARATOR, 0, nullptr);
  for (size_t i = 0; i < profiles_.size() && i < 500; ++i) {
    const bool checked = IEquals(profiles_[i].name, cfg_.forcedProfile);
    AppendMenuW(prof, MF_STRING | (checked ? MF_CHECKED : 0), kMenuProfileBase + UINT(i),
                ToWide(profiles_[i].name).c_str());
  }
  AppendMenuW(menu, MF_STRING, kMenuToggle, L"Mostra/nascondi overlay");
  AppendMenuW(menu, MF_STRING | (cfg_.fpsOnly ? MF_CHECKED : 0), kMenuFpsOnly, L"Solo FPS");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(prof), L"Profilo");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuSettings, L"Impostazioni...");
  AppendMenuW(menu, MF_STRING, kMenuTest, L"Test overlay (DirectX 11/12)");
  AppendMenuW(menu, MF_STRING, kMenuLog, L"Apri log");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, L"Esci");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(wnd_);  // necessario perché il menu si chiuda cliccando altrove
  const UINT cmd = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr));
  DestroyMenu(menu);  // distrugge anche il sottomenu

  if (cmd == kMenuToggle) {
    ToggleVisibility();
  } else if (cmd == kMenuFpsOnly) {
    ToggleFpsOnly();
  } else if (cmd == kMenuSettings) {
    OpenSettings();
  } else if (cmd == kMenuTest) {
    const auto exe = ExeDir() / kMonitorExe;
    ShellExecuteW(nullptr, L"open", exe.c_str(), L"--test", nullptr, SW_SHOWNORMAL);
  } else if (cmd == kMenuLog) {
    ShellExecuteW(nullptr, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else if (cmd == kMenuExit) {
    DestroyWindow(wnd_);
  } else if (cmd == kMenuAuto) {
    ForceProfile("");
  } else if (cmd >= kMenuProfileBase && cmd < kMenuProfileBase + profiles_.size()) {
    ForceProfile(profiles_[cmd - kMenuProfileBase].name);
  }
}

void App::ToggleVisibility() {
  // Accende/spegne l'overlay sui giochi; non lo fa mai comparire fuori dai giochi.
  hiddenByUser_ = !hiddenByUser_;
  LogInfo("Overlay {} manualmente", hiddenByUser_ ? "disattivato" : "riattivato");
  Tick();
}

void App::ToggleFpsOnly() {
  cfg_.fpsOnly = !cfg_.fpsOnly;
  SaveConfig(cfg_);
  ShowToast(cfg_.fpsOnly ? L"Modalità solo FPS" : L"Modalità completa");
}

void App::CycleProfile() {
  // Automatico → profilo 1 → profilo 2 → ... → Automatico
  std::string next;
  if (cfg_.forcedProfile.empty()) {
    next = profiles_.front().name;
  } else {
    for (size_t i = 0; i < profiles_.size(); ++i)
      if (IEquals(profiles_[i].name, cfg_.forcedProfile)) next = i + 1 < profiles_.size() ? profiles_[i + 1].name : "";
  }
  ForceProfile(next);
}

void App::ForceProfile(const std::string& name) {
  cfg_.forcedProfile = name;
  if (!name.empty()) cfg_.desktopProfile = name;  // anche sul desktop si vede il profilo appena scelto
  SaveConfig(cfg_);
  LogInfo("Profilo forzato: {}", name.empty() ? "(automatico)" : name);
  ShowToast(name.empty() ? L"Profilo: automatico" : L"Profilo: " + ToWide(name));
}

void App::ShowToast(const std::wstring& text) {
  toast_ = text;
  toastUntil_ = GetTickCount64() + kToastMs;
  Tick();
}

void App::OpenSettings() {
  const auto exe = ExeDir() / kMonitorExe;
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), L"--settings", nullptr, SW_SHOWNORMAL)) <=
      32)
    LogWarn("Impossibile avviare {}", ToUtf8(exe.wstring()));
}

}  // namespace po
