#include "common/benchmark.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/config.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/util.h"

namespace po {
namespace {

using nlohmann::json;

template <class T>
void Read(const json& j, const char* key, T& dst) {
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  try {
    dst = it->get<T>();
  } catch (const json::exception&) {  // tipo sbagliato in un file modificato a mano: resta il valore di prima
  }
}

// unit = 0.1, 0.01, 0.001: si divide per l'inverso, così 66.1 resta 66.1 e non 66.10000000000001.
double Round(double v, double unit) {
  const double scale = std::round(1.0 / unit);
  return std::round(v * scale) / scale;
}

json Series(const std::vector<float>& v) {
  json a = json::array();
  for (float f : v) a.push_back(Round(f, 0.1));
  return a;
}

json ToJson(const RunSummary& s) {
  return {{"version", 1},
          {"kind", s.kind},
          {"game", s.game},
          {"title", s.title},
          {"api", s.api},
          {"time", s.time},
          {"gpuName", s.gpuName},
          {"cpuName", s.cpuName},
          {"width", s.width},
          {"height", s.height},
          {"seconds", Round(s.seconds, 0.01)},
          {"frames", s.frames},
          {"fpsAvg", Round(s.fpsAvg, 0.01)},
          {"fpsMin", Round(s.fpsMin, 0.01)},
          {"fpsMax", Round(s.fpsMax, 0.01)},
          {"low1", Round(s.low1, 0.01)},
          {"low01", Round(s.low01, 0.01)},
          {"p95Ms", Round(s.p95Ms, 0.001)},
          {"p99Ms", Round(s.p99Ms, 0.001)},
          {"frameTimeMaxMs", Round(s.frameTimeMaxMs, 0.001)},
          {"stutterPct", Round(s.stutterPct, 0.001)},
          {"cpuAvg", Round(s.cpuAvg, 0.1)},
          {"cpuMax", Round(s.cpuMax, 0.1)},
          {"gpuAvg", Round(s.gpuAvg, 0.1)},
          {"gpuMax", Round(s.gpuMax, 0.1)},
          {"cpuTempMax", Round(s.cpuTempMax, 0.1)},
          {"gpuTempMax", Round(s.gpuTempMax, 0.1)},
          {"gpuPowerAvg", Round(s.gpuPowerAvg, 0.1)},
          {"ramMaxGB", Round(s.ramMaxGB, 0.01)},
          {"vramMaxGB", Round(s.vramMaxGB, 0.01)},
          {"step", s.step},
          {"fps", Series(s.fps)},
          {"cpu", Series(s.cpu)},
          {"gpu", Series(s.gpu)}};
}

RunSummary FromJson(const json& j) {
  RunSummary s;
  Read(j, "kind", s.kind);
  Read(j, "game", s.game);
  Read(j, "title", s.title);
  Read(j, "api", s.api);
  Read(j, "time", s.time);
  Read(j, "gpuName", s.gpuName);
  Read(j, "cpuName", s.cpuName);
  Read(j, "width", s.width);
  Read(j, "height", s.height);
  Read(j, "seconds", s.seconds);
  Read(j, "frames", s.frames);
  Read(j, "fpsAvg", s.fpsAvg);
  Read(j, "fpsMin", s.fpsMin);
  Read(j, "fpsMax", s.fpsMax);
  Read(j, "low1", s.low1);
  Read(j, "low01", s.low01);
  Read(j, "p95Ms", s.p95Ms);
  Read(j, "p99Ms", s.p99Ms);
  Read(j, "frameTimeMaxMs", s.frameTimeMaxMs);
  Read(j, "stutterPct", s.stutterPct);
  Read(j, "cpuAvg", s.cpuAvg);
  Read(j, "cpuMax", s.cpuMax);
  Read(j, "gpuAvg", s.gpuAvg);
  Read(j, "gpuMax", s.gpuMax);
  Read(j, "cpuTempMax", s.cpuTempMax);
  Read(j, "gpuTempMax", s.gpuTempMax);
  Read(j, "gpuPowerAvg", s.gpuPowerAvg);
  Read(j, "ramMaxGB", s.ramMaxGB);
  Read(j, "vramMaxGB", s.vramMaxGB);
  Read(j, "step", s.step);
  Read(j, "fps", s.fps);
  Read(j, "cpu", s.cpu);
  Read(j, "gpu", s.gpu);
  if (s.kind != "benchmark") s.kind = "session";
  s.step = std::max(1, s.step);
  return s;
}

}  // namespace

std::string RunSummary::DisplayName() const {
  if (!title.empty()) return title;
  std::string name = game;
  if (name.size() > 4 && IEquals(name.substr(name.size() - 4), ".exe")) name.resize(name.size() - 4);
  return name.empty() ? "?" : name;
}

// ---------------------------------------------------------------- registratore
void RunRecorder::Start(const Options& o, RunSummary meta, int64_t qpcFreq, int64_t qpcNow) {
  *this = RunRecorder();
  active_ = true;
  opt_ = o;
  opt_.step = std::max(1, o.step);
  meta_ = std::move(meta);
  meta_.step = opt_.step;
  freq_ = std::max<int64_t>(1, qpcFreq);
  last_ = qpcNow;
  hist_.assign(kBuckets, 0);
}

void RunRecorder::AddFrames(const std::vector<int64_t>& ts) {
  if (!active_) return;
  for (const int64_t t : ts) {
    if (t <= last_) continue;
    last_ = t;
    if (!prev_) {  // primo frame, o primo dopo una pausa: nessun intervallo da misurare
      prev_ = t;
      continue;
    }
    const double ms = double(t - prev_) * 1000.0 / double(freq_);
    prev_ = t;
    if (opt_.maxFrameMs > 0 && ms > opt_.maxFrameMs) continue;
    ++frames_;
    sumMs_ += ms;
    maxMs_ = std::max(maxMs_, ms);
    ++hist_[std::min(kBuckets - 1, size_t(ms / kBucketMs))];
    if (opt_.keepFrames) frameTimes_.push_back(float(ms));

    secMs_ += ms;
    ++secFrames_;
    if (secMs_ >= 1000.0) {
      const double fps = secFrames_ * 1000.0 / secMs_;
      fpsMin_ = anySecond_ ? std::min(fpsMin_, fps) : fps;
      fpsMax_ = anySecond_ ? std::max(fpsMax_, fps) : fps;
      anySecond_ = true;
      secMs_ = 0;
      secFrames_ = 0;
    }
    stepMs_ += ms;
    ++stepFrames_;
    if (stepMs_ >= opt_.step * 1000.0) {
      meta_.fps.push_back(float(stepFrames_ * 1000.0 / stepMs_));
      meta_.cpu.push_back(stepCpuN_ ? float(stepCpu_ / stepCpuN_) : 0.0f);
      meta_.gpu.push_back(stepGpuN_ ? float(stepGpu_ / stepGpuN_) : 0.0f);
      stepMs_ = stepCpu_ = stepGpu_ = 0;
      stepFrames_ = stepCpuN_ = stepGpuN_ = 0;
    }
  }
}

void RunRecorder::SetWindow(const std::string& title, int width, int height) {
  if (!title.empty()) meta_.title = title;
  if (width > 0 && height > 0) {
    meta_.width = width;
    meta_.height = height;
  }
}

void RunRecorder::AddSample(const SensorSample& s) {
  if (!active_ || !prev_) return;  // in pausa i sensori non descrivono il gioco
  if (s.cpu >= 0) {
    cpuSum_ += s.cpu;
    ++cpuN_;
    stepCpu_ += s.cpu;
    ++stepCpuN_;
    meta_.cpuMax = std::max(meta_.cpuMax, s.cpu);
  }
  if (s.gpu >= 0) {
    gpuSum_ += s.gpu;
    ++gpuN_;
    stepGpu_ += s.gpu;
    ++stepGpuN_;
    meta_.gpuMax = std::max(meta_.gpuMax, s.gpu);
  }
  if (s.gpuPower >= 0) {
    powerSum_ += s.gpuPower;
    ++powerN_;
  }
  meta_.cpuTempMax = std::max(meta_.cpuTempMax, s.cpuTemp);
  meta_.gpuTempMax = std::max(meta_.gpuTempMax, s.gpuTemp);
  meta_.ramMaxGB = std::max(meta_.ramMaxGB, s.ramGB);
  meta_.vramMaxGB = std::max(meta_.vramMaxGB, s.vramGB);
}

// Frame time sotto cui sta la frazione q dei frame (dall'istogramma: preciso al ventesimo di millisecondo).
double RunRecorder::Percentile(double q) const {
  if (!frames_) return 0;
  const uint64_t target = std::max<uint64_t>(1, uint64_t(std::ceil(q * double(frames_))));
  uint64_t seen = 0;
  for (size_t i = 0; i < kBuckets; ++i) {
    seen += hist_[i];
    if (seen >= target) return i + 1 == kBuckets ? maxMs_ : std::min(maxMs_, double(i + 1) * kBucketMs);
  }
  return maxMs_;
}

RunData RunRecorder::Finish() {
  RunData out;
  active_ = false;
  RunSummary& s = meta_;
  s.seconds = sumMs_ / 1000.0;
  s.frames = frames_;
  if (frames_ && sumMs_ > 0) {
    s.fpsAvg = double(frames_) * 1000.0 / sumMs_;
    s.fpsMin = anySecond_ ? fpsMin_ : s.fpsAvg;
    s.fpsMax = anySecond_ ? fpsMax_ : s.fpsAvg;
    double p999 = 0, median = 0;
    if (opt_.keepFrames && !frameTimes_.empty()) {  // valori esatti dai frame registrati
      std::vector<float> sorted = frameTimes_;
      std::sort(sorted.begin(), sorted.end());
      auto pct = [&](double q) {
        return double(sorted[std::min(sorted.size() - 1, size_t(std::ceil(q * double(sorted.size()))) - 1)]);
      };
      s.p95Ms = pct(0.95);
      s.p99Ms = pct(0.99);
      p999 = pct(0.999);
      median = sorted[sorted.size() / 2];
    } else {
      s.p95Ms = Percentile(0.95);
      s.p99Ms = Percentile(0.99);
      p999 = Percentile(0.999);
      median = Percentile(0.5);
    }
    s.low1 = s.p99Ms > 0 ? 1000.0 / s.p99Ms : 0;
    s.low01 = p999 > 0 ? 1000.0 / p999 : 0;
    s.frameTimeMaxMs = maxMs_;
    uint64_t stutters = 0;
    if (opt_.keepFrames && !frameTimes_.empty()) {
      for (const float ms : frameTimes_) stutters += ms > 2.0 * median;
    } else {
      for (size_t i = size_t(2.0 * median / kBucketMs) + 1; i < kBuckets; ++i) stutters += hist_[i];
    }
    s.stutterPct = 100.0 * double(stutters) / double(frames_);
  }
  // Ultimo intervallo incompleto: vale come punto se copre almeno metà del passo.
  if (stepFrames_ && stepMs_ >= opt_.step * 500.0) {
    s.fps.push_back(float(stepFrames_ * 1000.0 / stepMs_));
    s.cpu.push_back(stepCpuN_ ? float(stepCpu_ / stepCpuN_) : 0.0f);
    s.gpu.push_back(stepGpuN_ ? float(stepGpu_ / stepGpuN_) : 0.0f);
  }
  s.cpuAvg = cpuN_ ? cpuSum_ / cpuN_ : 0;
  s.gpuAvg = gpuN_ ? gpuSum_ / gpuN_ : 0;
  s.gpuPowerAvg = powerN_ ? powerSum_ / powerN_ : 0;
  out.s = std::move(meta_);
  out.frameTimes = std::move(frameTimes_);
  return out;
}

// ---------------------------------------------------------------- file
std::filesystem::path BenchmarksDir() {
  const auto dir = AppDataDir() / L"benchmarks";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path RunJsonPath(const std::string& id) { return BenchmarksDir() / (ToWide(id) + L".json"); }
std::filesystem::path RunCsvPath(const std::string& id) { return BenchmarksDir() / (ToWide(id) + L".csv"); }

std::string LocalTimeString() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// "2026-11-21 21:14:03" + "cyberpunk2077.exe" → "20261121-211403-b-cyberpunk2077"
std::string MakeRunId(const RunSummary& s) {
  std::string id;
  for (char c : s.time) {
    if (c >= '0' && c <= '9') id += c;
    if (c == ' ') id += '-';
  }
  id += s.IsBenchmark() ? "-b-" : "-s-";
  std::string name = s.game;
  if (name.size() > 4 && IEquals(name.substr(name.size() - 4), ".exe")) name.resize(name.size() - 4);
  size_t n = 0;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (n++ < 40) id += ok ? c : '_';
  }
  if (name.empty()) id += "gioco";
  return id;
}

bool SaveRun(const RunData& run) {
  if (run.s.id.empty()) return false;
  if (!WriteJsonFile(RunJsonPath(run.s.id), ToJson(run.s))) return false;
  if (run.frameTimes.empty()) return true;
  std::ofstream f(RunCsvPath(run.s.id), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  std::string buf = "frame,time_s,frametime_ms\n";
  buf.reserve(run.frameTimes.size() * 24 + 64);
  double t = 0;
  size_t n = 0;
  for (float ms : run.frameTimes) {
    t += ms;
    std::format_to(std::back_inserter(buf), "{},{:.4f},{:.3f}\n", ++n, t / 1000.0, ms);
  }
  f.write(buf.data(), std::streamsize(buf.size()));
  return bool(f);
}

std::vector<RunSummary> ListRuns() {
  std::vector<RunSummary> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(BenchmarksDir(), ec)) {
    if (!e.is_regular_file(ec) || !IEquals(ToUtf8(e.path().extension().wstring()), ".json")) continue;
    const auto j = ReadJsonFile(e.path());
    if (!j || !j->is_object()) continue;
    RunSummary s = FromJson(*j);
    s.id = ToUtf8(e.path().stem().wstring());
    out.push_back(std::move(s));
  }
  // L'id comincia con data e ora: l'ordine alfabetico inverso è dal più recente.
  std::sort(out.begin(), out.end(), [](const RunSummary& a, const RunSummary& b) { return a.id > b.id; });
  return out;
}

std::vector<float> LoadFrameTimes(const std::string& id) {
  std::vector<float> out;
  std::ifstream f(RunCsvPath(id), std::ios::binary);
  if (!f) return out;
  std::string line;
  std::getline(f, line);  // intestazione
  while (std::getline(f, line)) {
    const size_t comma = line.rfind(',');
    if (comma == std::string::npos) continue;
    try {
      out.push_back(std::stof(line.substr(comma + 1)));
    } catch (...) {  // riga rovinata: si salta
    }
  }
  return out;
}

bool DeleteRun(const std::string& id) {
  if (id.empty()) return false;
  std::error_code ec;
  std::filesystem::remove(RunCsvPath(id), ec);
  return std::filesystem::remove(RunJsonPath(id), ec);
}

void PruneSessions(size_t keep) {
  size_t sessions = 0;
  for (const auto& s : ListRuns()) {
    if (s.IsBenchmark()) continue;
    if (++sessions > keep) DeleteRun(s.id);
  }
}

}  // namespace po
