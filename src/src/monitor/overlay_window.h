#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"


namespace po {
class RtssEngine;
}

namespace po {

struct Segment {
  enum class Kind { Text, Graph } kind = Kind::Text;
  std::wstring text;
  Color color;
  float scale = 1.0f;  // rispetto alla dimensione font del profilo
  bool bold = false;
  std::vector<float> graph;  // solo Kind::Graph
};
using Row = std::vector<Segment>;

// Layer di un preset RTSS già pronto da disegnare (macro sostituite con i valori).
struct RtssDrawLayer {
  std::wstring text;
  int x = 0, y = 0, extentX = 0, extentY = 0, origin = 0, size = 100;
  Color color;
};

// Blocco del layout libero: coordinate sulla tela del formato scelto (alta 1080 px).
struct FreeBlock {
  Row row;
  int x = 0, y = 0;
  float scale = 1.0f;
};

// Finestra overlay trasparente, sempre in primo piano e "click-through", posizionata sopra
// l'area client del gioco. Disegno con D3D11 + DirectComposition (alpha per pixel) e
// Direct2D/DirectWrite per testo e grafici.
class OverlayWindow {
 public:
  ~OverlayWindow();
  bool Create(HINSTANCE inst);
  void Show(bool visible);
  bool Visible() const { return visible_; }
  void Render(const std::vector<Row>& rows, const Profile& profile, const RECT& anchor);
  // extra: righe aggiunte sotto il preset (sensori scelti nella finestra "Sensori di sistema").
  void RenderRtss(const std::vector<RtssDrawLayer>& layers, const RtssLayout& preset, const Profile& profile,
                  const RECT& anchor, const std::vector<Row>& extra = {});
  void RenderFree(const std::vector<FreeBlock>& blocks, const Profile& profile, const RECT& anchor,
                  int canvasWidth);
  // Preset RTSS avanzato (ipertesto dell'OverlayEditor: immagini, grafici, condizioni, formule).
  // extra: righe dei sensori scelti, sotto il preset.
  void RenderRtssAdvanced(RtssEngine& engine, const Profile& profile, const RECT& anchor,
                          const std::vector<Row>& extra = {});

 private:
  bool InitDevice();
  bool Prepare(const Profile& p);
  std::wstring ResolveRtssFont(const std::string& face);
  float UiScale(const Profile& p, const RECT& anchor) const;
  bool BeginFrame(int x, int y, UINT w, UINT h);
  void EndFrame();
  void ReleaseDevice();
  bool EnsureSize(UINT w, UINT h);
  IDWriteTextFormat* Format(float px, bool bold);

  HWND hwnd_ = nullptr;
  bool visible_ = false;
  UINT width_ = 64, height_ = 32;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;

  std::wstring family_;
  std::unordered_map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;

  // Preset avanzati
  IDWriteTextFormat* AdvFormat(const std::wstring& family, float px, bool bold);
  ID2D1Bitmap1* PresetImage(const std::wstring& file);
  Microsoft::WRL::ComPtr<IDWriteFontCollection> presetFonts_;  // font copiati in presetsonts
  bool presetFontsLoaded_ = false;
  std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextFormat>> advFormats_;
  std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> images_;
  Microsoft::WRL::ComPtr<ID2D1Effect> tint_;
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
};

}  // namespace po
