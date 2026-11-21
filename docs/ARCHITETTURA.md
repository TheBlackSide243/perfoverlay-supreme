# Architettura

## Nessuna iniezione nei giochi

PerfOverlay Supreme **non carica codice nei processi dei giochi** e non aggancia `Present` / `vkQueuePresentKHR`.

- **FPS / frame time**: una sessione ETW in tempo reale riceve gli eventi `Present` che Windows già pubblica, con lo stesso principio di PresentMon. I timestamp arrivano da QPC. Provider:

  | Provider | Evento | API coperte |
  |---|---|---|
  | `Microsoft-Windows-DXGI` | `Present_Start` | DirectX 10/11/12 |
  | `Microsoft-Windows-D3D9` | `Present_Start` | DirectX 9 |
  | `Microsoft-Windows-DxgKrnl` | `Present` | Vulkan / OpenGL e le altre API che non passano da DXGI |

- **Overlay**: una finestra separata, trasparente, *click-through*, sempre in primo piano, che non prende mai il focus ed è posizionata sopra l'area client del gioco. Usa D3D11 + DirectComposition (alpha per pixel) e Direct2D/DirectWrite per testo, grafici e sprite.
- **Conseguenza**: funziona con i giochi in finestra e borderless. Nel fullscreen esclusivo gli FPS vengono misurati, ma la finestra non è visibile: il monitor lo rileva con `SHQueryUserNotificationState` (`QUNS_RUNNING_D3D_FULL_SCREEN`) e mostra un avviso nella tray, senza toccare la finestra del gioco.

## Un solo eseguibile

`PerfOverlaySupreme.exe` ha un manifest `asInvoker` e sceglie la modalità dagli argomenti (`src/monitor/main.cpp`):

| Argomento | Modalità |
|---|---|
| *(nessuno)* | se l'overlay è già attivo apre le impostazioni. Se il processo è elevato fa da overlay; altrimenti si rilancia con `runas --monitor`. |
| `--monitor` | overlay (richiede amministratore per ETW) |
| `--settings` | impostazioni (utente normale) |
| `--test [dx11]` | scena di prova DirectX 12/11 |
| `--import-rtss "file" [profilo]` | importa un preset RTSS |

L'overlay (amministratore) pubblica la lista dei sensori in una shared memory `Local\PerfOverlaySupremeSensors`, con un seqlock e una ACL che consente la sola lettura agli utenti normali. Le impostazioni la leggono per il browser dei sensori. Dopo ogni salvataggio le impostazioni avvisano l'overlay con un messaggio alla sua finestra, e l'overlay ricarica config e profili.

## Struttura

```
src/
  CMakeLists.txt
  assets/                 icona
  config/                 games.example.json (formato della lista giochi)
  tools/
    overlay_test.cpp      scena di prova DX11/DX12 (--test)
    rtss_preview.cpp      anteprima di un preset RTSS con dati di esempio (target rtss-preview)
    close-monitor.ps1     chiude l'overlay prima di ricompilare
  src/common/             config, profili, export/import, preset RTSS, feed sensori, LHM, log, percorsi,
                          registrazioni (benchmark.*: RunRecorder, file JSON/CSV)
  src/monitor/
    main.cpp              dispatcher delle modalità
    app.*                 ciclo principale, tray, hotkey, selezione profilo, benchmark e riepilogo delle partite
    frame_source.*        FPS via ETW; legge anche V-Sync (payload di Present) e frame generati (Intel-PresentMon)
    bottleneck.*          che cosa limita gli FPS: GPU, CPU, limitatore o V-Sync (stima, con isteresi)
    game_detector.*       rilevamento gioco (lista + euristica + auto-apprendimento)
    overlay_window.*      renderer D3D11 + DirectComposition + Direct2D/DirectWrite
    overlay_content.*     impaginazione (barra / verticale / libero), soglie colore
    rtss_text.*           preset RTSS semplici
    rtss_engine.*         motore dell'OverlayEditor: sorgenti, formule, provider
    rtss_render.cpp       hypertext RTSS avanzato: <C> <S> <B> <I> <AI> <G> <P> <IF>
    stats.*               thread di raccolta metriche
    data/                 CPU (PDH), GPU (DXGI + contatori), NVML, RAM, batteria,
                          sensori (HWiNFO shared memory, LHM WMI/HTTP), sensori di sistema, WMI
  src/settings/
    settings_window.*     finestra principale delle impostazioni: barra laterale e una pagina alla volta (g.layoutPage, ShowPage);
                          ridimensionabile, Relayout() riscala i controlli dal disegno a 96 DPI
    color_wheel.*         ruota colori (anello tonalità + quadrato saturazione/luminosità) della sezione Tema
    overlay_preview.*     anteprima dell'overlay nella sezione Aspetto overlay: BuildRows con valori d'esempio e
                          DrawRowsPreview (monitor/overlay_window.cpp), cioè lo stesso disegno dell'overlay vero
    benchmark_page.*      sezione Benchmark: lista delle registrazioni, riquadri, grafico (GDI+) e confronto
    layout_editor.*       editor del layout libero e dei preset RTSS semplici
    sensor_browser.*      browser dei sensori
    games_editor.*        lista giochi (Steam / Epic / GOG / programmi aperti)
    gallery.*             store overlay (base + GitHub)
    updater.*             aggiornamento dall'app (release GitHub, SHA-256, sostituzione dell'exe)
    theme.*               tema scuro
```

## Lingua dell'interfaccia

I testi sono scritti in italiano direttamente nel codice e passano da `T()` / `TU()` / `TF()` (`src/common/i18n.*`). Con l'inglese attivo, queste funzioni restituiscono la traduzione della tabella `src/common/i18n_en.inc`, che usa come chiave il testo italiano esatto. Se una frase manca dalla tabella, resta in italiano.

La lingua è `language` in `config.json` (`"it"` / `"en"`, vuota = lingua di Windows):
- **Impostazioni**: la leggono all'avvio e si riaprono quando cambia.
- **Overlay**: la riapplica a ogni ricaricamento della configurazione.

## Versione e aggiornamenti

La versione si scrive solo in `project(... VERSION x.y.z)` di `CMakeLists.txt`: arriva al codice come `PO_VERSION` e alle risorse dell'exe (FileVersion / ProductVersion).

`src/settings/updater.*` legge `releases/latest` dall'API di GitHub (WinHTTP), scarica l'asset `PerfOverlaySupreme.exe` accanto all'exe (`<exe>.download`) e ne verifica il campo `digest` (SHA-256, BCrypt). Poi rinomina l'exe in uso in `<exe>.old` (un exe in esecuzione si può rinominare ma non cancellare) e mette il nuovo al suo posto. Il `.old` viene cancellato al successivo avvio. Le note mostrate nell'app sono il messaggio del tag, che il workflow usa come testo della release.

Aggiornamento automatico (`autoUpdate`, "Aggiornamenti: Automatici"): lo fa l'overlay (`App::StartAutoUpdate` in `monitor/app.cpp`), un minuto dopo l'avvio e poi ogni 6 ore. Controllo e download girano in un thread; l'exe verificato resta in `pendingUpdate_` finché non c'è un gioco in primo piano (`lastGamePid_ == 0`). Poi `InstallUpdate` lo mette al suo posto, l'overlay si chiude e `RunMonitor` (`monitor/main.cpp`), dopo aver liberato il mutex di istanza singola, avvia il nuovo exe con `--monitor --updated`: eredita i permessi di amministratore, quindi nessuna richiesta UAC. Con `--updated` il nuovo processo aspetta fino a 20 s il mutex e mostra la notifica "aggiornato alla vX".

## Sorgenti dei dati

| Dato | Sorgente | Fallback |
|---|---|---|
| FPS, frame time, P95/P99, 1% low, stutter | eventi ETW `Present` | `n/d` se ETW non è disponibile |
| V-Sync e tearing | campi `SyncInterval` e `Flags` dell'evento DXGI `Present_Start` | ignoti fuori da DXGI |
| Frame generati (AMD FSR e AFMF, Intel XeSS-FG) | evento `PresentFrameType_Info` del provider ETW Intel-PresentMon, emesso da driver e SDK | nessuna distinzione (es. NVIDIA DLSS-FG) |
| Che cosa limita gli FPS | `monitor/bottleneck.*`: utilizzo GPU, core più carico, regolarità e stabilità dei frame, V-Sync, frequenza del monitor | "sconosciuto" senza FPS |
| CPU utilizzo, per core, frequenza | PDH `% Processor Utility` / `% Processor Performance` | `% Processor Time` |
| CPU temperatura / potenza | HWiNFO → LibreHardwareMonitor (WMI o HTTP `127.0.0.1:8085`) | omessa |
| GPU NVIDIA (tutti i sensori) | NVML, caricata dinamicamente | contatori `GPU Engine` / DXGI |
| GPU utilizzo, VRAM (altre) | contatori `GPU Engine` / `GPU Adapter Memory` + DXGI | — |
| VRAM del gioco | contatore `GPU Process Memory` del processo del gioco | omessa |
| Temperatura SSD | `IOCTL_STORAGE_QUERY_PROPERTY` (StorageDeviceTemperatureProperty) | omessa |
| Dischi, rete, zone termiche ACPI | PDH | — |
| RAM | `GlobalMemoryStatusEx`, velocità da WMI | velocità omessa |
| Batteria | `GetSystemPowerStatus` | nascosta sui PC fissi |

## Preset RTSS avanzati

`rtss_engine` carica le sorgenti del preset (`<Source>`): nome, unità, formato, formula, provider e id.

- **Provider**: HAL, HwInfo, PresentMon e Afterburner vengono mappati sui dati di PerfOverlay.
- **Formule**:
  - operatori ternari, logici, di confronto e aritmetici;
  - funzioni `validate`, `if`, `min`, `max`, `abs`, `round`, `percentile`, `swavg`, `swmax`, `swmin`, `buf`.
- **Rendering**: `rtss_render.cpp` interpreta i tag hypertext dei layer:
  - colori, anche a soglie con sfumatura;
  - dimensioni relative al font master;
  - box, sprite dell'`EmbeddedImage` tinti con una ColorMatrix D2D, sprite animati e ruotati;
  - grafici e barre;
  - posizioni assolute e condizioni.
- **Scala**: la scala di RTSS è `k = zoom × scala UI × dimensione/15`.

## Benchmark e riepilogo delle partite

`RunRecorder` (`common/benchmark.*`) riceve i timestamp dei frame di un processo e una lettura dei sensori a ogni giro di `App::Tick`. I frame arrivano da `FrameSource::Since(pid, ultimo)`, che restituisce solo quelli nuovi.

- **Niente frame in memoria per le partite**: una partita può durare ore, quindi i percentili escono da un istogramma dei frame time (passo 0,05 ms, fino a 1 s). I benchmark tengono anche l'elenco completo dei frame, da cui i percentili sono esatti e che finisce nel CSV.
- **Benchmark** (`App::ToggleBenchmark`, hotkey registrata solo con un gioco in primo piano): si ferma con la hotkey, alla durata scelta, dopo un'ora o quando il gioco perde il primo piano.
- **Partita** (`App::UpdateRecorders`, `EndSession`): parte quando un gioco va in primo piano e finisce quando il suo processo termina. Con il gioco in secondo piano `SkipTo` scarta i frame presentati nel frattempo; i frame oltre 1 s (pause, caricamenti) non si contano. Sotto i 60 secondi non si salva niente.
- **Impostazioni**: `settings/benchmark_page.*` legge i file con `ListRuns` e carica i frame dal CSV solo per la registrazione mostrata.

## File di configurazione

Tutto si trova in `%APPDATA%\PerfOverlay Supreme\`:

| File | Contenuto |
|---|---|
| `config.json` | impostazioni globali (refresh, hotkey, fuori dal gioco, profilo del desktop, lingua, autostart, ...) |
| `games.json` | giochi noti, appresi, esclusi (formato in `src/config/games.example.json`) |
| `profiles\<Nome>.json` | un file per profilo; `default.json` è il fallback |
| `presets\` | preset RTSS importati, con immagini e font |
| `downloads\` | pacchetti scaricati dallo store |
| `benchmarks\<data>-b-<gioco>.json` | una registrazione: riepilogo e serie di FPS, CPU e GPU (`-b-` = benchmark, `-s-` = partita) |
| `benchmarks\<...>.csv` | solo benchmark: una riga per frame (`frame`, `time_s`, `frametime_ms`) |
| `overlay.log` | log di debug (ruota a 5 MB) |

LibreHardwareMonitor, se installato dalle impostazioni, si trova in `%LOCALAPPDATA%\PerfOverlay Supreme\LibreHardwareMonitor`.
