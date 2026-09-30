#include "monitor/rtss_text.h"

#include <windows.h>

#include <algorithm>
#include <format>

#include "common/util.h"

namespace po {

std::optional<double> MetricValue(const std::string& key, const SystemSnapshot& s, const FrameStats* f) {
  const bool frames = f && f->valid;
  if (key == "fps") return frames ? std::optional<double>(f->fps) : std::nullopt;
  if (key == "frametime") return frames ? std::optional<double>(f->frameTimeMs) : std::nullopt;
  if (key == "cpu.usage") return s.cpu.usage;
  if (key == "cpu.temp") return s.cpuTempC;
  if (key == "cpu.power") return s.cpuPowerW;
  if (key == "cpu.clock") return s.cpu.freqGHz ? std::optional<double>(*s.cpu.freqGHz * 1000.0) : std::nullopt;
  if (key == "gpu.usage") return s.gpu.usage;
  if (key == "gpu.temp") return s.gpu.tempC;
  if (key == "gpu.power") return s.gpu.powerW;
  if (key == "gpu.clock") return s.gpu.clockMHz;
  if (key == "gpu.vram_mb") return s.gpu.vramUsedGB ? std::optional<double>(*s.gpu.vramUsedGB * 1024.0) : std::nullopt;
  if (key == "gpu.vram_pct")
    return s.gpu.vramUsedGB && s.gpu.vramTotalGB && *s.gpu.vramTotalGB > 0
               ? std::optional<double>(100.0 * *s.gpu.vramUsedGB / *s.gpu.vramTotalGB)
               : std::nullopt;
  if (key == "ram.mb") return s.ram.totalGB > 0 ? std::optional<double>(s.ram.usedGB * 1024.0) : std::nullopt;
  if (key == "ram.pct") return s.ram.totalGB > 0 ? std::optional<double>(100.0 * s.ram.usedGB / s.ram.totalGB) : std::nullopt;
  if (key == "bat.pct")
    return s.battery.present && s.battery.percent >= 0 ? std::optional<double>(s.battery.percent) : std::nullopt;
  // "Charge Rate" (DRAIN): consumo della batteria; sui PC senza batteria mostra il consumo della GPU.
  if (key == "bat.rate_w") return s.battery.present ? s.battery.rateW : s.gpu.powerW;
  if (key == "bat.remain_min") return s.battery.present ? s.battery.remainMin : std::nullopt;
  return std::nullopt;
}

// "%%" dopo una macro resta un "%" letterale; i tag <..> di formattazione RTSS vengono ignorati.
std::wstring ExpandRtssText(const std::string& text, const RtssLayout& preset, const SystemSnapshot& s,
                            const FrameStats* f) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '<') {  // tag di formattazione RTSS (<C=..>, <S=..>, ...): ignorati
      const size_t close = text.find('>', i);
      if (close != std::string::npos && close - i < 24) {
        i = close + 1;
        continue;
      }
    }
    if (c == '%') {
      const size_t close = text.find('%', i + 1);
      if (close != std::string::npos) {
        const std::string name = text.substr(i + 1, close - i - 1);
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::string value;
        bool known = true;
        if (const auto it = preset.sources.find(name); it != preset.sources.end()) {
          const auto v = MetricValue(it->second.metric, s, f);
          value = v ? std::format("{:.{}f}", *v, std::clamp(it->second.decimals, 0, 3)) : "N/A";
        } else if (name == "Time12") {
          const int h12 = st.wHour % 12 == 0 ? 12 : st.wHour % 12;
          value = std::format("{}:{:02}:{:02} {}", h12, st.wMinute, st.wSecond, st.wHour < 12 ? "AM" : "PM");
        } else if (name == "Time24" || name == "Time") {
          value = std::format("{:02}:{:02}:{:02}", st.wHour, st.wMinute, st.wSecond);
        } else if (name == "Date") {
          value = std::format("{:02}/{:02}/{}", st.wDay, st.wMonth, st.wYear);
        } else {
          known = false;
        }
        if (known) {
          out += value;
          i = close + 1;
          continue;
        }
      }
    }
    out += c;
    ++i;
  }
  return ToWide(out);
}

}  // namespace po
