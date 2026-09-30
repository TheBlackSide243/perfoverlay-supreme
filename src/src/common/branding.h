#pragma once

// Identità dell'app. PerfOverlay Supreme è separata da PerfOverlay: cartella dati, classi
// finestra, mutex, sessione ETW e attività pianificata diverse, così le due possono convivere.
namespace po {

inline constexpr wchar_t kAppName[] = L"PerfOverlay Supreme";
inline constexpr wchar_t kAppTitleUpper[] = L"PERFOVERLAY SUPREME";
inline constexpr wchar_t kAppId[] = L"PerfOverlaySupreme";     // task, classi, mutex, ETW
inline constexpr wchar_t kAppDataDirName[] = L"PerfOverlay Supreme";
inline constexpr wchar_t kLegacyAppDataDirName[] = L"PerfOverlay";  // importata al primo avvio
// Un solo eseguibile: overlay (senza argomenti), impostazioni (--settings), prova DirectX (--test).
inline constexpr wchar_t kMonitorExe[] = L"PerfOverlaySupreme.exe";

}  // namespace po
