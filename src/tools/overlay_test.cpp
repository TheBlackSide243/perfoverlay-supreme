// PerfOverlay Test: finestra DirectX 11 / DirectX 12 per provare gli overlay (FPS, frame time, posizione,
// scala con la risoluzione). Disegna una scena animata con un carico GPU regolabile.
//
//   F1 = DirectX 11   F2 = DirectX 12   V = VSync   L = limite FPS (off/30/60/144)
//   Su/Giù = carico GPU   F11 o Alt+Invio = schermo intero senza bordi   Esc = esci
//   PerfOverlaySupreme.exe --test [dx11|dx12]
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>

#include "common/resources.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kShader[] = R"(
cbuffer C : register(b0) { float t; float w; float h; float iters; };
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target {
  float2 uv = (pos.xy - 0.5 * float2(w, h)) / h;
  // Plasma animato
  float v = sin(uv.x * 6 + t) + sin(uv.y * 5 - t * 1.3) + sin((uv.x + uv.y) * 4 + t * 0.7) +
            sin(length(uv * 8) - t * 2);
  float3 col = 0.5 + 0.5 * cos(v * 0.9 + float3(0, 2.1, 4.2) + t * 0.2);
  // Anelli che ruotano
  float a = atan2(uv.y, uv.x) + t * 0.5;
  float r = length(uv);
  col *= 0.75 + 0.25 * smoothstep(0.02, 0.0, abs(frac(r * 6 - t * 0.4) - 0.5) * sin(a * 6) * 0.3);
  // Carico GPU regolabile: calcolo inutile che il compilatore non può eliminare
  float2 q = uv;
  float acc = 0;
  [loop] for (int i = 0; i < (int)iters; ++i) {
    q = float2(sin(q.y * 1.7 + t), cos(q.x * 1.3 - t));
    acc += q.x * q.y;
  }
  col += acc * 1e-5;
  col *= 1.0 - 0.35 * dot(uv, uv);  // vignettatura
  return float4(col, 1);
}
)";

struct Consts {
  float t, w, h, iters;
};

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code, err;
  if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "overlay_test", nullptr, nullptr, entry, target,
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
    MessageBoxA(nullptr, err ? static_cast<const char*>(err->GetBufferPointer()) : "D3DCompile", "Shader", MB_ICONERROR);
    return nullptr;
  }
  return code;
}

bool TearingSupported(IDXGIFactory2* f) {
  ComPtr<IDXGIFactory5> f5;
  BOOL allow = FALSE;
  return SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f5))) &&
         SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;
}

std::wstring AdapterName(IUnknown* device) {
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> ad;
  DXGI_ADAPTER_DESC d{};
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&ad)) &&
      SUCCEEDED(ad->GetDesc(&d)))
    return d.Description;
  return L"";
}

class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual bool Init(HWND hwnd, UINT w, UINT h) = 0;
  virtual void Resize(UINT w, UINT h) = 0;
  virtual void Render(const Consts& c, bool vsync) = 0;
  virtual const wchar_t* Name() const = 0;
  std::wstring adapter;
};

// ------------------------------------------------------------------ DirectX 11
class Dx11 : public Renderer {
 public:
  const wchar_t* Name() const override { return L"DirectX 11"; }
  bool Init(HWND hwnd, UINT w, UINT h) override {
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev_,
                                 nullptr, &ctx_)))
      return false;
    adapter = AdapterName(dev_.Get());
    ComPtr<IDXGIFactory2> f;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    tearing_ = TearingSupported(f.Get());
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(f->CreateSwapChainForHwnd(dev_.Get(), hwnd, &d, nullptr, nullptr, &sc_))) return false;
    f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    auto vs = Compile("VS", "vs_5_0"), ps = Compile("PS", "ps_5_0");
    if (!vs || !ps) return false;
    dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{sizeof(Consts), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    dev_->CreateBuffer(&bd, nullptr, &cb_);
    w_ = w;
    h_ = h;
    CreateRtv();
    return true;
  }
  void Resize(UINT w, UINT h) override {
    rtv_.Reset();
    ctx_->ClearState();
    sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    w_ = w;
    h_ = h;
    CreateRtv();
  }
  void Render(const Consts& c, bool vsync) override {
    ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &c, 0, 0);
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->Draw(3, 0);
    sc_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
  }

 private:
  void CreateRtv() {
    ComPtr<ID3D11Texture2D> bb;
    sc_->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
  }
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain1> sc_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11Buffer> cb_;
  UINT w_ = 0, h_ = 0;
  bool tearing_ = false;
};

// ------------------------------------------------------------------ DirectX 12
class Dx12 : public Renderer {
 public:
  static constexpr UINT kFrames = 3;
  const wchar_t* Name() const override { return L"DirectX 12"; }
  ~Dx12() override {
    if (queue_) WaitIdle();
    if (event_) CloseHandle(event_);
  }
  bool Init(HWND hwnd, UINT w, UINT h) override {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) return false;
    adapter = AdapterName12();
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory2> f;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    tearing_ = TearingSupported(f.Get());
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kFrames;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(f->CreateSwapChainForHwnd(queue_.Get(), hwnd, &d, nullptr, nullptr, &sc1)) || FAILED(sc1.As(&sc_)))
      return false;
    f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrames};
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_));
    rtvSize_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto& a : alloc_) dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0].Get(), nullptr, IID_PPV_ARGS(&list_));
    list_->Close();
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Root signature: 4 costanti a 32 bit in b0.
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants = {0, 0, 4};
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{1, &rp};
    ComPtr<ID3DBlob> rsBlob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err)) ||
        FAILED(dev_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&root_))))
      return false;
    auto vs = Compile("VS", "vs_5_0"), ps = Compile("PS", "ps_5_0");
    if (!vs || !ps) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)))) return false;
    w_ = w;
    h_ = h;
    CreateRtvs();
    return true;
  }
  void Resize(UINT w, UINT h) override {
    WaitIdle();
    for (auto& b : buffers_) b.Reset();
    sc_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_UNKNOWN, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    w_ = w;
    h_ = h;
    CreateRtvs();
  }
  void Render(const Consts& c, bool vsync) override {
    const UINT i = sc_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < frameFence_[i]) {
      fence_->SetEventOnCompletion(frameFence_[i], event_);
      WaitForSingleObject(event_, INFINITE);
    }
    alloc_[i]->Reset();
    list_->Reset(alloc_[i].Get(), pso_.Get());
    Barrier(buffers_[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(i) * rtvSize_;
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
    const D3D12_RECT sr{0, 0, LONG(w_), LONG(h_)};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &sr);
    list_->SetGraphicsRootSignature(root_.Get());
    list_->SetGraphicsRoot32BitConstants(0, 4, &c, 0);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);
    Barrier(buffers_[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    sc_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    queue_->Signal(fence_.Get(), ++fenceValue_);
    frameFence_[i] = fenceValue_;
  }

 private:
  std::wstring AdapterName12() {
    const LUID luid = dev_->GetAdapterLuid();
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter1> ad;
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) &&
        SUCCEEDED(ad->GetDesc1(&d)))
      return d.Description;
    return L"";
  }
  void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list_->ResourceBarrier(1, &b);
  }
  void CreateRtvs() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; ++i) {
      sc_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i]));
      dev_->CreateRenderTargetView(buffers_[i].Get(), nullptr, h);
      h.ptr += rtvSize_;
    }
  }
  void WaitIdle() {
    queue_->Signal(fence_.Get(), ++fenceValue_);
    fence_->SetEventOnCompletion(fenceValue_, event_);
    WaitForSingleObject(event_, INFINITE);
  }
  ComPtr<ID3D12Device> dev_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> sc_;
  ComPtr<ID3D12DescriptorHeap> rtvHeap_;
  ComPtr<ID3D12Resource> buffers_[kFrames];
  ComPtr<ID3D12CommandAllocator> alloc_[kFrames];
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pso_;
  UINT64 fenceValue_ = 0, frameFence_[kFrames] = {};
  HANDLE event_ = nullptr;
  UINT rtvSize_ = 0, w_ = 0, h_ = 0;
  bool tearing_ = false;
};

// ------------------------------------------------------------------ app
struct App {
  HWND hwnd = nullptr;
  std::unique_ptr<Renderer> r;
  bool dx12 = true;
  bool vsync = false;
  int limitIdx = 0;
  int load = 2;  // 0..10: iterazioni del ciclo di carico = 64 * 2^load
  bool fullscreen = false;
  RECT windowed{};
  UINT w = 0, h = 0;
  bool resize = false;
} g;

constexpr int kLimits[] = {0, 30, 60, 144};

void ShowError(const wchar_t* what) {
  MessageBoxW(g.hwnd, std::format(L"Impossibile inizializzare {}.", what).c_str(), L"PerfOverlay Test", MB_ICONERROR);
}

bool CreateRenderer(bool dx12) {
  g.r.reset();  // prima libera la swap chain della finestra
  std::unique_ptr<Renderer> r = dx12 ? std::unique_ptr<Renderer>(new Dx12) : std::unique_ptr<Renderer>(new Dx11);
  if (!r->Init(g.hwnd, std::max(1u, g.w), std::max(1u, g.h))) {
    ShowError(r->Name());
    return false;
  }
  g.r = std::move(r);
  g.dx12 = dx12;
  return true;
}

void ToggleFullscreen() {
  g.fullscreen = !g.fullscreen;
  if (g.fullscreen) {
    GetWindowRect(g.hwnd, &g.windowed);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(g.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
  } else {
    SetWindowLongPtrW(g.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(g.hwnd, nullptr, g.windowed.left, g.windowed.top, g.windowed.right - g.windowed.left,
                 g.windowed.bottom - g.windowed.top, SWP_FRAMECHANGED | SWP_NOZORDER);
  }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      g.w = LOWORD(lp);
      g.h = HIWORD(lp);
      g.resize = true;
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN)) {
        ToggleFullscreen();
        return 0;
      }
      break;
    case WM_KEYDOWN:
      switch (wp) {
        case VK_ESCAPE: DestroyWindow(h); break;
        case VK_F1: if (g.dx12) CreateRenderer(false); break;
        case VK_F2: if (!g.dx12) CreateRenderer(true); break;
        case VK_F11: ToggleFullscreen(); break;
        case 'V': g.vsync = !g.vsync; break;
        case 'L': g.limitIdx = (g.limitIdx + 1) % int(std::size(kLimits)); break;
        case VK_UP: g.load = std::min(g.load + 1, 10); break;
        case VK_DOWN: g.load = std::max(g.load - 1, 0); break;
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace po {
// Modalità --test dell'exe unico: finestra di prova DirectX 11/12.
int RunOverlayTest(HINSTANCE inst, const wchar_t* cmd, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (cmd && (wcsstr(cmd, L"dx11") || wcsstr(cmd, L"DX11"))) g.dx12 = false;

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  wc.lpszClassName = L"PerfOverlayTestWindow";
  RegisterClassExW(&wc);

  // 1600x900 client, centrata sul monitor principale.
  RECT rc{0, 0, 1600, 900};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"PerfOverlay Test", WS_OVERLAPPEDWINDOW,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                           nullptr, nullptr, inst, nullptr);
  ShowWindow(g.hwnd, show);
  RECT cr;
  GetClientRect(g.hwnd, &cr);
  g.w = UINT(cr.right);
  g.h = UINT(cr.bottom);
  g.resize = false;
  if (!CreateRenderer(g.dx12) && !CreateRenderer(!g.dx12)) return 1;

  LARGE_INTEGER freq, start, last, titleAt;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  last = titleAt = start;
  int frames = 0;
  double nextFrame = 0;
  MSG msg{};
  while (msg.message != WM_QUIT) {
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      continue;
    }
    if (IsIconic(g.hwnd) || g.w == 0 || g.h == 0 || !g.r) {
      Sleep(50);
      continue;
    }
    if (g.resize) {
      g.r->Resize(g.w, g.h);
      g.resize = false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
    // Limite FPS: attesa fino al prossimo frame (Sleep grossolano + attesa attiva finale).
    if (const int lim = kLimits[g.limitIdx]; lim > 0) {
      if (nextFrame == 0 || t - nextFrame > 0.25) nextFrame = t;
      while (true) {
        QueryPerformanceCounter(&now);
        const double left = nextFrame - double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
        if (left <= 0) break;
        if (left > 0.002) Sleep(1);
      }
      nextFrame += 1.0 / lim;
    } else {
      nextFrame = 0;
    }

    const Consts c{float(t), float(g.w), float(g.h), float(64 << g.load)};
    g.r->Render(c, g.vsync);
    ++frames;

    QueryPerformanceCounter(&now);
    const double since = double(now.QuadPart - titleAt.QuadPart) / double(freq.QuadPart);
    if (since >= 0.5) {
      const int lim = kLimits[g.limitIdx];
      SetWindowTextW(g.hwnd,
                     std::format(L"PerfOverlay Test  ·  {}  ·  {} FPS  ·  {}x{}  ·  carico {}/10  ·  VSync {}  ·  "
                                 L"limite {}  ·  {}   [F1 DX11  F2 DX12  V  L  Su/Giù  F11  Esc]",
                                 g.r->Name(), int(std::lround(frames / since)), g.w, g.h, g.load,
                                 g.vsync ? L"ON" : L"OFF", lim ? std::to_wstring(lim) : std::wstring(L"off"),
                                 g.r->adapter)
                         .c_str());
      frames = 0;
      titleAt = now;
    }
    last = now;
  }
  g.r.reset();
  return 0;
}

}  // namespace po
