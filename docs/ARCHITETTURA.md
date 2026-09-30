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
- **Conseguenza**: funziona con i giochi in finestra e borderless. Nel fullscreen esclusivo gli FPS vengono misurati, ma la finestra non è visibile.

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
  src/common/             config, profili, export/import, preset RTSS, feed sensori, LHM, log, percorsi
  src/monitor/
    main.cpp              dispatcher delle modalità
    app.*                 ciclo principale, tray, hotkey, selezione profilo
    frame_source.*        FPS via ETW
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
    settings_window.*     finestra principale delle impostazioni
    layout_editor.*       editor del layout libero e dei preset RTSS semplici
    sensor_browser.*      browser dei sensori
    games_editor.*        lista giochi (Steam / Epic / GOG / programmi aperti)
    gallery.*             store overlay (base + GitHub)
    theme.*               tema scuro
```

## Lingua dell'interfaccia

I testi sono scritti in italiano direttamente nel codice e passano da `T()` / `TU()` / `TF()` (`src/common/i18n.*`). Con l'inglese attivo, queste funzioni restituiscono la traduzione della tabella `src/common/i18n_en.inc`, che usa come chiave il testo italiano esatto. Se una frase manca dalla tabella, resta in italiano.

La lingua è `language` in `config.json` (`"it"` / `"en"`, vuota = lingua di Windows):
- **Impostazioni**: la leggono all'avvio e si riaprono quando cambia.
- **Overlay**: la riapplica a ogni ricaricamento della configurazione.

## Sorgenti dei dati

| Dato | Sorgente | Fallback |
|---|---|---|
| FPS, frame time, P95/P99, 1% low, stutter | eventi ETW `Present` | `n/d` se ETW non è disponibile |
| CPU utilizzo, per core, frequenza | PDH `% Processor Utility` / `% Processor Performance` | `% Processor Time` |
| CPU temperatura / potenza | HWiNFO → LibreHardwareMonitor (WMI o HTTP `127.0.0.1:8085`) | omessa |
| GPU NVIDIA (tutti i sensori) | NVML, caricata dinamicamente | contatori `GPU Engine` / DXGI |
| GPU utilizzo, VRAM (altre) | contatori `GPU Engine` / `GPU Adapter Memory` + DXGI | — |
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

## File di configurazione

Tutto si trova in `%APPDATA%\PerfOverlay Supreme\`:

| File | Contenuto |
|---|---|
| `config.json` | impostazioni globali (refresh, hotkey, fuori dal gioco, profilo del desktop, lingua, autostart, ...) |
| `games.json` | giochi noti, appresi, esclusi (formato in `src/config/games.example.json`) |
| `profiles\<Nome>.json` | un file per profilo; `default.json` è il fallback |
| `presets\` | preset RTSS importati, con immagini e font |
| `downloads\` | pacchetti scaricati dallo store |
| `overlay.log` | log di debug (ruota a 5 MB) |

LibreHardwareMonitor, se installato dalle impostazioni, si trova in `%LOCALAPPDATA%\PerfOverlay Supreme\LibreHardwareMonitor`.
