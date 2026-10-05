#include "settings/updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/branding.h"
#include "common/i18n.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/util.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

#ifndef PO_VERSION
#define PO_VERSION "0.0.0"
#endif

namespace po {
namespace fs = std::filesystem;

namespace {

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

// GET via WinHTTP (niente cache di Internet Explorer, segue i redirect di GitHub verso il CDN).
// Con out != nullptr la risposta va su file, altrimenti in body.
bool HttpGet(const std::wstring& url, std::string* body, std::ofstream* out) {
  wchar_t host[256] = {}, path[4096] = {}, extra[2048] = {};
  URL_COMPONENTS uc{sizeof(uc)};
  uc.lpszHostName = host;
  uc.dwHostNameLength = DWORD(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = DWORD(std::size(path));
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = DWORD(std::size(extra));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

  Handle s{WinHttpOpen(L"PerfOverlaySupreme-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                       WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!s.h) return false;
  WinHttpSetTimeouts(s.h, 5000, 10000, 15000, 30000);
  Handle c{WinHttpConnect(s.h, host, uc.nPort, 0)};
  const std::wstring object = std::wstring(path) + extra;
  Handle r{c.h ? WinHttpOpenRequest(c.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
               : nullptr};
  if (!r.h) return false;
  const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
  if (!WinHttpSendRequest(r.h, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(r.h, nullptr))
    return false;
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(r.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    LogWarn("Aggiornamento: HTTP {} da {}", status, ToUtf8(url));
    return false;
  }
  std::vector<char> chunk;
  for (DWORD avail = 0; WinHttpQueryDataAvailable(r.h, &avail) && avail > 0;) {
    chunk.resize(avail);
    DWORD read = 0;
    if (!WinHttpReadData(r.h, chunk.data(), avail, &read) || read == 0) return false;
    if (out)
      out->write(chunk.data(), read);
    else
      body->append(chunk.data(), read);
  }
  return out ? bool(*out) : true;
}

std::array<int, 3> ParseVersion(std::string v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  std::array<int, 3> n{};
  sscanf_s(v.c_str(), "%d.%d.%d", &n[0], &n[1], &n[2]);
  return n;
}

std::string Sha256(const fs::path& file) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::ifstream f(file, std::ios::binary);
    std::vector<char> buf(1 << 16);
    while (f) {
      f.read(buf.data(), std::streamsize(buf.size()));
      if (f.gcount() > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(f.gcount()), 0);
    }
    UCHAR digest[32] = {};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
      for (UCHAR b : digest) hex += std::format("{:02x}", b);
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

// Note di rilascio leggibili: senza le sezioni che GitHub aggiunge da solo.
std::string CleanNotes(std::string s) {
  for (const char* cut : {"## What's Changed", "**Full Changelog**", "## New Contributors"})
    if (const size_t p = s.find(cut); p != std::string::npos) s.erase(p);
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  s = Trim(s);
  if (s.size() > 700) s = s.substr(0, 700) + "...";
  return s;
}

fs::path OldExePath() { return fs::path(ExePath().wstring() + L".old"); }

}  // namespace

const char* AppVersion() { return PO_VERSION; }

std::optional<UpdateInfo> CheckForUpdate(std::wstring& error) {
  error.clear();
  std::string body;
  if (!HttpGet(ToWide(std::string("https://api.github.com/repos/") + kGitHubRepo + "/releases/latest"), &body,
               nullptr)) {
    error = T(L"GitHub non raggiungibile.");
    return std::nullopt;
  }
  try {
    const auto j = nlohmann::json::parse(body);
    UpdateInfo u;
    u.version = j.value("tag_name", "");
    if (!u.version.empty() && (u.version[0] == 'v' || u.version[0] == 'V')) u.version.erase(0, 1);
    u.notes = CleanNotes(j.value("body", ""));
    u.pageUrl = j.value("html_url", "");
    for (const auto& a : j.value("assets", nlohmann::json::array())) {
      const std::string name = a.value("name", "");
      if (ToLowerAscii(name) != "perfoverlaysupreme.exe") continue;
      u.assetUrl = a.value("browser_download_url", "");
      const std::string digest = a.value("digest", "");
      if (digest.rfind("sha256:", 0) == 0) u.sha256 = ToLowerAscii(digest.substr(7));
    }
    LogInfo("Aggiornamento: ultima release {} (in uso {})", u.version, AppVersion());
    if (u.assetUrl.empty() || ParseVersion(u.version) <= ParseVersion(AppVersion())) return std::nullopt;
    return u;
  } catch (const std::exception& e) {
    LogWarn("Aggiornamento: risposta di GitHub non valida ({})", e.what());
    error = T(L"GitHub non raggiungibile.");
    return std::nullopt;
  }
}

fs::path UpdateDownloadPath() { return fs::path(ExePath().wstring() + L".download"); }

bool DownloadUpdate(const UpdateInfo& u, std::wstring& error) {
  const fs::path dst = UpdateDownloadPath();
  {
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = T(L"Impossibile scrivere nella cartella dell'exe (cartella protetta?): scarica la nuova versione dalla pagina della release.");
      return false;
    }
    if (!HttpGet(ToWide(u.assetUrl), nullptr, &out)) {
      out.close();
      std::error_code ec;
      fs::remove(dst, ec);
      error = T(L"Download non riuscito.");
      return false;
    }
  }
  // Deve essere un eseguibile Windows e, se GitHub ne indica l'impronta, identico a quello della release.
  char mz[2] = {};
  std::ifstream(dst, std::ios::binary).read(mz, 2);
  std::error_code ec;
  const bool exe = mz[0] == 'M' && mz[1] == 'Z' && fs::file_size(dst, ec) > 256 * 1024;
  const std::string hash = Sha256(dst);
  if (!exe || (!u.sha256.empty() && hash != u.sha256)) {
    LogWarn("Aggiornamento: file scaricato non valido (sha256 {} atteso {})", hash, u.sha256);
    fs::remove(dst, ec);
    error = T(L"Il file scaricato non corrisponde a quello della release: aggiornamento annullato.");
    return false;
  }
  LogInfo("Aggiornamento {} scaricato ({} byte, sha256 {}{})", u.version, fs::file_size(dst, ec), hash,
          u.sha256.empty() ? ", impronta non indicata da GitHub" : ", verificato");
  return true;
}

bool InstallUpdate(std::wstring& error) {
  const fs::path exe = ExePath(), old = OldExePath(), dl = UpdateDownloadPath();
  DeleteFileW(old.c_str());  // resto di un aggiornamento precedente
  // L'exe in esecuzione non si può cancellare ma si può rinominare.
  if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    error = T(L"Impossibile sostituire l'exe (cartella protetta?): scarica la nuova versione dalla pagina della release.");
    LogWarn("Aggiornamento: rinomina di {} non riuscita (errore {})", ToUtf8(exe.wstring()), GetLastError());
    return false;
  }
  if (!MoveFileExW(dl.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LogWarn("Aggiornamento: sostituzione non riuscita (errore {})", GetLastError());
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    error = T(L"Impossibile sostituire l'exe (cartella protetta?): scarica la nuova versione dalla pagina della release.");
    return false;
  }
  LogInfo("Aggiornamento installato in {}", ToUtf8(exe.wstring()));
  return true;
}

void CleanupUpdateFiles() {
  // Il vecchio exe resta occupato finché il vecchio overlay non si chiude: si riprova al prossimo avvio.
  DeleteFileW(OldExePath().c_str());
}

}  // namespace po
