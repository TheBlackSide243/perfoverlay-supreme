#include "monitor/app.h"

#include "common/lhm.h"

#include <commctrl.h>

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <thread>
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
#include "common/i18n.h"

namespace po {
namespace {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgUpdateReady = WM_APP + 10;  // lParam = UpdateInfo* scaricato e verificato, o nullptr
constexpr UINT_PTR kTimerTick = 1;
constexpr UINT_PTR kTimerUpdate = 2;
constexpr UINT kFirstUpdateCheckMs = 60 * 1000;          // un minuto dopo l'avvio
constexpr UINT kUpdateCheckEveryMs = 6 * 60 * 60 * 1000;  // poi ogni 6 ore
constexpr ULONGLONG kToastMs = 2000;
constexpr double kMinSessionSeconds = 60;    // partite più corte non lasciano un riepilogo
constexpr double kMaxBenchmarkSeconds = 3600;  // oltre un'ora il benchmark si ferma da solo
constexpr size_t kKeepSessions = 200;        // riepiloghi di partita conservati (i benchmark restano tutti)

enum HotkeyId { kHkToggle = 1, kHkCycle, kHkFpsOnly, kHkBenchmark };
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

int64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

// 75 → "1:15", 3720 → "1:02:00"
std::wstring ClockText(double seconds) {
  const int s = int(seconds);
  return s >= 3600 ? std::format(L"{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60)
                   : std::format(L"{}:{:02}", s / 60, s % 60);
}

// Nome mostrato nelle registrazioni: il titolo della finestra. Quello della scena di prova cambia a ogni
// secondo (ci scrive gli FPS), quindi lì si usa un nome fisso.
std::string RunTitle(const ForegroundInfo& fg) {
  return fg.windowClass == "PerfOverlayTestWindow" ? std::string("PerfOverlay Test") : fg.title;
}

// 5040 → "1 h 24 min"
std::wstring DurationText(double seconds) {
  const int m = std::max(1, int(seconds / 60.0 + 0.5));
  return m >= 60 ? std::format(L"{} h {} min", m / 60, m % 60) : std::format(L"{} min", m);
}

}  // namespace

int App::Run(HINSTANCE inst, bool justUpdated) {
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
  SetTimer(wnd_, kTimerUpdate, kFirstUpdateCheckMs, nullptr);
  LogInfo("PerfOverlay Supreme {} avviato (refresh {} ms, FPS {})", AppVersion(), cfg_.refreshMs,
          framesOk_ ? "attivi" : "non disponibili");
  if (justUpdated)
    ShowTrayBalloon(T(L"PerfOverlay Supreme aggiornato"),
                    TF(L"Ora è in uso la versione v{}.", ToWide(std::string(AppVersion()))));

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  KillTimer(wnd_, kTimerTick);
  KillTimer(wnd_, kTimerUpdate);
  if (bench_.Active()) StopBenchmark();  // quello che è stato registrato non va perso
  if (session_.Active()) EndSession(false);
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
      if (wp == kTimerTick) {
        Tick();
        if (pendingUpdate_) TryInstallUpdate();  // appena si esce dal gioco
      } else if (wp == kTimerUpdate) {
        SetTimer(wnd_, kTimerUpdate, kUpdateCheckEveryMs, nullptr);
        StartAutoUpdate();
      }
      return 0;
    case kMsgUpdateReady: {
      updateBusy_ = false;
      std::unique_ptr<UpdateInfo> u(reinterpret_cast<UpdateInfo*>(lp));
      if (u) {
        pendingUpdate_ = std::move(*u);
        TryInstallUpdate();
      }
      return 0;
    }
    case WM_HOTKEY:
      if (wp == kHkToggle) ToggleVisibility();
      if (wp == kHkCycle) CycleProfile();
      if (wp == kHkFpsOnly) ToggleFpsOnly();
      if (wp == kHkBenchmark) ToggleBenchmark();
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
      if (!cfg_.autoUpdate) pendingUpdate_.reset();
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
  SetLang(LangFromCode(cfg_.language));  // menu della tray e sensori nella lingua scelta
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
  const bool ok = SetAutostart(cfg_.autostart, ExePath());
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
  UpdateBenchmarkHotkey();
}

void App::UnregisterHotkeys() {
  hotkeysActive_ = false;
  for (int id : {kHkToggle, kHkCycle, kHkFpsOnly}) UnregisterHotKey(wnd_, id);
  UpdateBenchmarkHotkey();
}

// La hotkey del benchmark serve solo in gioco: fuori resta libera per gli altri programmi
// (Ctrl+Shift+B nei browser mostra la barra dei preferiti).
void App::UpdateBenchmarkHotkey() {
  const bool want = hotkeysActive_ && !cfg_.hotkeyBenchmark.empty() && (lastGamePid_ || bench_.Active());
  if (want == benchHotkeyOn_) return;
  benchHotkeyOn_ = want;
  if (!want) {
    UnregisterHotKey(wnd_, kHkBenchmark);
    return;
  }
  unsigned mods, vk;
  if (!ParseHotkey(cfg_.hotkeyBenchmark, mods, vk))
    LogWarn("Hotkey non valida: '{}'", cfg_.hotkeyBenchmark);
  else if (!RegisterHotKey(wnd_, kHkBenchmark, mods | MOD_NOREPEAT, vk))
    LogWarn("Hotkey '{}' già usata da un altro programma", cfg_.hotkeyBenchmark);
}

RunSummary App::MakeRunMeta(const char* kind, const ForegroundInfo& fg, const SystemSnapshot& sys) const {
  RunSummary m;
  m.kind = kind;
  m.game = fg.exe;
  m.title = RunTitle(fg);
  m.api = frames_.Api(fg.pid);
  m.time = LocalTimeString();
  m.gpuName = sys.gpu.name;
  m.cpuName = sys.cpu.name;
  m.width = fg.client.right - fg.client.left;
  m.height = fg.client.bottom - fg.client.top;
  return m;
}

// Passa ai registratori i frame nuovi del gioco in primo piano e una lettura dei sensori.
void App::UpdateRecorders(const ForegroundInfo& fg, bool isGame, const SystemSnapshot& sys) {
  const DWORD game = isGame && framesOk_ ? fg.pid : 0;
  SensorSample smp;
  if (sys.ready) {
    smp.cpu = sys.cpu.usage.value_or(-1);
    smp.gpu = sys.gpu.usage.value_or(-1);
    smp.cpuTemp = sys.cpuTempC.value_or(-1);
    smp.gpuTemp = sys.gpu.tempC.value_or(-1);
    smp.gpuPower = sys.gpu.powerW.value_or(-1);
    smp.ramGB = sys.ram.usedGB > 0 ? sys.ram.usedGB : -1;
    smp.vramGB = sys.gpu.vramUsedGB.value_or(-1);
  }
  const int w = fg.client.right - fg.client.left, h = fg.client.bottom - fg.client.top;

  if (bench_.Active()) {
    if (game != benchPid_) {  // il gioco ha perso il primo piano o si è chiuso
      StopBenchmark();
    } else {
      bench_.AddFrames(frames_.Since(benchPid_, bench_.LastTimestamp()));
      bench_.AddSample(smp);
      bench_.SetApi(frames_.Api(benchPid_));
      const double limit = cfg_.benchmarkSeconds > 0 ? double(cfg_.benchmarkSeconds) : kMaxBenchmarkSeconds;
      if (bench_.Seconds() >= limit) StopBenchmark();
    }
  }

  if (session_.Active() && (!cfg_.sessionLog || (game && game != sessionPid_))) EndSession(cfg_.sessionLog);
  if (game && !session_.Active() && cfg_.sessionLog) {
    RunRecorder::Options o;
    o.step = 10;
    o.maxFrameMs = 1000;  // pause e caricamenti non sono frame lenti
    session_.Start(o, MakeRunMeta("session", fg, sys), frames_.Frequency(), QpcNow());
    sessionPid_ = game;
    sessionProc_ = OpenProcess(SYNCHRONIZE, FALSE, game);
    sessionSeen_ = GetTickCount64();
  }
  if (session_.Active()) {
    if (game == sessionPid_) {
      session_.AddFrames(frames_.Since(sessionPid_, session_.LastTimestamp()));
      session_.AddSample(smp);
      session_.SetWindow(RunTitle(fg), w, h);
      session_.SetApi(frames_.Api(sessionPid_));  // all'inizio il gioco può non aver ancora presentato un frame
      sessionSeen_ = GetTickCount64();
    } else {
      session_.SkipTo(QpcNow());  // fuori dal gioco (Alt+Tab): la partita è in pausa
      const bool gone = sessionProc_ ? WaitForSingleObject(sessionProc_, 0) == WAIT_OBJECT_0
                                     : GetTickCount64() - sessionSeen_ > 60000;
      if (gone) EndSession(true);
    }
  }
  UpdateBenchmarkHotkey();
}

void App::ToggleBenchmark() {
  if (bench_.Active()) {
    StopBenchmark();
    return;
  }
  const ForegroundInfo fg = QueryForeground();
  if (!framesOk_ || !lastGamePid_ || !fg.valid || fg.pid != lastGamePid_) {
    ShowToast(T(L"Benchmark: nessun gioco in primo piano"));
    return;
  }
  RunRecorder::Options o;
  o.keepFrames = true;
  bench_.Start(o, MakeRunMeta("benchmark", fg, stats_.Get()), frames_.Frequency(), QpcNow());
  benchPid_ = fg.pid;
  LogInfo("Benchmark avviato su {} (pid {})", fg.exe.empty() ? "?" : fg.exe, fg.pid);
  if (!inTick_) Tick();
}

void App::StopBenchmark() {
  RunData run = bench_.Finish();
  benchPid_ = 0;
  if (run.s.seconds < 2 || run.s.frames < 10) {
    LogInfo("Benchmark troppo breve ({:.1f} s): non salvato", run.s.seconds);
    ShowToast(T(L"Benchmark troppo breve: non salvato"), 4000);
    return;
  }
  run.s.id = MakeRunId(run.s);
  if (!SaveRun(run)) {
    LogWarn("Benchmark non salvato: impossibile scrivere {}", ToUtf8(RunJsonPath(run.s.id).wstring()));
    ShowToast(T(L"Benchmark non salvato: vedi il log"), 4000);
    return;
  }
  LogInfo("Benchmark salvato: {} ({:.1f} s, {} frame, media {:.1f} FPS, 1% low {:.1f})", run.s.id, run.s.seconds,
          run.s.frames, run.s.fpsAvg, run.s.low1);
  ShowToast(TF(L"Benchmark salvato: media {} FPS, 1% low {}", int(run.s.fpsAvg + 0.5), int(run.s.low1 + 0.5)), 6000);
}

// Chiude la partita: salva il riepilogo e, se richiesto, lo mostra in una notifica.
void App::EndSession(bool notify) {
  RunData run = session_.Finish();
  if (sessionProc_) CloseHandle(sessionProc_);
  sessionProc_ = nullptr;
  sessionPid_ = 0;
  if (run.s.seconds < kMinSessionSeconds || run.s.frames < 100) return;
  run.s.id = MakeRunId(run.s);
  if (!SaveRun(run)) {
    LogWarn("Riepilogo della partita non salvato: impossibile scrivere {}", ToUtf8(RunJsonPath(run.s.id).wstring()));
    return;
  }
  PruneSessions(kKeepSessions);
  LogInfo("Partita finita: {} ({:.0f} s, media {:.1f} FPS, 1% low {:.1f})", run.s.id, run.s.seconds, run.s.fpsAvg,
          run.s.low1);
  if (!notify || !cfg_.sessionNotify) return;
  std::wstring name = ToWide(run.s.DisplayName());
  if (name.size() > 36) name = name.substr(0, 35) + L"…";
  std::wstring text = TF(L"Media {} FPS, 1% low {}.", int(run.s.fpsAvg + 0.5), int(run.s.low1 + 0.5));
  if (run.s.gpuTempMax > 0) text += L" " + TF(L"GPU fino a {} °C.", int(run.s.gpuTempMax + 0.5));
  if (run.s.cpuTempMax > 0) text += L" " + TF(L"CPU fino a {} °C.", int(run.s.cpuTempMax + 0.5));
  text += std::wstring(L" ") + T(L"I dettagli sono in Impostazioni › Benchmark.");
  ShowTrayBalloon(name + L": " + DurationText(run.s.seconds), text);
}

void App::Tick() {
  struct Guard {
    bool& flag;
    bool was;
    explicit Guard(bool& f) : flag(f), was(f) { flag = true; }
    ~Guard() { flag = was; }
  } guard(inTick_);
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
  UpdateRecorders(fg, isGame, sys);

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

  if (mode == Mode::Game) CheckExclusiveFullscreen(fg);

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
  if (GetTickCount64() < toastUntil_) {
    in.toast = toast_;
  } else if (bench_.Active()) {  // registrazione in corso: pallino rosso e tempo trascorso
    in.toast = L"● REC " + ClockText(bench_.Seconds());
    if (cfg_.benchmarkSeconds > 0) in.toast += L" / " + ClockText(cfg_.benchmarkSeconds);
    in.toastAlert = true;
  }

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
      Row toast{Segment{Segment::Kind::Text, in.toast, in.toastAlert ? Color{0xFF, 0x4A, 0x4A} : profile.colors.text,
                        0.8f, in.toastAlert}};
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
  AppendMenuW(prof, MF_STRING | (cfg_.forcedProfile.empty() ? MF_CHECKED : 0), kMenuAuto, T(L"Automatico (per gioco)"));
  AppendMenuW(prof, MF_SEPARATOR, 0, nullptr);
  for (size_t i = 0; i < profiles_.size() && i < 500; ++i) {
    const bool checked = IEquals(profiles_[i].name, cfg_.forcedProfile);
    AppendMenuW(prof, MF_STRING | (checked ? MF_CHECKED : 0), kMenuProfileBase + UINT(i),
                ToWide(profiles_[i].name).c_str());
  }
  AppendMenuW(menu, MF_STRING, kMenuToggle, T(L"Mostra/nascondi overlay"));
  AppendMenuW(menu, MF_STRING | (cfg_.fpsOnly ? MF_CHECKED : 0), kMenuFpsOnly, T(L"Solo FPS"));
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(prof), T(L"Profilo"));
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuSettings, T(L"Impostazioni..."));
  AppendMenuW(menu, MF_STRING, kMenuTest, L"Test overlay (DirectX 11/12)");
  AppendMenuW(menu, MF_STRING, kMenuLog, T(L"Apri log"));
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, T(L"Esci"));

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
    Launch(L"--test");
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
  ShowToast(cfg_.fpsOnly ? T(L"Modalità solo FPS") : T(L"Modalità completa"));
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
  ShowToast(name.empty() ? T(L"Profilo: automatico") : T(L"Profilo: ") + ToWide(name));
}

void App::ShowToast(const std::wstring& text, ULONGLONG ms) {
  toast_ = text;
  toastUntil_ = GetTickCount64() + (ms ? ms : kToastMs);
  if (!inTick_) Tick();  // subito sullo schermo; se arriva da dentro Tick lo mostra il giro in corso
}

// L'overlay è una finestra a parte: sopra un gioco in fullscreen esclusivo (D3D) Windows non la
// compone. Non tocchiamo la finestra del gioco (cambiarne lo stile da fuori può farlo crashare o
// lasciarlo a una risoluzione sbagliata): avvisiamo una volta per gioco di usare il borderless.
void App::CheckExclusiveFullscreen(const ForegroundInfo& fg) {
  if (!cfg_.fullscreenHint || !fg.valid || !fg.fullscreen) return;
  QUERY_USER_NOTIFICATION_STATE state{};
  if (FAILED(SHQueryUserNotificationState(&state)) || state != QUNS_RUNNING_D3D_FULL_SCREEN) return;
  const std::string key = ToLowerAscii(fg.exe.empty() ? std::to_string(fg.pid) : fg.exe);
  if (!fullscreenHinted_.insert(key).second) return;
  LogInfo("{} usa il fullscreen esclusivo: l'overlay potrebbe non essere visibile", fg.exe.empty() ? "?" : fg.exe);
  ShowTrayBalloon(T(L"Fullscreen esclusivo"),
                  (fg.exe.empty() ? std::wstring(T(L"Il gioco")) : ToWide(fg.exe)) +
                      T(L" usa il fullscreen esclusivo: l'overlay potrebbe non comparire. Nelle opzioni video del "
                        L"gioco scegli \"Finestra senza bordi\" (borderless)."));
}

void App::ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
  NOTIFYICONDATAW nid{sizeof(nid)};
  nid.hWnd = wnd_;
  nid.uID = 1;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::OpenSettings() { Launch(L"--settings"); }

// Rilancia questo stesso exe (impostazioni, prova DirectX) con il nome e il percorso che ha davvero.
void App::Launch(const wchar_t* args) {
  const auto exe = ExePath();
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args, nullptr, SW_SHOWNORMAL)) <= 32) {
    LogWarn("Impossibile avviare {} {} (errore {})", ToUtf8(exe.wstring()), ToUtf8(args), GetLastError());
    ShowToast(T(L"Impossibile aprire la finestra: vedi il log"));
  }
}

// Aggiornamento automatico: cerca l'ultima release su GitHub e, se è più nuova, ne scarica l'exe e lo
// verifica (SHA-256) in un thread a parte. L'installazione avviene poi sul thread della finestra.
void App::StartAutoUpdate() {
  if (!cfg_.checkUpdates || !cfg_.autoUpdate || updateBusy_ || updateFailed_ || pendingUpdate_) return;
  updateBusy_ = true;
  std::thread([wnd = wnd_] {
    std::wstring error;
    UpdateInfo* ready = nullptr;
    if (auto u = CheckForUpdate(error)) {
      LogInfo("Aggiornamento automatico: scarico la v{}", u->version);
      if (DownloadUpdate(*u, error))
        ready = new UpdateInfo(std::move(*u));
      else
        LogWarn("Aggiornamento automatico: download non riuscito ({})", ToUtf8(error));
    }
    if (!PostMessageW(wnd, kMsgUpdateReady, 0, reinterpret_cast<LPARAM>(ready))) delete ready;
  }).detach();
}

// Mai durante una partita: l'aggiornamento resta in attesa finché non c'è più un gioco in primo piano.
// L'exe nuovo prende il posto di quello in uso; l'overlay si chiude e RunMonitor (main.cpp) lo riavvia.
void App::TryInstallUpdate() {
  if (!pendingUpdate_ || lastGamePid_) return;
  const UpdateInfo u = std::move(*pendingUpdate_);
  pendingUpdate_.reset();
  std::wstring error;
  if (!InstallUpdate(error)) {
    updateFailed_ = true;
    LogWarn("Aggiornamento automatico alla v{} non riuscito: {}", u.version, ToUtf8(error));
    ShowTrayBalloon(T(L"Aggiornamento non riuscito"), error);
    return;
  }
  LogInfo("Aggiornamento automatico alla v{} installato: riavvio dell'overlay", u.version);
  restartAfterUpdate_ = true;
  DestroyWindow(wnd_);
}

}  // namespace po
