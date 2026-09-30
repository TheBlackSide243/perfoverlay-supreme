#include "common/util.h"

#include <windows.h>

namespace po {

std::string ToUtf8(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring ToWide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

std::vector<std::string> Split(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t p = s.find(sep, start);
    out.emplace_back(s.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
    if (p == std::string_view::npos) break;
    start = p + 1;
  }
  return out;
}

}  // namespace po
