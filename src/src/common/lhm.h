#pragma once
#include <filesystem>
#include <string>

namespace po {

// LibreHardwareMonitor (open source, GitHub): legge temperature di CPU e scheda madre, ventole e tensioni
// tramite il suo driver firmato (PawnIO). PerfOverlay lo scarica su richiesta, lo configura con il server
// web locale attivo (http://localhost:8085/data.json) e lo avvia nascosto nella tray.
inline constexpr int kLhmPort = 8085;

std::filesystem::path LhmDir();  // %LOCALAPPDATA%\PerfOverlay Supreme\LibreHardwareMonitor
std::filesystem::path LhmExe();
bool LhmInstalled();
bool LhmRunning();
// Scarica l'ultima versione da GitHub ed estrae; scrive la configurazione. false + errore se non riesce.
bool InstallLhm(std::wstring& error);
// Avvia LibreHardwareMonitor (se non è già in esecuzione). Chiede l'amministratore se serve.
bool StartLhm();

}  // namespace po
