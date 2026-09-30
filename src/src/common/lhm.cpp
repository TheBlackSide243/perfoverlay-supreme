#include "common/lhm.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wininet.h>

#include <format>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/branding.h"
#include "common/log.h"
#include "common/util.h"
#include "common/i18n.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")

namespace po {
namespace fs = std::filesystem;

namespace {

std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Opzioni di LibreHardwareMonitor (file .config accanto all'exe, formato appSettings).
void WriteConfig() {
  const fs::path cfg = LhmDir() / L"LibreHardwareMonitor.config";
  std::ofstream(cfg, std::ios::binary)
      << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<configuration>\r\n  <appSettings>\r\n"
      << "    <add key=\"runWebServerMenuItem\" value=\"true\" />\r\n"
      << std::format("    <add key=\"listenerPort\" value=\"{}\" />\r\n", kLhmPort)
      << "    <add key=\"startMinMenuItem\" value=\"true\" />\r\n"
      << "    <add key=\"minTrayMenuItem\" value=\"true\" />\r\n"
      << "    <add key=\"minCloseMenuItem\" value=\"true\" />\r\n"
      << "  </appSettings>\r\n</configuration>\r\n";
}

}  // namespace

fs::path LhmDir() {
  PWSTR p = nullptr;
  fs::path base;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) base = p;
  CoTaskMemFree(p);
  return base / kAppDataDirName / L"LibreHardwareMonitor";
}

fs::path LhmExe() { return LhmDir() / L"LibreHardwareMonitor.exe"; }

bool LhmInstalled() {
  std::error_code ec;
  return fs::exists(LhmExe(), ec);
}

bool LhmRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{sizeof(pe)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    found = _wcsicmp(pe.szExeFile, L"LibreHardwareMonitor.exe") == 0;
  CloseHandle(snap);
  return found;
}

bool InstallLhm(std::wstring& error) {
  std::error_code ec;
  fs::create_directories(LhmDir(), ec);
  const fs::path json = LhmDir() / L"release.json", zip = LhmDir() / L"release.zip";
  constexpr wchar_t kApi[] = L"https://api.github.com/repos/LibreHardwareMonitor/LibreHardwareMonitor/releases/latest";
  DeleteUrlCacheEntryW(kApi);
  if (FAILED(URLDownloadToFileW(nullptr, kApi, json.c_str(), 0, nullptr))) {
    error = T(L"GitHub non raggiungibile (controlla la connessione).");
    return false;
  }
  std::string url, tag;
  try {
    const auto j = nlohmann::json::parse(ReadAll(json));
    tag = j.value("tag_name", "");
    for (const auto& a : j.at("assets"))
      if (a.value("name", "") == "LibreHardwareMonitor.zip") url = a.value("browser_download_url", "");
  } catch (...) {
  }
  fs::remove(json, ec);
  if (url.empty()) {
    error = T(L"Pacchetto di LibreHardwareMonitor non trovato su GitHub.");
    return false;
  }
  if (FAILED(URLDownloadToFileW(nullptr, ToWide(url).c_str(), zip.c_str(), 0, nullptr))) {
    error = T(L"Download di LibreHardwareMonitor non riuscito.");
    return false;
  }
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);  // tar.exe di Windows estrae gli zip
  std::wstring cmd = std::format(L"\"{}\\tar.exe\" -xf \"{}\" -C \"{}\"", sys, zip.wstring(), LhmDir().wstring());
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = T(L"Impossibile estrarre LibreHardwareMonitor.");
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  fs::remove(zip, ec);
  if (!LhmInstalled()) {
    error = T(L"Pacchetto scaricato ma LibreHardwareMonitor.exe non trovato.");
    return false;
  }
  WriteConfig();
  LogInfo("LibreHardwareMonitor {} installato in {}", tag, ToUtf8(LhmDir().wstring()));
  return true;
}

bool StartLhm() {
  if (!LhmInstalled()) return false;
  if (LhmRunning()) return true;
  WriteConfig();  // server web sempre attivo, anche se l'utente ha cambiato le opzioni
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.lpVerb = L"runas";  // LibreHardwareMonitor richiede l'amministratore (driver dei sensori)
  const std::wstring exe = LhmExe().wstring(), dir = LhmDir().wstring();
  sei.lpFile = exe.c_str();
  sei.lpDirectory = dir.c_str();
  sei.nShow = SW_SHOWMINNOACTIVE;
  const bool ok = ShellExecuteExW(&sei) != FALSE;
  LogInfo("Avvio di LibreHardwareMonitor: {}", ok ? "ok" : "annullato");
  return ok;
}

}  // namespace po
