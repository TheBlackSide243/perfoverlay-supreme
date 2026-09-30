#pragma once
#include <format>
#include <string>
#include <string_view>

// Log su %APPDATA%\PerfOverlay\overlay.log (condiviso da monitor e settings).
namespace po {

void LogInit(std::string_view component);
void LogWrite(std::string_view level, std::string_view msg);

template <class... A>
void LogInfo(std::format_string<A...> f, A&&... a) {
  LogWrite("INFO ", std::format(f, std::forward<A>(a)...));
}
template <class... A>
void LogWarn(std::format_string<A...> f, A&&... a) {
  LogWrite("WARN ", std::format(f, std::forward<A>(a)...));
}
template <class... A>
void LogError(std::format_string<A...> f, A&&... a) {
  LogWrite("ERROR", std::format(f, std::forward<A>(a)...));
}

}  // namespace po
