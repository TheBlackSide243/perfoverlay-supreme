#pragma once
#include <format>
#include <string>

// Lingua dell'interfaccia. I testi sono scritti in italiano nel codice; T() li sostituisce con la
// traduzione inglese (tabella in i18n_en.inc) quando la lingua attiva è l'inglese.
namespace po {

enum class Lang { It, En };

Lang CurrentLang();  // alla prima chiamata: "language" di config.json, altrimenti la lingua di Windows
void SetLang(Lang lang);
Lang LangFromCode(const std::string& code);  // "it" / "en"; vuoto o sconosciuto = lingua di Windows
const char* LangCode(Lang lang);

const wchar_t* T(const wchar_t* it);  // testo nella lingua attiva
const char* TU(const char* it);       // idem per i testi UTF-8

// std::format con la stringa di formato tradotta.
template <class... A>
std::wstring TF(const wchar_t* fmt, const A&... args) {
  return std::vformat(T(fmt), std::make_wformat_args(args...));
}
template <class... A>
std::string TFU(const char* fmt, const A&... args) {
  return std::vformat(TU(fmt), std::make_format_args(args...));
}

}  // namespace po
