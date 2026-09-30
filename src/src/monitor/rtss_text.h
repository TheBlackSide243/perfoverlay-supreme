#pragma once
#include <optional>
#include <string>

#include "common/config.h"
#include "monitor/frame_source.h"
#include "monitor/stats.h"

namespace po {

// Valore di un dato di PerfOverlay usato da una sorgente RTSS (nullopt = non disponibile).
std::optional<double> MetricValue(const std::string& key, const SystemSnapshot& s, const FrameStats* f);

// Testo di un layer RTSS con le macro sostituite: %Nome sorgente%, %Time12%, %Time24%, %Date%.
std::wstring ExpandRtssText(const std::string& text, const RtssLayout& preset, const SystemSnapshot& s,
                            const FrameStats* f);

}  // namespace po
