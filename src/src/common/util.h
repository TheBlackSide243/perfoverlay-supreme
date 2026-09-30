#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace po {

std::string ToUtf8(std::wstring_view w);
std::wstring ToWide(std::string_view s);
std::string Trim(std::string_view s);
std::vector<std::string> Split(std::string_view s, char sep);

inline std::string ToLowerAscii(std::string s) {
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

inline bool IEquals(std::string_view a, std::string_view b) {
  return ToLowerAscii(std::string(a)) == ToLowerAscii(std::string(b));
}

inline bool IContains(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  return ToLowerAscii(std::string(hay)).find(ToLowerAscii(std::string(needle))) != std::string::npos;
}

}  // namespace po
