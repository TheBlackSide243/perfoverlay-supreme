#include "monitor/bottleneck.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace po {
namespace {

constexpr double kGpuEnter = 92.0, kGpuLeave = 85.0;  // % di utilizzo: entra sopra 92, esce sotto 85
constexpr int kRepeatsToSwitch = 3;                   // letture uguali di fila prima di cambiare

bool Near(double value, double target, double tolerance) {
  return target > 0 && std::fabs(value - target) <= target * tolerance;
}

// Valori su cui di solito si impostano i limitatori (giochi, driver, RTSS) e quelli legati al monitor.
bool NearCommonCap(double fps, double refreshHz, double tolerance) {
  static constexpr double kCaps[] = {24, 30, 40, 45, 48, 50, 60, 72, 75, 80, 90, 100, 110, 120, 141, 144,
                                     160, 165, 170, 175, 180, 200, 225, 237, 240, 300, 360, 480};
  for (double c : kCaps)
    if (Near(fps, c, tolerance)) return true;
  if (refreshHz > 0) {
    // Frequenza del monitor, qualche FPS sotto (limite consigliato con G-Sync / FreeSync) e la sua metà.
    for (double c : {refreshHz, refreshHz - 3, refreshHz - 4, refreshHz / 2})
      if (Near(fps, c, tolerance)) return true;
  }
  return false;
}

}  // namespace

Limit ClassifyLimit(const LimitInput& in, Limit previous) {
  if (!in.valid || in.fps <= 0) return Limit::Unknown;
  if (in.gpuUsage >= (previous == Limit::Gpu ? kGpuLeave : kGpuEnter)) return Limit::Gpu;
  // V-Sync acceso e FPS pari alla frequenza del monitor o a un suo divisore intero.
  if (in.syncInterval >= 1 && in.refreshHz > 0) {
    for (int k = 1; k <= 4; ++k)
      if (Near(in.fps, in.refreshHz / k, 0.03)) return Limit::VSync;
  }
  // Un core al massimo: è il processore a far aspettare la scheda video.
  if (in.cpuTopCore >= 90.0) return Limit::Cpu;
  // Un limitatore si riconosce da frame regolarissimi, oppure da FPS fermi su un valore tipico: quelli che
  // aspettano con una pausa (molti giochi) hanno frame irregolari ma una media che non si muove.
  if (in.frameCv < 0.03) return Limit::Cap;
  if (in.frameCv < 0.08 && NearCommonCap(in.fps, in.refreshHz, 0.015)) return Limit::Cap;
  if (in.fpsSpread < 0.025 && NearCommonCap(in.fps, in.refreshHz, 0.025)) return Limit::Cap;
  // Altrimenti la GPU ha margine e i frame arrivano quando il processore (o il gioco) li finisce.
  return Limit::Cpu;
}

Limit LimitDetector::Update(const LimitInput& input) {
  LimitInput in = input;
  if (in.gpuUsage >= 0) {  // media mobile: una lettura bassa isolata non cambia il verdetto
    gpuSmooth_ = gpuSmooth_ < 0 ? in.gpuUsage : gpuSmooth_ * 0.6 + in.gpuUsage * 0.4;
    in.gpuUsage = gpuSmooth_;
  }
  if (in.valid && in.fps > 0) {  // quanto si sono mossi gli FPS nelle ultime letture
    fps_[fpsNext_] = in.fps;
    fpsNext_ = (fpsNext_ + 1) % kFpsHistory;
    fpsCount_ = std::min(fpsCount_ + 1, kFpsHistory);
    if (fpsCount_ == kFpsHistory) {
      double lo = fps_[0], hi = fps_[0], sum = 0;
      for (double f : fps_) {
        lo = std::min(lo, f);
        hi = std::max(hi, f);
        sum += f;
      }
      in.fpsSpread = (hi - lo) / (sum / kFpsHistory);
    }
  } else {
    fpsCount_ = fpsNext_ = 0;
  }
  const Limit now = ClassifyLimit(in, current_);
  if (now == current_) {
    repeats_ = 0;
    candidate_ = now;
    return current_;
  }
  if (now == Limit::Unknown) {  // il gioco si è fermato: subito, senza aspettare
    current_ = candidate_ = now;
    repeats_ = 0;
    gpuSmooth_ = -1;
    return current_;
  }
  if (now != candidate_) {
    candidate_ = now;
    repeats_ = 1;
  } else {
    ++repeats_;
  }
  if (repeats_ >= kRepeatsToSwitch || current_ == Limit::Unknown) {
    current_ = now;
    repeats_ = 0;
  }
  return current_;
}

}  // namespace po
