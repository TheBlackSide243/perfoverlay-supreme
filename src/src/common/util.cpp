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

std::wstring CleanTitle(std::wstring_view w) {
  std::wstring out;
  out.reserve(w.size());
  for (const wchar_t c : w) {
    const bool invisible = c == 0x00AD || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
                           (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
    if (!invisible) out.push_back(c);
  }
  constexpr const wchar_t* kSpace = L" \t\r\n";
  const auto b = out.find_first_not_of(kSpace);
  if (b == std::wstring::npos) return {};
  return out.substr(b, out.find_last_not_of(kSpace) - b + 1);
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
