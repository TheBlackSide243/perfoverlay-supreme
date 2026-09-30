// Anteprima di un preset RTSS (.ovx/.ovl) con valori di esempio, per controllare posizioni,
// font e colori senza avviare PerfOverlaySupreme.exe (che richiede l'amministratore).
//   rtss-preview.exe <preset> [secondi] [dimensione font]
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <string>

#include "common/rtss_preset.h"
#include "common/util.h"
#include "monitor/overlay_window.h"
#include "monitor/rtss_engine.h"
#include "monitor/rtss_text.h"

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    fwprintf(stderr, L"uso: rtss-preview <preset.ovx|.ovl> [secondi] [dimensione font]\n");
    return 2;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto r = po::ImportRtssPreset(argv[1]);
  if (!r.ok) {
    printf("errore: %s\n", r.error.c_str());
    return 1;
  }
  printf("preset %s: font %s, %zu layer, %zu sorgenti, avanzato=%d, immagine=%s, zoom=%d\n", r.layout.name.c_str(),
         r.layout.fontFace.c_str(), r.layout.layers.size(), r.layout.defs.size(), int(r.layout.advanced),
         r.layout.image.c_str(), r.layout.zoomRatio);

  // Valori di esempio realistici.
  po::SystemSnapshot s;
  s.ready = true;
  s.cpu.name = "AMD Ryzen 7 9800X3D";
  s.cpu.usage = 32;
  s.cpu.freqGHz = 5.19;
  s.cpuTempC = 68;
  s.cpuPowerW = 54;
  s.gpu.name = "NVIDIA GeForce RTX 3080";
  s.gpu.vendorId = 0x10DE;
  s.gpu.usage = 97;
  s.gpu.tempC = 71;
  s.gpu.clockMHz = 1905;
  s.gpu.powerW = 312;
  s.gpu.vramUsedGB = 7.9;
  s.gpu.vramTotalGB = 10;
  s.ram.usedGB = 16.8;
  s.ram.totalGB = 31.1;
  auto sensors = std::make_shared<po::SensorList>();
  for (int c = 0; c < 16; ++c) {
    sensors->push_back({std::format("cpu:core{}:usage", c), "", "", "%", 20.0 + (c * 37 % 70), 0, 100});
    sensors->push_back({std::format("cpu:core{}:clock", c), "", "", "MHz", 4800.0 + c * 20, 0, 0});
  }
  sensors->push_back({"nv:0:fan0", "", "", "%", 54, 0, 100});
  s.sensors = sensors;
  po::FrameStats f;
  f.valid = true;
  f.fps = 144;
  f.fpsAvg = 141;
  f.frameTimeMs = 6.9;
  f.api = "DXGI";
  for (int i = 0; i < 600; ++i) f.frameTimes.push_back(float(6.9 + 0.6 * std::sin(i * 0.37) + (i % 97 == 0 ? 6 : 0)));

  po::Profile p;
  p.position = "top-left";
  if (argc >= 4) p.fontSize = float(_wtof(argv[3]));
  po::OverlayWindow ow;
  if (!ow.Create(GetModuleHandleW(nullptr))) return 1;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);

  po::RtssEngine engine;
  if (r.layout.advanced) engine.Load(r.layout);
  auto render = [&] {
    if (r.layout.advanced) {
      engine.Update(s, &f);
      ow.RenderRtssAdvanced(engine, p, mi.rcWork);
    } else {
      std::vector<po::RtssDrawLayer> layers;
      for (const auto& l : r.layout.layers)
        layers.push_back({po::ExpandRtssText(l.text, r.layout, s, &f), l.x, l.y, l.extentX, l.extentY, l.origin,
                          l.size, l.color});
      ow.RenderRtss(layers, r.layout, p, mi.rcWork);
    }
  };
  render();
  ow.Show(true);

  const DWORD until = GetTickCount() + DWORD(argc >= 3 ? _wtoi(argv[2]) : 8) * 1000;
  MSG msg;
  while (GetTickCount() < until) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(100);
    render();  // animazioni e timer
  }
  return 0;
}
