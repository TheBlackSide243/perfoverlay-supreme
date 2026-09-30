#include "common/log.h"

#include <windows.h>
#include <share.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

#include "common/paths.h"

namespace po {
namespace {
std::mutex g_mu;
std::string g_component = "app";
std::filesystem::path g_path;
constexpr std::uintmax_t kMaxLogSize = 5 * 1024 * 1024;
}  // namespace

void LogInit(std::string_view component) {
  std::lock_guard lock(g_mu);
  g_component = component;
  g_path = LogPath();
  std::error_code ec;
  if (std::filesystem::exists(g_path, ec) && std::filesystem::file_size(g_path, ec) > kMaxLogSize) {
    auto old = g_path;
    old += L".1";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(g_path, old, ec);
  }
}

void LogWrite(std::string_view level, std::string_view msg) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::lock_guard lock(g_mu);
  const auto line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] {} {}\n", st.wYear, st.wMonth,
                                st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_component, level, msg);
  if (g_path.empty()) g_path = LogPath();
  // Apertura per riga: il file è condiviso tra i due processi e il volume è basso.
  if (FILE* f = _wfsopen(g_path.c_str(), L"ab", _SH_DENYNO)) {
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
  }
  OutputDebugStringA(line.c_str());
}

}  // namespace po
