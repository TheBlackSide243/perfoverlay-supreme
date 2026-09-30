cmake_minimum_required(VERSION 3.21)
project(PerfOverlaySupreme VERSION 1.0.0 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "PerfOverlay richiede MSVC (Visual Studio 2022 o Build Tools).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Solo build x64. Usa: cmake -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /MP)

# --- libreria comune: config, profili, log, percorsi ---------------------
add_library(po_common STATIC
  src/common/util.cpp
  src/common/log.cpp
  src/common/paths.cpp
  src/common/config.cpp
  src/common/winutil.cpp
  src/common/darkmode.cpp
  src/common/rtss_preset.cpp
  src/common/sensor_feed.cpp
  src/common/lhm.cpp
  src/common/i18n.cpp)
target_include_directories(po_common PUBLIC src)
target_link_libraries(po_common PUBLIC nlohmann_json::nlohmann_json shell32 ole32 advapi32 user32 dwmapi uxtheme urlmon wininet)

# --- un solo eseguibile: overlay, impostazioni (--settings) e prova DirectX (--test) -------------
add_executable(PerfOverlay WIN32
  src/monitor/main.cpp
  src/monitor/app.cpp
  src/monitor/frame_source.cpp
  src/monitor/game_detector.cpp
  src/monitor/overlay_window.cpp
  src/monitor/overlay_content.cpp
  src/monitor/rtss_text.cpp
  src/monitor/rtss_engine.cpp
  src/monitor/rtss_render.cpp
  src/monitor/stats.cpp
  src/monitor/data/cpu.cpp
  src/monitor/data/gpu.cpp
  src/monitor/data/memory.cpp
  src/monitor/data/battery.cpp
  src/monitor/data/sensors.cpp
  src/monitor/data/system_sensors.cpp
  src/monitor/data/nvml.cpp
  src/monitor/data/wmi.cpp
  src/settings/main.cpp
  src/settings/settings_window.cpp
  src/settings/theme.cpp
  src/settings/layout_editor.cpp
  src/settings/sensor_browser.cpp
  src/settings/games_editor.cpp
  src/settings/gallery.cpp
  tools/overlay_test.cpp
  src/common/resources.rc)
# asInvoker: impostazioni e prova girano senza privilegi; l'overlay si rilancia da solo come amministratore
# (serve per gli eventi ETW da cui arrivano gli FPS).
target_link_libraries(PerfOverlay PRIVATE po_common
  d3d11 d3d12 dxgi d2d1 dwrite dcomp d3dcompiler pdh powrprof wbemuuid oleaut32 gdi32 comctl32 comdlg32 psapi urlmon wininet winhttp)

# Nomi degli eseguibili distinti da PerfOverlay: le due app possono convivere.
set_target_properties(PerfOverlay PROPERTIES OUTPUT_NAME PerfOverlaySupreme)


# Anteprima di un preset RTSS con valori di esempio (strumento di sviluppo, non incluso nella build normale):
#   cmake --build build --config Release --target rtss-preview
add_executable(rtss-preview EXCLUDE_FROM_ALL
  tools/rtss_preview.cpp
  src/monitor/overlay_window.cpp
  src/monitor/rtss_text.cpp
  src/monitor/rtss_engine.cpp
  src/monitor/rtss_render.cpp
  src/monitor/stats.cpp
  src/monitor/data/cpu.cpp
  src/monitor/data/gpu.cpp
  src/monitor/data/memory.cpp
  src/monitor/data/battery.cpp
  src/monitor/data/sensors.cpp
  src/monitor/data/system_sensors.cpp
  src/monitor/data/nvml.cpp
  src/monitor/data/wmi.cpp)
target_link_libraries(rtss-preview PRIVATE po_common d3d11 dxgi d2d1 dwrite dcomp pdh powrprof wbemuuid oleaut32 psapi winhttp)
