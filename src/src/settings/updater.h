#pragma once
#include <filesystem>
#include <optional>
#include <string>

// Aggiornamento dall'interno dell'app: legge l'ultima release su GitHub, scarica l'exe, ne verifica
// l'impronta SHA-256 indicata da GitHub e lo mette al posto di quello in uso.
namespace po {

struct UpdateInfo {
  std::string version;   // "1.2.0"
  std::string notes;     // testo della release, senza la parte generata da GitHub
  std::string assetUrl;  // exe da scaricare
  std::string sha256;    // impronta indicata da GitHub (esadecimale minuscolo), vuota se manca
  std::string pageUrl;   // pagina della release
};

const char* AppVersion();  // versione di questo exe ("1.2.0")

// Versione più nuova di questa, se c'è. error resta vuoto se semplicemente non ci sono novità.
std::optional<UpdateInfo> CheckForUpdate(std::wstring& error);

std::filesystem::path UpdateDownloadPath();  // accanto all'exe: <exe>.download
bool DownloadUpdate(const UpdateInfo& update, std::wstring& error);

// Mette l'exe scaricato al posto di quello in uso (che diventa <exe>.old, cancellato al prossimo avvio).
bool InstallUpdate(std::wstring& error);
void CleanupUpdateFiles();

}  // namespace po
