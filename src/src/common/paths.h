#pragma once
#include <filesystem>

namespace po {

std::filesystem::path AppDataDir();   // %APPDATA%\PerfOverlay (creata se manca)
std::filesystem::path ProfilesDir();  // %APPDATA%\PerfOverlay\profiles
std::filesystem::path ConfigPath();   // config.json
std::filesystem::path GamesPath();    // games.json
std::filesystem::path LogPath();      // overlay.log
std::filesystem::path PresetsDir();   // presets\ (immagini e font dei preset RTSS importati)
std::filesystem::path ExeDir();       // cartella dell'eseguibile corrente

}  // namespace po
