#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Registrazioni delle prestazioni: i benchmark avviati con la hotkey e il riepilogo di ogni partita.
// Ogni registrazione è un file JSON in %APPDATA%\PerfOverlay Supreme\benchmarks; i benchmark hanno
// accanto un CSV con il tempo di ogni frame, apribile con un foglio di calcolo.
namespace po {

struct RunSummary {
  std::string id;     // nome del file senza estensione
  std::string kind;   // "benchmark" | "session"
  std::string game;   // exe in minuscolo
  std::string title;  // titolo della finestra
  std::string api;    // DXGI | D3D9 | DxgKrnl
  std::string time;   // inizio, ora locale "2026-11-21 21:14:03"
  std::string gpuName, cpuName;
  int width = 0, height = 0;

  double seconds = 0;  // tempo di gioco misurato (somma dei frame time)
  uint64_t frames = 0;
  double fpsAvg = 0, fpsMin = 0, fpsMax = 0;  // min e max su intervalli di un secondo
  double low1 = 0, low01 = 0;                 // FPS al 99° e al 99,9° percentile del frame time
  double p95Ms = 0, p99Ms = 0, frameTimeMaxMs = 0;
  double stutterPct = 0;                      // frame più lunghi del doppio della mediana

  // Sensori: 0 = dato non disponibile.
  double cpuAvg = 0, cpuMax = 0, gpuAvg = 0, gpuMax = 0;
  double cpuTempMax = 0, gpuTempMax = 0, gpuPowerAvg = 0, ramMaxGB = 0, vramMaxGB = 0;

  int step = 1;                      // secondi tra un punto e l'altro delle serie
  std::vector<float> fps, cpu, gpu;  // una voce per intervallo

  bool IsBenchmark() const { return kind == "benchmark"; }
  std::string DisplayName() const;  // titolo della finestra, o il nome dell'exe se manca
};

struct RunData {
  RunSummary s;
  std::vector<float> frameTimes;  // ms, solo nei benchmark
};

struct SensorSample {
  double cpu = -1, gpu = -1;  // %, -1 = assente
  double cpuTemp = -1, gpuTemp = -1, gpuPower = -1, ramGB = -1, vramGB = -1;
};

// Accumula i frame di un processo e ne ricava il riepilogo. Non tiene tutti i frame in memoria
// (una partita può durare ore): usa un istogramma dei frame time, più l'elenco completo solo nei benchmark.
class RunRecorder {
 public:
  struct Options {
    bool keepFrames = false;  // conserva ogni frame time (benchmark)
    int step = 1;             // secondi per punto delle serie
    double maxFrameMs = 0;    // frame più lunghi sono pause o caricamenti e non si contano (0 = conta tutto)
  };
  void Start(const Options& o, RunSummary meta, int64_t qpcFreq, int64_t qpcNow);
  bool Active() const { return active_; }
  // Il gioco non è in primo piano: i frame presentati fino a "qpcNow" non contano e il prossimo non fa intervallo.
  void SkipTo(int64_t qpcNow) {
    prev_ = 0;
    if (qpcNow > last_) last_ = qpcNow;
  }
  void SetWindow(const std::string& title, int width, int height);  // titolo e risoluzione più recenti
  void SetApi(const std::string& api) {
    if (!api.empty()) meta_.api = api;
  }
  void AddFrames(const std::vector<int64_t>& qpcTimestamps);
  void AddSample(const SensorSample& s);
  double Seconds() const { return sumMs_ / 1000.0; }
  uint64_t Frames() const { return frames_; }
  int64_t LastTimestamp() const { return last_; }  // ultimo evento visto: i successivi sono nuovi
  const RunSummary& Meta() const { return meta_; }
  RunData Finish();
  void Cancel() { active_ = false; }

 private:
  static constexpr double kBucketMs = 0.05;
  static constexpr size_t kBuckets = 20000;  // fino a 1 s; oltre finisce nell'ultimo
  double Percentile(double q) const;

  bool active_ = false;
  Options opt_;
  RunSummary meta_;
  int64_t freq_ = 1, prev_ = 0, last_ = 0;
  uint64_t frames_ = 0;
  double sumMs_ = 0, maxMs_ = 0;
  std::vector<uint32_t> hist_;
  std::vector<float> frameTimes_;
  double secMs_ = 0, stepMs_ = 0;
  uint32_t secFrames_ = 0, stepFrames_ = 0;
  double fpsMin_ = 0, fpsMax_ = 0;
  bool anySecond_ = false;
  // Sensori: somme per le medie, massimi, e le somme dell'intervallo in corso per le serie.
  double cpuSum_ = 0, gpuSum_ = 0, powerSum_ = 0;
  uint32_t cpuN_ = 0, gpuN_ = 0, powerN_ = 0;
  double stepCpu_ = 0, stepGpu_ = 0;
  uint32_t stepCpuN_ = 0, stepGpuN_ = 0;
};

std::filesystem::path BenchmarksDir();  // creata se manca
std::filesystem::path RunJsonPath(const std::string& id);
std::filesystem::path RunCsvPath(const std::string& id);
std::string MakeRunId(const RunSummary& s);        // dalla data e dal nome del gioco
std::string LocalTimeString();                     // "2026-11-21 21:14:03"
bool SaveRun(const RunData& run);                  // JSON, più il CSV dei frame se ci sono
std::vector<RunSummary> ListRuns();                // dalla più recente
std::vector<float> LoadFrameTimes(const std::string& id);  // dal CSV; vuoto se non c'è
bool DeleteRun(const std::string& id);
void PruneSessions(size_t keep);                   // tiene solo i riepiloghi di partita più recenti

}  // namespace po
