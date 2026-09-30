# Guida di PerfOverlay Supreme

## Indice

- [Avvio e menu](#avvio-e-menu)
- [Quando appare l'overlay](#quando-appare-loverlay)
- [Profili e overlay sul desktop](#profili-e-overlay-sul-desktop)
- [Store overlay](#store-overlay)
- [Preset RTSS (.ovx / .ovl)](#preset-rtss-ovx--ovl)
- [Overlay TroyMetrics e preset RTSS avanzati](#overlay-troymetrics-e-preset-rtss-avanzati)
- [Layout libero (editor in stile RTSS)](#layout-libero-editor-in-stile-rtss)
- [Sensori di sistema](#sensori-di-sistema)
- [Temperatura CPU](#temperatura-cpu)
- [Prova dell'overlay (DirectX 11 / 12)](#prova-delloverlay-directx-11--12)
- [Hotkey](#hotkey)
- [Amministratore e FPS](#amministratore-e-fps)
- [Dati e log](#dati-e-log)
- [Limiti](#limiti)

## Avvio e menu

Tutto è in un solo file, `PerfOverlaySupreme.exe`: overlay, impostazioni, editor, store e finestra di prova.

- **Doppio clic**: avvia l'overlay. Chiede l'amministratore, che serve per gli FPS. Se l'overlay è già attivo, apre le **impostazioni**.
- **Icona vicino all'orologio, tasto destro**: mostra/nascondi, solo FPS, profilo, Impostazioni, Test overlay (DirectX 11/12), log, esci.
- **Riga di comando**:

  | Argomento | Effetto |
  |---|---|
  | *(nessuno)* | overlay (oppure impostazioni se l'overlay è già attivo) |
  | `--settings` | impostazioni |
  | `--test [dx11]` | finestra di prova DirectX 12 (o 11) |
  | `--import-rtss "file.ovx" [profilo]` | importa un preset RTSS nel profilo indicato |

Se sul PC c'è anche PerfOverlay, al primo avvio Supreme importa config, lista giochi e profili, con l'avvio automatico spento. Conviene usarne uno solo alla volta, altrimenti sui giochi compaiono due overlay.

## Quando appare l'overlay

L'overlay appare sui giochi presenti nella lista e su quelli riconosciuti in automatico. Un gioco viene riconosciuto se sta renderizzando e in più usa molta GPU, VRAM o RAM, è a schermo intero oppure si trova in una cartella di giochi. Browser, editor, player video e launcher sono esclusi.

**Impostazioni › "Lista giochi..."** mostra tutti i giochi: quelli aggiunti, quelli appresi in automatico e gli esclusi. Si aggiungono senza scrivere nulla:

- **"Dai programmi aperti..."**: avvia il gioco e spuntalo.
- **"Dai giochi installati..."**: elenco dalle librerie Steam, Epic e GOG. Per i giochi Unreal spunta `...-Win64-Shipping.exe`.
- **"Scegli file .exe..."**.

"Escludi" significa niente overlay su quel programma. "Solo se il titolo contiene" serve per i programmi con più finestre (es. `javaw.exe` + "Minecraft"). OK salva e applica subito.

## Profili e overlay sul desktop

- In gioco si usa il profilo associato al gioco. Se il gioco non ne ha uno, si usa `default` (oppure il profilo forzato dall'hotkey o dalla tray).
- Sul desktop si vede il **profilo selezionato nelle impostazioni** (o scelto dalla tray): selezionarne un altro cambia subito l'overlay.
- Se importi un preset in un profilo non associato a nessun gioco, ti viene chiesto se usarlo per tutti i giochi.

## Store overlay

Impostazioni › **"Store overlay..."**:

- **Overlay base**: sei overlay pronti (Steam classico, Solo FPS, Completo verticale, Compatto a destra, Benchmark grande, Trasparente). Doppio clic per applicarne uno al profilo selezionato.
- **Store in evidenza**: overlay RTSS pubblicati su GitHub dai loro autori (TroyMetrics Benchmark, RTSS-Overlay, ...).
  - Doppio clic su un pacchetto per scaricarlo, poi doppio clic su un overlay per applicarlo.
  - "Scarica / aggiorna" riscarica l'ultima versione.
- **"Cerca su GitHub"**: trova altri repository con overlay RTSS.
  - I risultati non sono verificati: se un repository non contiene overlay, non viene aggiunto nulla.
  - Si scaricano solo file `.ovl`/`.ovx`, immagini e font.

Gli overlay scaricati restano dei rispettivi autori: PerfOverlay Supreme li scarica dai loro repository e non li ridistribuisce.

## Preset RTSS (.ovx / .ovl)

Impostazioni › **"Importa RTSS..."** applica un preset dell'OverlayEditor di RivaTuner al profilo selezionato (Layout = "Preset RTSS"). Il preset mantiene righe, colonne, font, dimensioni e colori di RTSS.

- **Dati**: arrivano da PerfOverlay: FPS, utilizzo, temperatura, potenza e frequenza di CPU e GPU, VRAM, RAM, batteria (livello, consumo, autonomia) e ora `%Time12%`/`%Time24%`.
- **Dati mancanti**: un dato che il PC non ha (es. BATT su un PC fisso) mostra "N/A", come in RTSS. Sui PC senza batteria, DRAIN ("Charge Rate") mostra il consumo della GPU.
- **Sorgenti non supportate**: dopo l'import la barra di stato indica se qualche sorgente del preset non è supportata.
- **Editor**: con un preset semplice importato, "Editor..." apre l'**editor preset RTSS**. Ogni testo del preset è un blocco da trascinare (o spostare con le frecce) e ridimensionare, con l'anteprima nel font e nei colori del preset.

I preset importati vengono copiati in `%APPDATA%\PerfOverlay Supreme\presets`.

## Overlay TroyMetrics e preset RTSS avanzati

I preset dell'OverlayEditor con immagini, grafici, formule e condizioni (es. TroyMetrics Benchmark) vengono mostrati dal motore completo. Il motore disegna pannelli, sprite animati e colorati (ventole, fiamme, anelli), il grafico del frametime, il barchart dei core e usa i font del preset.

- **Dati**:
  - FPS, frametime e 1% low da PerfOverlay;
  - GPU da NVML;
  - CPU e RAM da Windows;
  - con LibreHardwareMonitor o HWiNFO anche temperatura CPU, potenza e sensori extra.
- **Limiti**: le latenze Reflex/PresentMon mostrano N/A.
- **Grandezza**: "Dimensione" del profilo (15 = originale).
- **Posizione**: "Posizione" e X / Y.
- L'editor non modifica i preset avanzati.

## Layout libero (editor in stile RTSS)

Impostazioni › Profilo selezionato › Layout › **"Editor..."**

- **Formato della tela**: si sceglie in alto a destra. L'editor si apre già su quello del monitor (3440×1440 → 21:9). Se un formato non ha ancora un layout, viene creato copiando il 16:9.
- **Blocchi**: ogni elemento (FPS, grafico FPS, frame time, CPU, GPU, RAM, batteria, sensori) è un blocco da trascinare col mouse.
- **Tastiera**:
  - frecce: sposta di 1 pixel;
  - Shift+frecce: sposta di un passo di griglia;
  - Alt mentre trascini: niente aggancio alla griglia;
  - PagSu / PagGiù: blocco precedente / successivo.
- **"Pixel in"**: mostra X e Y nei pixel del tuo monitor o a 1080p / 1440p / 4K. Cambia solo come li leggi: l'overlay si adatta comunque a ogni risoluzione.
- **Zoom**:
  - Ctrl+rotellina (o + / −) ingrandisce la tela, 0 torna alla tela intera;
  - rotellina, Shift+rotellina o tasto centrale del mouse spostano la vista.
- **Pannello a destra**:
  - visibilità di ogni elemento;
  - X / Y e dimensione % del blocco selezionato;
  - passo della griglia;
  - "Ripristina disposizione".
- **OK** salva e applica subito (il layout del profilo diventa "Libero").

Le coordinate sono in pixel di una tela alta 1080 e vengono riportate in proporzione sulla risoluzione del gioco: un blocco messo in basso a destra resta in basso a destra. In gioco viene usato il layout del formato più vicino a quello della finestra del gioco.

## Sensori di sistema

Impostazioni › Profilo selezionato › **"Sensori di sistema"** (anche dall'editor) mostra tutti i sensori letti dall'overlay. Per ogni sensore ci sono valore, minimo e massimo, aggiornati ogni secondo, e una casella di ricerca. Spunta un sensore per mostrarlo nell'overlay e scegli l'etichetta con cui compare.

- **Integrati**:
  - CPU totale e per core (utilizzo, frequenza);
  - GPU NVIDIA completa: temperatura, ventole, clock, consumo e limite, VRAM, P-state, encoder/decoder, PCIe;
  - RAM;
  - dischi: temperatura SSD, attività, lettura/scrittura;
  - rete, batteria, sistema.
- **Temperature di CPU e scheda madre, ventole del case, tensioni**: Windows non le espone senza un driver. Servono LibreHardwareMonitor (vedi sotto) o HWiNFO con "Shared Memory Support". I loro sensori compaiono nella stessa finestra.

`PerfOverlaySupreme.exe` deve essere in esecuzione per vedere i valori.

## Temperatura CPU

Senza un driver, su Ryzen e Intel Windows non espone la temperatura della CPU. Per averla, apri Impostazioni › "Sensori di sistema" › **"Temperatura CPU..."**. Dopo una conferma PerfOverlay Supreme:

1. scarica [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) (open source) dalle release ufficiali su GitHub in `%LOCALAPPDATA%\PerfOverlay Supreme\LibreHardwareMonitor`;
2. lo avvia nascosto nella tray, con il server web locale su `127.0.0.1:8085`. Al primo avvio LibreHardwareMonitor chiede di installare il suo driver PawnIO;
3. da quel momento lo avvia da solo insieme all'overlay.

In alternativa puoi usare HWiNFO con "Shared Memory Support".

## Prova dell'overlay (DirectX 11 / 12)

Per aprire la scena di prova usa Tray › **"Test overlay (DirectX 11/12)"** oppure `PerfOverlaySupreme.exe --test`. La scena è riconosciuta sempre come gioco. Il titolo della finestra mostra API, FPS, risoluzione e scheda video.

| Tasto | Azione |
|---|---|
| F1 / F2 | DirectX 11 / DirectX 12 |
| V | VSync on/off |
| L | limite FPS (off / 30 / 60 / 144) |
| Su / Giù | carico GPU |
| F11 o Alt+Invio | schermo intero senza bordi |
| Esc | esci |

## Hotkey

| Hotkey | Azione |
|---|---|
| Ctrl+Shift+O | mostra / nascondi overlay |
| Ctrl+Shift+P | cambia profilo (automatico → profili → automatico) |
| Ctrl+Shift+F | modalità solo FPS |

Si cambiano da Impostazioni › "Cambia" accanto alla hotkey.

## Amministratore e FPS

`PerfOverlaySupreme.exe` chiede l'amministratore (UAC) quando avvii l'overlay a mano: serve per leggere gli FPS dagli eventi ETW di Windows.

Con "Avvia PerfOverlay Supreme con Windows" attivo viene creata l'attività pianificata `PerfOverlaySupreme`, che lo avvia all'accesso senza richieste UAC.

## Dati e log

Tutto si trova in `%APPDATA%\PerfOverlay Supreme\`:

- `config.json`
- `games.json`
- `profiles\`
- `presets\`
- `downloads\`
- `overlay.log`

Per aprire la cartella dalle impostazioni: "Apri cartella dati".

## Limiti

- Nel fullscreen **esclusivo** l'overlay non si vede. Usa la modalità "Finestra senza bordi" del gioco.
- Alcuni giochi competitivi vietano gli overlay di terze parti: controlla i termini del gioco.
