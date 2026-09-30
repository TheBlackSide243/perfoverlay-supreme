#pragma once
#include <filesystem>
#include <string_view>

namespace po {

// Finestra nascosta del monitor: la UI la usa per chiedere il ricaricamento della config.
inline constexpr wchar_t kMonitorWndClass[] = L"PerfOverlaySupremeMonitor";
inline constexpr unsigned kMsgReload = 0x8000 /*WM_APP*/ + 2;
// wParam 1 = sospendi le hotkey globali, 0 = riattivale (usato mentre la UI acquisisce una combinazione).
inline constexpr unsigned kMsgSuspendHotkeys = 0x8000 /*WM_APP*/ + 3;
// Chiude il monitor (anche se elevato): usato per aggiornarlo senza passare dalla tray.
inline constexpr unsigned kMsgQuit = 0x8000 /*WM_APP*/ + 4;

// "Ctrl+Shift+O", "Alt+F10", ... → modificatori MOD_* e virtual key.
bool ParseHotkey(std::string_view text, unsigned& mods, unsigned& vk);

// Avvio con Windows tramite attività pianificata con privilegi elevati (richiede un processo elevato).
bool SetAutostart(bool enable, const std::filesystem::path& exe);
bool IsAutostartEnabled();

// true se il monitor era in esecuzione ed è stato notificato.
bool NotifyMonitorReload();
void NotifyMonitorSuspendHotkeys(bool suspend);

}  // namespace po
