#include "common/i18n.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "common/paths.h"

namespace po {
namespace {

struct WEntry {
  const wchar_t* it;
  const wchar_t* en;
};
struct NEntry {
  const char* it;
  const char* en;
};
#include "common/i18n_en.inc"

std::atomic<int> g_lang{-1};

Lang SystemLang() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN ? Lang::It : Lang::En; }

// Letta direttamente da config.json: T() può servire già durante l'inizializzazione statica.
Lang LoadLang() {
  try {
    std::ifstream f(ConfigPath());
    if (f) {
      const auto j = nlohmann::json::parse(f, nullptr, false, true);
      if (j.is_object() && j.contains("language") && j["language"].is_string())
        return LangFromCode(j["language"].get<std::string>());
    }
  } catch (...) {
  }
  return SystemLang();
}

template <class Map, class Entry, size_t N>
Map BuildMap(const Entry (&entries)[N]) {
  Map m;
  for (const auto& e : entries) m.emplace(e.it, e.en);
  return m;
}

}  // namespace

Lang LangFromCode(const std::string& code) {
  if (code == "it") return Lang::It;
  if (code == "en") return Lang::En;
  return SystemLang();
}

const char* LangCode(Lang lang) { return lang == Lang::En ? "en" : "it"; }

Lang CurrentLang() {
  int v = g_lang.load();
  if (v < 0) {
    v = int(LoadLang());
    g_lang = v;
  }
  return Lang(v);
}

void SetLang(Lang lang) { g_lang = int(lang); }

const wchar_t* T(const wchar_t* it) {
  if (!it || CurrentLang() == Lang::It) return it;
  static const auto map = BuildMap<std::unordered_map<std::wstring_view, const wchar_t*>>(kWide);
  const auto f = map.find(it);
  return f == map.end() ? it : f->second;
}

const char* TU(const char* it) {
  if (!it || CurrentLang() == Lang::It) return it;
  static const auto map = BuildMap<std::unordered_map<std::string_view, const char*>>(kNarrow);
  const auto f = map.find(it);
  return f == map.end() ? it : f->second;
}

}  // namespace po
