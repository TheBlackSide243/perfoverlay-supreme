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

constexpr int kKeepSeconds = 310;  // copre il massimo di graphSeconds (300)

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
  LogInfo("ETW: provider DXGI={} D3D9={} DxgKrnl={}", dxgi, d3d9, krnl);
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
    if (id == 42 || id == 55) self->Add(pid, t, "DXGI", true);
  } else if (IsEqualGUID(h.ProviderId, kD3D9Provider)) {
    if (id == 1) self->Add(pid, t, "D3D9", true);
  } else if (IsEqualGUID(h.ProviderId, kDxgKrnlProvider)) {
    if (id == 184) self->Add(pid, t, "DxgKrnl", false);
  }
}

void FrameSource::Add(DWORD pid, int64_t t, const char* api, bool fromApi) {
  std::lock_guard lock(mu_);
  auto& p = procs_[pid];
  if (fromApi) {
    p.lastApiTs = t;
    p.api = api;
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

FrameStats FrameSource::Query(DWORD pid, int windowSeconds) const {
  FrameStats s;
  std::vector<int64_t> ts;
  {
    std::lock_guard lock(mu_);
    const auto it = procs_.find(pid);
    if (it == procs_.end() || it->second.ts.size() < 2) return s;
    ts.assign(it->second.ts.begin(), it->second.ts.end());
    s.api = it->second.api;
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
  double lastSecSum = 0;
  int lastSecCount = 0;
  for (size_t i = first; i < ts.size(); ++i) {
    const double dt = double(ts[i] - ts[i - 1]) * toMs;
    dts.push_back(dt);
    if (ts[i] > last - freq_) {
      lastSecSum += dt;
      ++lastSecCount;
    }
  }
  if (dts.empty() || lastSecCount == 0) return s;

  s.frameTimeMs = lastSecSum / lastSecCount;
  s.fps = s.frameTimeMs > 0 ? 1000.0 / s.frameTimeMs : 0;

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
