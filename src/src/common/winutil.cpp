#include "common/winutil.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "common/log.h"
#include "common/util.h"

namespace po {
namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"PerfOverlaySupreme";
}  // namespace

bool ParseHotkey(std::string_view text, unsigned& mods, unsigned& vk) {
  mods = 0;
  vk = 0;
  for (const auto& part : Split(text, '+')) {
    const auto t = ToLowerAscii(Trim(part));
    if (t.empty()) continue;
    if (t == "ctrl" || t == "control") {
      mods |= MOD_CONTROL;
    } else if (t == "shift") {
      mods |= MOD_SHIFT;
    } else if (t == "alt") {
      mods |= MOD_ALT;
    } else if (t == "win") {
      mods |= MOD_WIN;
    } else if (t.size() == 1 && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= '0' && t[0] <= '9'))) {
      vk = unsigned(t[0] >= 'a' ? t[0] - 'a' + 'A' : t[0]);
    } else if (t[0] == 'f' && t.size() <= 3) {
      const int n = std::atoi(t.c_str() + 1);
      if (n < 1 || n > 24) return false;
      vk = VK_F1 + unsigned(n - 1);
    } else if (t == "home") {
      vk = VK_HOME;
    } else if (t == "end") {
      vk = VK_END;
    } else if (t == "insert" || t == "ins") {
      vk = VK_INSERT;
    } else if (t == "delete" || t == "del") {
      vk = VK_DELETE;
    } else if (t == "pageup") {
      vk = VK_PRIOR;
    } else if (t == "pagedown") {
      vk = VK_NEXT;
    } else if (t == "pause") {
      vk = VK_PAUSE;
    } else if (t == "scroll") {
      vk = VK_SCROLL;
    } else {
      return false;
    }
  }
  return vk != 0;
}

namespace {

constexpr wchar_t kTaskName[] = L"PerfOverlaySupreme";

// Esegue un comando senza finestra e ne restituisce il codice di uscita (-1 se non parte).
int RunHidden(std::wstring cmd) {
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, 15000);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

std::wstring XmlEscape(std::wstring_view s) {
  std::wstring out;
  for (wchar_t c : s) {
    switch (c) {
      case L'&': out += L"&amp;"; break;
      case L'<': out += L"&lt;"; break;
      case L'>': out += L"&gt;"; break;
      case L'"': out += L"&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::wstring Env(const wchar_t* name) {
  wchar_t buf[256];
  const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
  return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

std::wstring Schtasks() {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  return L"\"" + std::wstring(sys) + L"\\schtasks.exe\"";
}

// Versioni precedenti usavano HKCU\...\Run: con requireAdministrator Windows ignorerebbe la voce.
void RemoveLegacyRunKey() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
  RegDeleteValueW(key, kRunValue);
  RegCloseKey(key);
}

}  // namespace

// Avvio con Windows: attività pianificata "all'accesso" con privilegi elevati, così
// PerfOverlay (che richiede l'amministratore) parte senza richiesta UAC. Va chiamata da un
// processo già elevato (il monitor).
bool SetAutostart(bool enable, const std::filesystem::path& exe) {
  RemoveLegacyRunKey();
  if (!enable) {
    const int rc = RunHidden(Schtasks() + L" /Delete /TN " + kTaskName + L" /F");
    return rc == 0 || !IsAutostartEnabled();
  }
  const std::wstring user = Env(L"USERDOMAIN") + L"\\" + Env(L"USERNAME");
  const std::wstring xml =
      L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
      L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
      L"  <RegistrationInfo><Description>Avvia PerfOverlay Supreme all'accesso</Description></RegistrationInfo>\n"
      L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEscape(user) + L"</UserId>"
      L"<Delay>PT5S</Delay></LogonTrigger></Triggers>\n"
      L"  <Principals><Principal id=\"Author\"><UserId>" + XmlEscape(user) + L"</UserId>"
      L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n"
      L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
      L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
      L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
      L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>\n"
      L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe.wstring()) + L"</Command>"
      L"<Arguments>--autostart</Arguments></Exec></Actions>\n"
      L"</Task>\n";

  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::filesystem::path xmlPath = std::filesystem::path(tmp) / L"PerfOverlaySupreme-task.xml";
  if (FILE* f = nullptr; _wfopen_s(&f, xmlPath.c_str(), L"wb") == 0 && f) {
    const wchar_t bom = 0xFEFF;
    fwrite(&bom, sizeof(bom), 1, f);
    fwrite(xml.data(), sizeof(wchar_t), xml.size(), f);
    fclose(f);
  } else {
    return false;
  }
  const int rc =
      RunHidden(Schtasks() + L" /Create /TN " + kTaskName + L" /XML \"" + xmlPath.wstring() + L"\" /F");
  std::error_code ec;
  std::filesystem::remove(xmlPath, ec);
  if (rc != 0) LogWarn("Creazione attività pianificata fallita (schtasks {})", rc);
  return rc == 0;
}

bool IsAutostartEnabled() { return RunHidden(Schtasks() + L" /Query /TN " + kTaskName) == 0; }

bool NotifyMonitorReload() {
  HWND h = FindWindowW(kMonitorWndClass, nullptr);
  if (!h) return false;
  PostMessageW(h, kMsgReload, 0, 0);
  return true;
}

void NotifyMonitorSuspendHotkeys(bool suspend) {
  // Sincrono: quando ritorna, le combinazioni sono davvero libere per la cattura.
  if (HWND h = FindWindowW(kMonitorWndClass, nullptr))
    SendMessageTimeoutW(h, kMsgSuspendHotkeys, suspend ? 1 : 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
}

}  // namespace po
