#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace po {

struct FrameStats {
  bool valid = false;   // il processo sta presentando frame
  double fps = 0;       // ultimo secondo
  double fpsMin = 0, fpsMax = 0;  // sulla finestra del grafico
  double frameTimeMs = 0;
  double p95Ms = 0, p99Ms = 0;
  double stutterPct = 0;          // frame > 2× mediana
  std::vector<float> history;     // FPS per intervalli di 0,5 s, dal più vecchio al più recente
  std::string api;                // "DXGI" (DX10-12), "D3D9", "DxgKrnl" (Vulkan/OpenGL e altri)
  std::vector<float> frameTimes;  // ultimi frame time in ms (max 1000), dal più vecchio (preset RTSS avanzati)
  double fpsAvg = 0;              // media sulla finestra del grafico
};

// Conta i frame di tutti i processi tramite una sessione ETW in tempo reale
// (stesso principio di PresentMon): eventi Present di DXGI, D3D9 e del kernel grafico.
// Nessun codice viene caricato nei processi dei giochi.
// Richiede privilegi di amministratore o l'appartenenza al gruppo "Performance Log Users".
class FrameSource {
 public:
  ~FrameSource() { Stop(); }
  bool Start();
  void Stop();
  bool Running() const { return running_; }

  FrameStats Query(DWORD pid, int windowSeconds) const;
  double PresentRate(DWORD pid) const;  // frame nell'ultimo secondo (per l'euristica giochi)

 private:
  struct Proc {
    std::deque<int64_t> ts;  // timestamp QPC dei Present
    int64_t lastApiTs = 0;   // ultimo evento DXGI/D3D9 (per scartare i duplicati del kernel)
    std::string api;
  };
  static VOID WINAPI OnEvent(PEVENT_RECORD rec);
  void Add(DWORD pid, int64_t t, const char* api, bool fromApi);
  bool EnableProvider(const GUID& guid, std::initializer_list<USHORT> ids);
  void StopSession();

  TRACEHANDLE session_ = 0;
  TRACEHANDLE trace_ = INVALID_PROCESSTRACE_HANDLE;
  std::thread thread_;
  std::atomic<bool> running_{false};
  int64_t freq_ = 1;

  mutable std::mutex mu_;
  std::unordered_map<DWORD, Proc> procs_;
};

}  // namespace po
