#include "common/darkmode.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace po {
namespace {
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using FlushMenuThemesFn = void(WINAPI*)();

HMODULE Uxtheme() {
  static HMODULE mod = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return mod;
}
template <class Fn>
Fn Ordinal(WORD n) {
  return Uxtheme() ? reinterpret_cast<Fn>(GetProcAddress(Uxtheme(), MAKEINTRESOURCEA(n))) : nullptr;
}
}  // namespace

void EnableDarkModeForApp() {
  if (auto setMode = Ordinal<SetPreferredAppModeFn>(135)) setMode(PreferredAppMode::ForceDark);
  if (auto flush = Ordinal<FlushMenuThemesFn>(136)) flush();
}

void ApplyDarkTitleBar(HWND hwnd, COLORREF caption) {
  BOOL on = TRUE;
  if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on))))
    DwmSetWindowAttribute(hwnd, 19, &on, sizeof(on));  // build precedenti a 20H1
  DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
  if (auto allow = Ordinal<AllowDarkModeForWindowFn>(133)) allow(hwnd, true);
}

void ApplyDarkControlTheme(HWND ctl, const wchar_t* theme) {
  if (auto allow = Ordinal<AllowDarkModeForWindowFn>(133)) allow(ctl, true);
  SetWindowTheme(ctl, theme, nullptr);
}

}  // namespace po
