#include "monitor/frame_source.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/log.h"

namespace po {
namespace {

constexpr wchar_t kSessionName[] = L"PerfOverlaySupreme-FrameTrace";

// FlushTimer espresso in millisecondi (Windows 8+); non sempre esposto dagli header dell'SDK.
#ifndef EVENT_TRACE_USE_MS_FLUSH_TIMER
#define EVENT_TRACE_USE_MS_FLUSH_TIMER 0x00000010
#endif

// Microsoft-Windows-DXGI: Present_Start (42), PresentMultiplaneOverlay_Start (55)
constexpr GUID kDxgiProvider = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
// Microsoft-Windows-D3D9: Present_Start (1)
constexpr GUID kD3D9Provider = {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};
// Microsoft-Windows-DxgKrnl: Present (184) — copre le API che non passano da DXGI (es. Vulkan, OpenGL)
constexpr GUID kDxgKrnlProvider = {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};

// Intel-PresentMon: PresentFrameType_Info (1). Lo emettono i driver e gli SDK che generano frame (AMD FSR e AFMF,
// Intel XeSS-FG) sul thread che sta per presentare, per dire se quel frame è del gioco o generato.
// Payload: FrameId (u32), FrameType (u8), dalla versione 1 anche AppFrameId (u32).
constexpr GUID kIntelPresentMonProvider = {0xECAA4712, 0x4644, 0x442F, {0xB9, 0x4C, 0xA3, 0x2F, 0x6C, 0xF8, 0xA4, 0x99}};
constexpr uint8_t kFrameOriginal = 1, kFrameIntelXefg = 50, kFrameAmdAfmf = 100, kFrameAmdFsrFg = 101;
constexpr uint32_t kPresentAllowTearing = 0x200;  // DXGI_PRESENT_ALLOW_TEARING

constexpr int kKeepSeconds = 310;  // copre il massimo di graphSeconds (300)
constexpr int kKeepFrameTypeSeconds = 5;

std::vector<BYTE> MakeProperties() {
  const size_t size = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName);
  std::vector<BYTE> buf(size, 0);
  auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
  p->Wnode.BufferSize = ULONG(size);
  p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  p->Wnode.ClientContext = 1;  // timestamp QPC
  p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_USE_MS_FLUSH_TIMER;
  p->FlushTimer = 200;         // ms: latenza massima di consegna degli eventi
  p->BufferSize = 16;          // KB
  p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  return buf;
}

}  // namespace

bool FrameSource::Start() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  freq_ = f.QuadPart;

  // Una sessione rimasta aperta da un'esecuzione precedente (crash) va chiusa prima.
  auto props = MakeProperties();
  ControlTraceW(0, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()), EVENT_TRACE_CONTROL_STOP);

  props = MakeProperties();
  const ULONG st = StartTraceW(&session_, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()));
  if (st != ERROR_SUCCESS) {
    if (st == ERROR_ACCESS_DENIED)
      LogWarn("ETW: accesso negato. FPS non disponibili: avvia come amministratore o aggiungi l'utente al gruppo "
              "'Performance Log Users'.");
    else
      LogError("ETW: StartTrace fallito ({})", st);
    session_ = 0;
    return false;
  }

  const bool dxgi = EnableProvider(kDxgiProvider, {42, 55});
  const bool d3d9 = EnableProvider(kD3D9Provider, {1});
  const bool krnl = EnableProvider(kDxgKrnlProvider, {184});
  const bool frameType = EnableProvider(kIntelPresentMonProvider, {1});
  LogInfo("ETW: provider DXGI={} D3D9={} DxgKrnl={} FrameType={}", dxgi, d3d9, krnl, frameType);
  if (!dxgi && !krnl) {
    StopSession();
    return false;
  }

  EVENT_TRACE_LOGFILEW lf{};
  lf.LoggerName = const_cast<LPWSTR>(kSessionName);
  lf.ProcessTraceMode =
      PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  lf.EventRecordCallback = &FrameSource::OnEvent;
  lf.Context = this;
  trace_ = OpenTraceW(&lf);
  if (trace_ == INVALID_PROCESSTRACE_HANDLE) {
    LogError("ETW: OpenTrace fallito ({})", GetLastError());
    StopSession();
    return false;
  }

  running_ = true;
  thread_ = std::thread([this] {
    const ULONG r = ProcessTrace(&trace_, 1, nullptr, nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_CANCELLED) LogWarn("ETW: ProcessTrace terminato ({})", r);
    running_ = false;
  });
  LogInfo("ETW: sessione frame avviata");
  return true;
}

bool FrameSource::EnableProvider(const GUID& guid, std::initializer_list<USHORT> ids) {
  // Filtro per ID evento lato ETW: arrivano solo gli eventi Present, non tutto il provider.
  const size_t size = offsetof(EVENT_FILTER_EVENT_ID, Events) + ids.size() * sizeof(USHORT);
  std::vector<BYTE> filterBuf(size, 0);
  auto* filter = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filterBuf.data());
  filter->FilterIn = TRUE;
  filter->Count = USHORT(ids.size());
  std::copy(ids.begin(), ids.end(), filter->Events);

  EVENT_FILTER_DESCRIPTOR desc{};
  desc.Ptr = reinterpret_cast<ULONGLONG>(filter);
  desc.Size = ULONG(size);
  desc.Type = EVENT_FILTER_TYPE_EVENT_ID;

  ENABLE_TRACE_PARAMETERS params{};
  params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
  params.EnableFilterDesc = &desc;
  params.FilterDescCount = 1;

  constexpr ULONGLONG kAllKeywords = ~0ULL;
  ULONG st = EnableTraceEx2(session_, &guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, kAllKeywords,
                            0, 0, &params);
  if (st != ERROR_SUCCESS) {
    st = EnableTraceEx2(session_, &guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, kAllKeywords, 0, 0,
                        nullptr);
  }
  return st == ERROR_SUCCESS;
}

void FrameSource::StopSession() {
  if (!session_) return;
  auto props = MakeProperties();
  ControlTraceW(session_, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()), EVENT_TRACE_CONTROL_STOP);
  session_ = 0;
}

void FrameSource::Stop() {
  StopSession();  // fa terminare ProcessTrace
  if (trace_ != INVALID_PROCESSTRACE_HANDLE) {
    CloseTrace(trace_);
    trace_ = INVALID_PROCESSTRACE_HANDLE;
  }
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

VOID WINAPI FrameSource::OnEvent(PEVENT_RECORD rec) {
  auto* self = static_cast<FrameSource*>(rec->UserContext);
  const auto& h = rec->EventHeader;
  const USHORT id = h.EventDescriptor.Id;
  const DWORD pid = h.ProcessId;
  if (pid == 0 || pid == 4) return;
  const int64_t t = h.TimeStamp.QuadPart;
  if (IsEqualGUID(h.ProviderId, kDxgiProvider)) {
    if (id == 42 || id == 55) {
      // Payload: pIDXGISwapChain (puntatore del processo che presenta), Flags (u32), SyncInterval (i32).
      int sync = -1;
      uint32_t flags = 0;
      const size_t ptr = (h.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
      if (rec->UserData && rec->UserDataLength >= ptr + 8) {
        const BYTE* d = static_cast<const BYTE*>(rec->UserData) + ptr;
        std::memcpy(&flags, d, 4);
        std::memcpy(&sync, d + 4, 4);
        if (sync < 0 || sync > 4) sync = -1;
      }
      self->Add(pid, t, "DXGI", true, sync, flags);
    }
  } else if (IsEqualGUID(h.ProviderId, kIntelPresentMonProvider)) {
    if (id == 1 && rec->UserData && rec->UserDataLength >= 5)
      self->AddFrameType(pid, t, static_cast<const BYTE*>(rec->UserData)[4]);
  } else if (IsEqualGUID(h.ProviderId, kD3D9Provider)) {
    if (id == 1) self->Add(pid, t, "D3D9", true);
  } else if (IsEqualGUID(h.ProviderId, kDxgKrnlProvider)) {
    if (id == 184) self->Add(pid, t, "DxgKrnl", false);
  }
}

void FrameSource::AddFrameType(DWORD pid, int64_t t, uint8_t type) {
  const bool generated = type == kFrameIntelXefg || type == kFrameAmdAfmf || type == kFrameAmdFsrFg;
  if (!generated && type != kFrameOriginal) return;  // frame ripetuti o tipi non noti: non contano
  std::lock_guard lock(mu_);
  auto& p = procs_[pid];
  auto& list = generated ? p.generated : p.original;
  if (generated) p.fgType = type;
  list.push_back(t);
  const int64_t oldest = t - int64_t(kKeepFrameTypeSeconds) * freq_;
  for (auto* l : {&p.generated, &p.original})
    while (!l->empty() && l->front() < oldest) l->pop_front();
}

void FrameSource::Add(DWORD pid, int64_t t, const char* api, bool fromApi, int sync, uint32_t flags) {
  std::lock_guard lock(mu_);
  auto& p = procs_[pid];
  if (fromApi) {
    p.lastApiTs = t;
    p.api = api;
    p.sync = sync;
    p.tearing = (flags & kPresentAllowTearing) != 0;
  } else if (p.lastApiTs && t - p.lastApiTs < 2 * freq_) {
    return;  // lo stesso frame è già stato contato dall'evento DXGI/D3D9
  } else {
    p.api = api;
  }
  p.ts.push_back(t);
  const int64_t oldest = t - int64_t(kKeepSeconds) * freq_;
  while (!p.ts.empty() && p.ts.front() < oldest) p.ts.pop_front();

  // Pulizia periodica dei processi che non presentano più.
  if (procs_.size() > 64) {
    std::erase_if(procs_, [&](const auto& kv) { return kv.second.ts.empty() || kv.second.ts.back() < t - 30 * freq_; });
  }
}

double FrameSource::PresentRate(DWORD pid) const {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  std::lock_guard lock(mu_);
  const auto it = procs_.find(pid);
  if (it == procs_.end()) return 0;
  const auto& ts = it->second.ts;
  // Riferimento: ultimo evento ricevuto (gli eventi arrivano con ~200 ms di ritardo).
  if (ts.empty() || now.QuadPart - ts.back() > 2 * freq_) return 0;
  const int64_t from = ts.back() - freq_;
  return double(ts.end() - std::upper_bound(ts.begin(), ts.end(), from));
}

std::vector<int64_t> FrameSource::Since(DWORD pid, int64_t after) const {
  std::lock_guard lock(mu_);
  const auto it = procs_.find(pid);
  if (it == procs_.end()) return {};
  const auto& ts = it->second.ts;
  return {std::upper_bound(ts.begin(), ts.end(), after), ts.end()};
}

std::string FrameSource::Api(DWORD pid) const {
  std::lock_guard lock(mu_);
  const auto it = procs_.find(pid);
  return it == procs_.end() ? std::string() : it->second.api;
}

FrameStats FrameSource::Query(DWORD pid, int windowSeconds) const {
  FrameStats s;
  std::vector<int64_t> ts;
  double generated = 0, original = 0;  // annunci nell'ultimo secondo
  uint8_t fgType = 0;
  {
    std::lock_guard lock(mu_);
    const auto it = procs_.find(pid);
    if (it == procs_.end() || it->second.ts.size() < 2) return s;
    const Proc& p = it->second;
    ts.assign(p.ts.begin(), p.ts.end());
    s.api = p.api;
    s.syncInterval = p.sync;
    s.tearing = p.tearing;
    const int64_t from = p.ts.back() - freq_;
    generated = double(p.generated.end() - std::upper_bound(p.generated.begin(), p.generated.end(), from));
    original = double(p.original.end() - std::upper_bound(p.original.begin(), p.original.end(), from));
    fgType = p.fgType;
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const int64_t last = ts.back();
  if (now.QuadPart - last > 2 * freq_) return s;  // gioco in pausa, caricamento o non più in primo piano

  const double toMs = 1000.0 / double(freq_);
  const int64_t windowStart = last - int64_t(windowSeconds) * freq_;
  const size_t first = std::max<size_t>(1, size_t(std::lower_bound(ts.begin(), ts.end(), windowStart) - ts.begin()));

  // Frame time sulla finestra e sull'ultimo secondo.
  std::vector<double> dts;
  dts.reserve(ts.size() - first);
  double lastSecSum = 0, lastSecSq = 0;
  int lastSecCount = 0;
  for (size_t i = first; i < ts.size(); ++i) {
    const double dt = double(ts[i] - ts[i - 1]) * toMs;
    dts.push_back(dt);
    if (ts[i] > last - freq_) {
      lastSecSum += dt;
      lastSecSq += dt * dt;
      ++lastSecCount;
    }
  }
  if (dts.empty() || lastSecCount == 0) return s;

  s.frameTimeMs = lastSecSum / lastSecCount;
  s.fps = s.frameTimeMs > 0 ? 1000.0 / s.frameTimeMs : 0;
  if (s.frameTimeMs > 0)
    s.frameCv = std::sqrt(std::max(0.0, lastSecSq / lastSecCount - s.frameTimeMs * s.frameTimeMs)) / s.frameTimeMs;

  if (generated > 0) {
    s.fpsGenerated = generated;
    if (fgType == kFrameAmdAfmf) {
      // AFMF lavora nel driver: i frame generati non passano dai Present del gioco, quindi si sommano.
      s.fgTech = "AMD AFMF";
      s.fpsReal = s.fps;
      s.fpsDisplayed = s.fps + generated;
    } else {
      // FSR e XeSS-FG presentano dal gioco sia i frame veri sia quelli generati: i Present li contano già tutti.
      s.fgTech = fgType == kFrameIntelXefg ? "Intel XeSS-FG" : "AMD FSR";
      s.fpsDisplayed = s.fps;
      s.fpsReal = original > 0 ? original : std::max(0.0, s.fps - generated);
    }
  }

  {
    double sum = 0;
    for (double d : dts) sum += d;
    s.fpsAvg = sum > 0 ? 1000.0 * double(dts.size()) / sum : 0;
    const size_t keep = std::min<size_t>(dts.size(), 1000);
    s.frameTimes.reserve(keep);
    for (auto it = dts.end() - ptrdiff_t(keep); it != dts.end(); ++it) s.frameTimes.push_back(float(*it));
  }
  std::vector<double> sorted = dts;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double q) { return sorted[std::min(sorted.size() - 1, size_t(std::ceil(q * sorted.size())) - 1)]; };
  s.p95Ms = pct(0.95);
  s.p99Ms = pct(0.99);
  const double median = sorted[sorted.size() / 2];
  const auto stutters = std::count_if(dts.begin(), dts.end(), [&](double d) { return d > 2.0 * median; });
  s.stutterPct = 100.0 * double(stutters) / double(dts.size());

  // Storico: intervalli da 0,5 s che terminano sull'ultimo frame.
  const int bins = windowSeconds * 2;
  const int64_t binTicks = freq_ / 2;
  const int64_t histStart = last - int64_t(bins) * binTicks;
  s.history.assign(size_t(bins), 0.0f);
  for (auto t : ts) {
    if (t <= histStart) continue;
    const int64_t b = std::min<int64_t>(bins - 1, (t - histStart - 1) / binTicks);
    s.history[size_t(b)] += 2.0f;  // frame per 0,5 s → FPS
  }
  const int firstBin = ts.front() > histStart ? int((ts.front() - histStart) / binTicks) + 1 : 0;
  bool any = false;
  for (int b = firstBin; b < bins; ++b) {
    const double v = s.history[size_t(b)];
    s.fpsMin = any ? std::min(s.fpsMin, v) : v;
    s.fpsMax = any ? std::max(s.fpsMax, v) : v;
    any = true;
  }
  if (!any) s.fpsMin = s.fpsMax = s.fps;
  s.valid = true;
  return s;
}

}  // namespace po
