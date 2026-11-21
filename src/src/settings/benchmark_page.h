#pragma once
#include <windows.h>

#include <string>

// Sezione "Benchmark" delle impostazioni: elenco delle registrazioni (benchmark e riepiloghi di partita),
// dettaglio con i numeri principali, grafico e confronto con un'altra registrazione.
namespace po::ui {

inline constexpr wchar_t kBenchViewClass[] = L"POBenchView";  // riquadro del dettaglio
void RegisterBenchView(HINSTANCE inst);

struct BenchPageControls {
  HWND list = nullptr;     // LISTBOX owner-draw delle registrazioni
  HWND filter = nullptr;   // combobox: tutte / solo benchmark / solo partite
  HWND compare = nullptr;  // combobox "Confronta con"
  HWND view = nullptr;     // kBenchViewClass
  HWND remove = nullptr;   // pulsante Elimina
  HWND file = nullptr;     // pulsante Mostra file
};
void BenchPageInit(const BenchPageControls& c);
void BenchPageSetHotkey(const std::wstring& hotkey);  // per il testo mostrato quando non c'è ancora niente
void BenchPageReload();          // rilegge i file; la selezione resta sulla stessa registrazione
void BenchPageListChanged();     // selezione della lista cambiata
void BenchPageCompareChanged();  // scelta di "Confronta con" cambiata
void BenchPageDrawItem(const DRAWITEMSTRUCT* d);
int BenchPageItemHeight(HWND list);  // altezza di una voce alla scala attuale
bool BenchPageDelete(HWND owner);    // chiede conferma; true se ha eliminato la registrazione scelta
void BenchPageShowFile();            // apre la cartella con il file della registrazione selezionato

}  // namespace po::ui
