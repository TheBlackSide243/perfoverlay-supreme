#include "common/rtss_preset.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include "common/log.h"
#include "common/paths.h"
#include "common/util.h"
#include "common/i18n.h"

namespace po {
namespace fs = std::filesystem;
namespace {

uint32_t U32(const std::vector<uint8_t>& b, size_t off) {
  uint32_t v = 0;
  std::memcpy(&v, b.data() + off, 4);
  return v;
}

// LZW a larghezza variabile (9..12 bit), bit LSB-first, codice 256 = clear, 257 = fine.
bool LzwDecode(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
  constexpr int kClear = 256, kEnd = 257, kFirst = 258, kMaxBits = 12;
  std::vector<std::vector<uint8_t>> dict;
  auto reset = [&] {
    dict.assign(kFirst, {});
    for (int i = 0; i < 256; ++i) dict[size_t(i)] = {uint8_t(i)};
  };
  reset();
  size_t bitpos = 0;
  const size_t nbits = size * 8;
  int width = 9;
  std::vector<uint8_t> prev;
  bool havePrev = false;
  while (bitpos + size_t(width) <= nbits) {
    int code = 0;
    for (int i = 0; i < width; ++i, ++bitpos) code |= ((data[bitpos >> 3] >> (bitpos & 7)) & 1) << i;
    if (code == kClear) {
      reset();
      width = 9;
      havePrev = false;
      continue;
    }
    if (code == kEnd) break;
    std::vector<uint8_t> entry;
    if (code < int(dict.size()) && (code < 256 || code >= kFirst)) {
      entry = dict[size_t(code)];
    } else if (code == int(dict.size()) && havePrev) {
      entry = prev;
      entry.push_back(prev[0]);
    } else {
      return false;
    }
    out.insert(out.end(), entry.begin(), entry.end());
    if (havePrev && dict.size() < (size_t{1} << kMaxBits)) {
      auto e = prev;
      e.push_back(entry[0]);
      dict.push_back(std::move(e));
      if (dict.size() >= (size_t{1} << width) && width < kMaxBits) ++width;
    }
    prev = std::move(entry);
    havePrev = true;
  }
  return true;
}

// Estrae il testo .ovl da un .ovx (o restituisce il file così com'è se è già testo).
bool ExtractOvl(const std::vector<uint8_t>& raw, std::string& text, std::string& error) {
  if (raw.size() >= 24 && std::memcmp(raw.data(), "0CDU", 4) == 0) {  // "UDC0" letto al contrario
    const uint32_t usize = U32(raw, 4);
    if (std::memcmp(raw.data() + 20, "0WZL", 4) != 0) {
      error = TU("compressione .ovx non supportata (atteso LZW)");
      return false;
    }
    std::vector<uint8_t> dec;
    dec.reserve(usize);
    if (!LzwDecode(raw.data() + 24, raw.size() - 24, dec) || dec.size() != usize) {
      error = TU("file .ovx danneggiato (decompressione fallita)");
      return false;
    }
    // Contenitore interno "0AZS": 20 byte di intestazione, poi il testo del layout.
    size_t start = 0;
    if (dec.size() >= 20 && std::memcmp(dec.data(), "0AZS", 4) == 0) start = 20;
    while (start < dec.size() && dec[start] != '[') ++start;
    text.assign(reinterpret_cast<const char*>(dec.data()) + start, dec.size() - start);
  } else {
    text.assign(raw.begin(), raw.end());
  }
  if (text.find("[Master]") == std::string::npos && text.find("[Layer0]") == std::string::npos) {
    error = TU("il file non è un layout dell'OverlayEditor di RTSS");
    return false;
  }
  // I layout RTSS sono in ANSI (Windows-1252): "°C" ecc. → UTF-8
  const int n = MultiByteToWideChar(1252, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(1252, 0, text.data(), int(text.size()), w.data(), n);
  text = ToUtf8(w);
  return true;
}

using Ini = std::map<std::string, std::unordered_map<std::string, std::string>>;

Ini ParseIni(const std::string& text) {
  Ini ini;
  std::string section;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const auto eq = line.find('=');
    if (eq != std::string::npos && !section.empty()) ini[section][line.substr(0, eq)] = line.substr(eq + 1);
  }
  return ini;
}

int ToInt(const std::unordered_map<std::string, std::string>& s, const char* key, int fallback) {
  const auto it = s.find(key);
  if (it == s.end() || it->second.empty()) return fallback;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return fallback;
  }
}

std::string Val(const std::unordered_map<std::string, std::string>& s, const char* key) {
  const auto it = s.find(key);
  return it == s.end() ? std::string() : it->second;
}

}  // namespace

std::string RtssMetricForSource(const std::string& name, const std::string& id, const std::string& reading) {
  const std::string k = ToLowerAscii(name + " | " + id + " | " + reading);
  auto has = [&](const char* s) { return k.find(s) != std::string::npos; };
  if (has("framerate") || has("frame rate") || ToLowerAscii(name) == "fps") return "fps";
  if (has("frametime") || has("frame time")) return "frametime";
  if (has("charge level") || has("battery level") || has("battery charge")) return "bat.pct";
  if (has("charge rate") || has("discharge rate") || has("battery power")) return "bat.rate_w";
  if (has("remaining time") || has("time remaining")) return "bat.remain_min";
  const bool gpu = has("gpu");
  const bool cpu = has("cpu");
  if (gpu && has("memory usage percent")) return "gpu.vram_pct";
  if (gpu && has("memory usage")) return "gpu.vram_mb";
  if (gpu && has("temperature")) return "gpu.temp";
  if (gpu && has("power")) return "gpu.power";
  if (gpu && has("clock") && !has("memory")) return "gpu.clock";
  if (gpu && has("usage")) return "gpu.usage";
  if (has("ram usage percent") || has("memory usage percent")) return "ram.pct";
  if (has("ram usage")) return "ram.mb";
  if (cpu && has("temperature")) return "cpu.temp";
  if (cpu && has("power")) return "cpu.power";
  if (cpu && has("clock")) return "cpu.clock";
  if (cpu && has("usage")) return "cpu.usage";
  return {};
}

RtssImportResult ImportRtssPreset(const std::filesystem::path& file) {
  RtssImportResult r;
  std::ifstream f(file, std::ios::binary);
  if (!f) {
    r.error = TU("impossibile aprire il file");
    return r;
  }
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string text;
  if (!ExtractOvl(raw, text, r.error)) return r;
  const Ini ini = ParseIni(text);

  auto get = [&](const std::string& sec) -> const std::unordered_map<std::string, std::string>& {
    static const std::unordered_map<std::string, std::string> kEmpty;
    const auto it = ini.find(sec);
    return it == ini.end() ? kEmpty : it->second;
  };
  const auto& master = get("Master");
  const auto& general = get("General");
  r.layout.name = ToUtf8(file.filename().wstring());
  if (const std::string face = Val(master, "FontFace"); !face.empty()) r.layout.fontFace = face;
  r.layout.fontWeight = ToInt(master, "FontWeight", 400);
  r.layout.fontHeight = ToInt(master, "FontHeight", 0);
  r.layout.zoomRatio = std::max(1, ToInt(master, "ZoomRatio", 1));
  const auto& settings = get("Settings");

  const int sources = ToInt(general, "Sources", 0);
  bool formulas = false;
  for (int i = 0; i < sources; ++i) {
    const auto& s = get("Source" + std::to_string(i));
    const std::string name = Val(s, "Name");
    if (name.empty()) continue;
    RtssSourceDef def{name,           Val(s, "Units"), Val(s, "Format"), Val(s, "Formula"),
                      Val(s, "Provider"), Val(s, "ID"), Val(s, "ReadingName")};
    formulas = formulas || !def.formula.empty();
    r.layout.defs.push_back(std::move(def));
    RtssSource src;
    src.metric = RtssMetricForSource(name, Val(s, "ID"), Val(s, "ReadingName"));
    // Formato stile printf: "%.1f" → 1 decimale; vuoto → intero, come in RTSS.
    const std::string fmt = Val(s, "Format");
    if (const auto dot = fmt.find('.'); dot != std::string::npos && dot + 1 < fmt.size() && isdigit(fmt[dot + 1]))
      src.decimals = fmt[dot + 1] - '0';
    if (src.metric.empty()) r.unsupported.push_back(name);
    r.layout.sources[name] = src;
  }

  const int layers = ToInt(general, "Layers", 0);
  for (int i = 0; i < layers; ++i) {
    const auto& s = get("Layer" + std::to_string(i));
    if (s.empty()) continue;
    RtssLayer l;
    l.text = Val(s, "Text");
    l.x = ToInt(s, "PositionX", 0);
    l.y = ToInt(s, "PositionY", 0);
    l.extentX = ToInt(s, "ExtentX", 0);
    l.extentY = ToInt(s, "ExtentY", 0);
    l.origin = ToInt(s, "ExtentOrigin", 0);
    l.size = ToInt(s, "Size", 100);
    l.name = Val(s, "Name");
    l.colorSpec = Val(s, "TextColor");
    l.bgColor = Val(s, "BgndColor");
    l.visibility = Val(s, "VisibilitySource");
    l.marginTop = ToInt(s, "MarginTop", 0);
    l.marginBottom = ToInt(s, "MarginBottom", 0);
    // Colore RRGGBB esadecimale, senza zeri iniziali ("FF00" = verde)
    const std::string c = Val(s, "TextColor");
    if (!c.empty()) {
      try {
        const unsigned long v = std::stoul(c, nullptr, 16);
        l.color = {uint8_t((v >> 16) & 0xFF), uint8_t((v >> 8) & 0xFF), uint8_t(v & 0xFF)};
      } catch (...) {
      }
    }
    if (!l.text.empty()) r.layout.layers.push_back(std::move(l));
  }
  if (r.layout.layers.empty()) {
    r.error = TU("il preset non contiene layer di testo");
    return r;
  }

  // Preset avanzato (ipertesto con immagini/grafici/condizioni, formule): motore completo.
  const std::string image = Val(settings, "EmbeddedImage");
  bool hyper = false;
  for (const auto& l : r.layout.layers)
    for (const char* tag : {"<I=", "<AI=", "<G=", "<B=", "<IF", "<P="})
      hyper = hyper || l.text.find(tag) != std::string::npos;
  r.layout.advanced = hyper || formulas || !image.empty();
  if (r.layout.advanced) {
    r.unsupported.clear();  // le sorgenti le risolve il motore (HAL, HWiNFO, PresentMon, formule)
    std::error_code ec;
    const auto dir = file.parent_path();
    if (!image.empty()) {
      const auto src = dir / ToWide(image);
      if (fs::copy_file(src, PresetsDir() / ToWide(image), fs::copy_options::overwrite_existing, ec))
        r.layout.image = image;
      else
        LogWarn("Immagine del preset non trovata: {}", ToUtf8(src.wstring()));
    }
    // Font del preset: cartella Fonts di RTSS (…\RivaTuner Statistics Server\Fonts) o accanto al preset.
    for (const auto& fontDir : {dir, dir / L".." / L".." / L".." / L"Fonts", dir / L"Fonts"}) {
      for (const auto& e : fs::directory_iterator(fontDir, ec)) {
        const auto ext = ToLowerAscii(ToUtf8(e.path().extension().wstring()));
        if (ext == ".ttf" || ext == ".otf")
          fs::copy_file(e.path(), PresetsDir() / L"fonts" / e.path().filename(), fs::copy_options::overwrite_existing,
                        ec);
      }
    }
  }
  r.ok = true;
  LogInfo("Preset RTSS importato: {} ({} layer, {} sorgenti, {} non supportate)", r.layout.name,
          r.layout.layers.size(), r.layout.sources.size(), r.unsupported.size());
  return r;
}

}  // namespace po
