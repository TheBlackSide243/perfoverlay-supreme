#pragma once
#include <windows.h>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "monitor/frame_source.h"
#include "monitor/stats.h"

namespace po {

// Motore dei preset avanzati dell'OverlayEditor di RTSS (es. TroyMetrics): sorgenti con provider
// (HAL, HwInfo, PresentMon, MSI Afterburner) e formule, valutate a ogni aggiornamento, con uno storico
// per grafici, medie mobili e percentili.
class RtssEngine {
 public:
  struct Val {
    double v = 0;
    bool ok = false;
  };

  void Load(const RtssLayout& preset);
  bool Loaded() const { return loaded_; }
  const RtssLayout* Preset() const { return preset_; }
  // Aggiorna tutte le sorgenti con i dati attuali.
  void Update(const SystemSnapshot& sys, const FrameStats* frames);

  Val Value(const std::string& name);                        // sorgente per nome (o dato noto)
  std::wstring Formatted(const std::string& name);           // %Nome% nel testo: valore col Format della sorgente
  const std::deque<float>* History(const std::string& name); // per i grafici <G=...>
  bool Truthy(const std::string& condition);                  // <IF nome>, <IF !nome>, VisibilitySource
  bool Knows(const std::string& name);                        // sorgente del preset o dato noto (per %Nome%)
  double TimeMs() const { return nowMs_; }

  const SystemSnapshot& System() const { return sys_; }
  const FrameStats* Frames() const { return framesValid_ ? &frames_ : nullptr; }

  // Espressione (per formule e colori a soglie).
  struct Node;

 private:
  struct Source {
    RtssSourceDef def;
    std::shared_ptr<Node> formula;
    Val value;
    int evalStamp = -1;
    bool evaluating = false;
    Val previous;  // buf(x,-1)
    std::deque<float> history;
  };
  Val Evaluate(Source& s);
  Val Raw(const Source& s);
  Val Builtin(const std::string& name);
  Source* Find(const std::string& name);
  Val Eval(const Node& n, Source* self, const Val& x);
  Val Percentile(const std::string& name, double p);
  Val SlidingWindow(const std::string& name, int n, int mode);

  const RtssLayout* preset_ = nullptr;
  bool loaded_ = false;
  std::vector<Source> sources_;
  std::unordered_map<std::string, size_t> index_;  // nome minuscolo → sorgente
  int stamp_ = 0;
  double nowMs_ = 0;
  ULONGLONG startTick_ = 0;
  SystemSnapshot sys_;
  FrameStats frames_;
  bool framesValid_ = false;
  std::deque<float> frameTimes_;  // storico dei frame time (ms), anche tra un Query e l'altro
  std::unordered_map<std::string, std::deque<float>> builtinHistory_;
};

// Colore di un layer o di <C=...>: "AARRGGBB" / "RRGGBB" oppure con soglie
// "c1,min1,max1,c2,min2,max2,...(sorgente,flag)" (flag 1 = sfumatura tra i colori).
struct ArgbColor {
  float r = 1, g = 1, b = 1, a = 1;
};
ArgbColor ParseRtssColor(const std::string& spec, RtssEngine& engine, ArgbColor fallback);

}  // namespace po
