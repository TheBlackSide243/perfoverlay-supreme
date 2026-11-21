#pragma once

// Che cosa sta limitando gli FPS del gioco in primo piano. È una stima ricavata da dati già disponibili
// (utilizzo della GPU, regolarità dei frame, V-Sync), senza toccare il gioco.
namespace po {

enum class Limit {
  Unknown,  // nessun dato (gioco fermo, FPS non disponibili)
  Gpu,      // la scheda video è al massimo: è lei a fissare gli FPS
  Cpu,      // la GPU ha margine e i frame non sono regolati da un limite: aspetta il processore (o il gioco)
  Cap,      // un limitatore di FPS tiene i frame a un ritmo fisso
  VSync,    // i frame seguono la frequenza del monitor
};

struct LimitInput {
  bool valid = false;      // il gioco sta presentando frame
  double fps = 0;
  double frameCv = 0;      // deviazione standard dei frame time / media, nell'ultimo secondo
  double gpuUsage = -1;    // % della scheda su cui gira il gioco; -1 = sconosciuto
  int syncInterval = -1;   // 0 = V-Sync spento; -1 = sconosciuto
  double refreshHz = 0;    // frequenza del monitor del gioco; 0 = sconosciuta
  double cpuTopCore = -1;  // % del core più carico; -1 = sconosciuto
  double fpsSpread = 1;    // (massimo - minimo) / media degli FPS negli ultimi secondi; lo calcola LimitDetector
};

// Classificazione di un singolo istante. "previous" serve per l'isteresi sulla soglia della GPU.
Limit ClassifyLimit(const LimitInput& in, Limit previous);

// Stabilizza la classificazione nel tempo: cambia solo quando la nuova si ripete per alcune letture di fila,
// così l'indicatore non sfarfalla quando il gioco è vicino a una soglia.
class LimitDetector {
 public:
  Limit Update(const LimitInput& in);
  Limit Current() const { return current_; }
  void Reset() { *this = LimitDetector(); }

 private:
  Limit current_ = Limit::Unknown;
  Limit candidate_ = Limit::Unknown;
  int repeats_ = 0;
  double gpuSmooth_ = -1;
  static constexpr int kFpsHistory = 6;  // ultime letture degli FPS (circa tre secondi)
  double fps_[kFpsHistory] = {};
  int fpsCount_ = 0, fpsNext_ = 0;
};

}  // namespace po
