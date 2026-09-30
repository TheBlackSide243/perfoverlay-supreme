#include "monitor/overlay_content.h"

#include <format>
#include <utility>

#include "common/sensor_feed.h"
#include "common/util.h"
#include "common/i18n.h"

namespace po {
namespace {

constexpr Color kRed{0xFF, 0x4A, 0x4A};
constexpr Color kYellow{0xFF, 0xC8, 0x3C};

Color WithAlpha(Color c, uint8_t a) {
  c.a = a;
  return c;
}

class Builder {
 public:
  explicit Builder(const Profile& p) : p_(p), neon_(p.style == "neon") {}

  void Label(const std::wstring& t, Color c) { Add(t, c, 0.8f, true); }
  // Valore: colore testo, o colore dell'elemento nello stile neon.
  void Value(const std::wstring& t, Color element, float scale = 1.0f) { Add(t, neon_ ? element : p_.colors.text, scale); }
  void Unit(const std::wstring& t, Color element) { Value(t, element, 0.72f); }
  void Colored(const std::wstring& t, Color c, float scale = 1.0f, bool bold = false) { Add(t, c, scale, bold); }
  void Graph(const std::vector<float>& h, Color c, float scale) {
    Segment s;
    s.kind = Segment::Kind::Graph;
    s.graph = h;
    s.color = c;
    s.scale = scale;
    row_.push_back(std::move(s));
  }
  void Separator() {
    if (!row_.empty()) Add(L"  |  ", WithAlpha(p_.colors.text, 0x70), 0.8f);
  }
  void Space() { Add(L"  ", p_.colors.text, 0.8f); }
  bool Empty() const { return row_.empty(); }
  Row Take() { return std::exchange(row_, {}); }

 private:
  void Add(const std::wstring& t, Color c, float scale, bool bold = false) {
    Segment s;
    s.text = t;
    s.color = c;
    s.scale = scale;
    s.bold = bold;
    row_.push_back(std::move(s));
  }
  const Profile& p_;
  bool neon_;
  Row row_;
};

Color FpsColor(const Profile& p, double fps) {
  if (fps < p.fpsRedBelow) return kRed;
  if (fps < p.fpsYellowBelow) return kYellow;
  return p.style == "neon" ? p.colors.fps : p.colors.text;
}

std::wstring Pct(double v) { return std::format(L"{:.0f}%", v); }

// ---------------------------------------------------------------- gruppi
void AddFps(Builder& b, const ContentInput& in, bool vertical, bool includeGraph = true) {
  const Profile& p = *in.profile;
  const FrameStats& f = *in.frames;
  b.Label(L"FPS ", p.colors.fps);
  if (!f.valid) {
    b.Value(in.frameSourceAvailable ? L"—" : L"n/d", p.colors.fps, vertical ? 2.0f : 1.0f);
    return;
  }
  if (p.Field("fps", "value"))
    b.Colored(std::format(L"{:.0f}", f.fps), FpsColor(p, f.fps), vertical ? 2.0f : 1.0f, true);
  if (p.Field("fps", "minmax")) b.Value(std::format(L" ↓{:.0f} ↑{:.0f}", f.fpsMin, f.fpsMax), p.colors.fps, 0.7f);
  if (p.Field("fps", "low1") && f.p99Ms > 0) {  // 1% low = FPS equivalente al 99° percentile del frame time
    b.Label(L"  1%L ", p.colors.fps);
    b.Value(std::format(L"{:.0f}", 1000.0 / f.p99Ms), p.colors.fps, 0.85f);
  }
  if (includeGraph && p.show.graph && !f.history.empty()) {
    b.Space();
    b.Graph(f.history, p.colors.fps, vertical ? 1.8f : 1.0f);
  }
}

// block = layout libero: il blocco "frame time" è visibile di suo, conta solo il campo.
void AddFrameTiming(Builder& b, const ContentInput& in, bool block = false) {
  const Profile& p = *in.profile;
  const FrameStats& f = *in.frames;
  if (!f.valid) return;
  bool first = true;
  auto gap = [&] {
    if (!first) b.Space();
    first = false;
  };
  if (p.Field("frametime", "value") && (block || p.show.frametime)) {
    gap();
    b.Value(std::format(L"{:.1f}", f.frameTimeMs), p.colors.fps);
    b.Unit(L"ms", p.colors.fps);
  }
  const bool p95 = p.show.percentiles && p.Field("frametime", "p95");
  const bool p99 = p.show.percentiles && p.Field("frametime", "p99");
  if (p95 || p99) {
    gap();
    if (p95) {
      b.Label(L"P95 ", p.colors.fps);
      b.Value(std::format(L"{:.1f}", f.p95Ms), p.colors.fps);
    }
    if (p99) {
      b.Label(p95 ? L"  P99 " : L"P99 ", p.colors.fps);
      b.Value(std::format(L"{:.1f}", f.p99Ms), p.colors.fps);
    }
    b.Unit(L"ms", p.colors.fps);
  }
  if (p.show.stutter && p.Field("frametime", "stutter")) {
    gap();
    b.Label(L"ST ", p.colors.fps);
    b.Value(std::format(L"{:.1f}%", f.stutterPct), p.colors.fps);
  }
}

void AddCpu(Builder& b, const Profile& p, const SystemSnapshot& s, bool vertical) {
  const Color c = p.colors.cpu;
  b.Label(L"CPU ", c);
  const bool usage = p.Field("cpu", "usage");
  if (usage) b.Value(s.cpu.usage ? Pct(*s.cpu.usage) : L"—", c);
  if (p.Field("cpu", "topcore") && s.cpu.topCoreUsage) {
    if (vertical && s.cpu.topCore >= 0)
      b.Unit(std::format(L"{}core {} ", usage ? L"  " : L"", s.cpu.topCore), c);
    else
      b.Unit(usage ? L"/↑" : L"↑", c);
    b.Value(Pct(*s.cpu.topCoreUsage), c);
  }
  if (p.Field("cpu", "temp") && s.cpuTempC) {
    b.Value(std::format(L" {:.0f}", *s.cpuTempC), c);
    b.Unit(L"°C", c);
  }
  const bool freq = p.Field("cpu", "freq") && s.cpu.freqGHz;
  const bool maxFreq = p.Field("cpu", "maxfreq") && s.cpu.maxFreqGHz;
  if (freq) b.Value(std::format(L" {:.2f}", *s.cpu.freqGHz), c);
  if (maxFreq) b.Value(std::format(L"{}↑{:.2f}", freq ? L"/" : L" ", *s.cpu.maxFreqGHz), c, freq ? 0.85f : 1.0f);
  if (freq || maxFreq) b.Unit(L"GHz", c);
}

void AddGpu(Builder& b, const Profile& p, const SystemSnapshot& s) {
  const Color c = p.colors.gpu;
  b.Label(L"GPU ", c);
  if (p.Field("gpu", "usage")) b.Value(s.gpu.usage ? Pct(*s.gpu.usage) : L"—", c);
  if (p.Field("gpu", "temp") && s.gpu.tempC) {
    b.Value(std::format(L" {:.0f}", *s.gpu.tempC), c);
    b.Unit(L"°C", c);
  }
  if (p.Field("gpu", "clock") && s.gpu.clockMHz) {
    b.Value(std::format(L" {:.0f}", *s.gpu.clockMHz), c);
    b.Unit(L"MHz", c);
  }
  if (p.Field("gpu", "vram") && s.gpu.vramUsedGB && s.gpu.vramTotalGB) {
    b.Value(std::format(L" {:.1f}/{:.1f}", *s.gpu.vramUsedGB, *s.gpu.vramTotalGB), c);
    b.Unit(L"GB", c);
  }
}

void AddRam(Builder& b, const Profile& p, const SystemSnapshot& s, bool vertical) {
  const Color c = p.colors.ram;
  b.Label(L"RAM ", c);
  const bool used = p.Field("ram", "used");
  if (used) {
    b.Value(std::format(L"{:.1f}/{:.1f}", s.ram.usedGB, s.ram.totalGB), c);
    b.Unit(L"GB", c);
  }
  if (vertical && p.Field("ram", "speed") && s.ram.speedMTs > 0) {
    b.Value(std::format(L"{}{}", used ? L" " : L"", s.ram.speedMTs), c);
    b.Unit(L"MT/s", c);
  }
}

void AddBattery(Builder& b, const Profile& p, const SystemSnapshot& s) {
  const Color c = p.colors.battery;
  b.Label(L"BAT ", c);
  if (p.Field("battery", "percent")) b.Value(s.battery.percent >= 0 ? Pct(s.battery.percent) : L"—", c);
  if (p.Field("battery", "state")) b.Unit(s.battery.charging ? T(L" ▲ in carica") : T(L" ▼ a batteria"), c);
}

// Sensore scelto dall'utente: etichetta nel colore della sua categoria (CPU, GPU, ...), valore e unità.
void AddSensor(Builder& b, const Profile& p, const SystemSnapshot& s, const SensorPick& sp) {
  SensorEntry probe;
  probe.id = sp.id;
  probe.group = sp.group;
  const std::string cat = SensorCategory(probe);
  const Color c = cat == "cpu"       ? p.colors.cpu
                  : cat == "gpu"     ? p.colors.gpu
                  : cat == "ram"     ? p.colors.ram
                  : cat == "battery" ? p.colors.battery
                                     : p.colors.text;
  b.Label(ToWide(sp.label.empty() ? sp.id : sp.label) + L" ", c);
  const SensorEntry* e = FindSensor(s, sp.id);
  if (!e) {
    b.Value(s.sensors ? L"N/A" : L"—", c);
    return;
  }
  b.Value(FormatSensorValue(e->value, e->unit, false), c);
  if (!e->unit.empty()) b.Unit(ToWide(e->unit), c);
}

}  // namespace

std::vector<Row> BuildSensorRows(const ContentInput& in) {
  std::vector<Row> rows;
  if (in.fpsOnly || !in.system->ready) return rows;
  Builder b(*in.profile);
  for (const auto& sp : in.profile->sensors) {
    AddSensor(b, *in.profile, *in.system, sp);
    rows.push_back(b.Take());
  }
  return rows;
}

Row BuildElementRow(const std::string& element, const ContentInput& in) {
  const Profile& p = *in.profile;
  const bool game = in.frames != nullptr;
  const SystemSnapshot& s = *in.system;
  Builder b(p);
  if (element == "fps") {
    if (game && p.show.fps) AddFps(b, in, false, false);
  } else if (element == "graph") {
    if (game && p.show.graph && in.frames->valid && !in.frames->history.empty())
      b.Graph(in.frames->history, p.colors.fps, 1.6f);
  } else if (in.fpsOnly || !s.ready) {
    // modalità solo FPS: nient'altro
  } else if (element == "frametime") {
    if (game && in.frames->valid && p.show.frametime) AddFrameTiming(b, in, true);
  } else if (element == "cpu") {
    if (p.show.cpu) AddCpu(b, p, s, false);
  } else if (element == "gpu") {
    if (p.show.gpu) AddGpu(b, p, s);
  } else if (element == "ram") {
    if (p.show.ram) AddRam(b, p, s, true);
  } else if (element == "battery") {
    if (p.show.battery && s.battery.present) AddBattery(b, p, s);
  } else if (IsSensorElement(element)) {
    const std::string id = element.substr(kSensorElementPrefix.size());
    for (const auto& sp : p.sensors)
      if (sp.id == id) AddSensor(b, p, s, sp);
  }
  return b.Take();
}

std::vector<Row> BuildRows(const ContentInput& in) {
  std::vector<Row> rows;
  const Profile& p = *in.profile;
  const bool vertical = p.layout == "vertical";
  const bool game = in.frames != nullptr;
  const SystemSnapshot& s = *in.system;
  Builder b(p);

  if (!in.toast.empty()) {
    b.Colored(in.toast, p.colors.text, 0.8f);
    rows.push_back(b.Take());
  }

  const bool showFps = game && p.show.fps;
  const bool showTiming = game && !in.fpsOnly && (p.show.frametime || p.show.percentiles || p.show.stutter);
  const bool showSystem = !in.fpsOnly && s.ready;

  if (vertical) {
    if (showFps) {
      AddFps(b, in, true);
      rows.push_back(b.Take());
    }
    if (showTiming) {
      AddFrameTiming(b, in);
      if (!b.Empty()) rows.push_back(b.Take());
    }
    if (showSystem) {
      if (p.show.cpu) {
        AddCpu(b, p, s, true);
        rows.push_back(b.Take());
      }
      if (p.show.gpu) {
        AddGpu(b, p, s);
        rows.push_back(b.Take());
      }
      if (p.show.ram) {
        AddRam(b, p, s, true);
        rows.push_back(b.Take());
      }
      if (p.show.battery && s.battery.present) {
        AddBattery(b, p, s);
        rows.push_back(b.Take());
      }
      for (const auto& sp : p.sensors) {
        AddSensor(b, p, s, sp);
        rows.push_back(b.Take());
      }
    }
    return rows;
  }

  // Barra orizzontale compatta (stile della barra Steam).
  if (showFps) AddFps(b, in, false);
  if (showTiming && in.frames->valid) {
    if (!b.Empty()) b.Space();
    AddFrameTiming(b, in);
  }
  if (showSystem) {
    if (p.show.cpu) {
      b.Separator();
      AddCpu(b, p, s, false);
    }
    if (p.show.gpu) {
      b.Separator();
      AddGpu(b, p, s);
    }
    if (p.show.ram) {
      b.Separator();
      AddRam(b, p, s, false);
    }
    if (p.show.battery && s.battery.present) {
      b.Separator();
      AddBattery(b, p, s);
    }
    for (const auto& sp : p.sensors) {
      b.Separator();
      AddSensor(b, p, s, sp);
    }
  }
  if (!b.Empty()) rows.push_back(b.Take());
  return rows;
}

}  // namespace po
