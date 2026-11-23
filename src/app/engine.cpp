#include "engine.h"

#include "../core/knock.h"
#include "../core/snowball.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <dxgi1_3.h>
#include <mmsystem.h>

#if defined(__has_include)
#if __has_include(<d2d1_3.h>)
#include <d2d1_3.h>
#define SNOW_HAS_SPRITE_BATCH 1
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../core/actors.h"
#include "../core/motion.h"
#include "../core/pacing.h"
#include "../core/shared_stats.h"
#include "../core/theme.h"
#include "../core/world.h"
#include "gfx_common.h"
#include "gfx_decor.h"

namespace snow::win {

using gfx::ComPtr;

namespace {

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_DPICHANGED:
      return 0;  // keep the exact monitor rectangle; never accept the suggested one
    case WM_WINDOWPOSCHANGING: {
      auto* wp = reinterpret_cast<WINDOWPOS*>(l);
      wp->flags |= SWP_NOACTIVATE;
      break;
    }
    case WM_CLOSE:
      return 0;
    default:
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

struct Overlay {
  HWND hwnd = nullptr;
  std::string device;
  RectI rect;
  RectI work;
  float scale = 1;
  bool primary = false;
  bool shown = false;
  bool hadContent = true;
  double fadeStart = 0;     // when the overlay was (re)shown: content fades in
  int pending = 0;          // presents whose frame-latency release has not been consumed yet
  double stalledSince = 0;  // DWM stopped taking our frames: present nothing until it catches up
  ComPtr<IDXGISwapChain1> swap;
  HANDLE waitable = nullptr;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual> visual;
  ComPtr<ID2D1Bitmap1> bitmap;
};

struct PileGeom {
  uint32_t version = 0xffffffffu;
  float width = -1, radius = -1;
  float peak = 0;
  double builtAt = -1;
  ComPtr<ID2D1PathGeometry> fill, rim;
};

struct SnowmanRt {
  HWND bound = nullptr;
  bool hidden = false;
  double buildStart = -100;
  double hopStart = -100;
  float hopFrom = 0.5f, hopTo = 0.5f;
  uint64_t lastSnapSeq = 0;
  uint64_t anchorHwnd = ~0ull;  // anchor the binding was made for (user re-anchoring resets it)
  bool anchorTaskbar = false;
};

struct SnowmanDraw {
  const SnowmanDecor* sm = nullptr;
  uint64_t surfaceId = 0;  // window it stands on (0 = desktop ground)
  Vec2 base;
  float height = 0;
  float build = 1, wobble = 0, lift = 0;
};

struct FrameTiming {
  double sample = 0, target = 0, period = 1.0 / 60;
  bool latch = false;
};

constexpr double kMinLead = 0.0015;
constexpr double kFadeIn = 0.6;  // s, overlay content fading in after a pause

}  // namespace

struct Engine::Impl {
  // ---------------------------------------------------------------- shared
  std::mutex mu;
  EngineConfig cfg;
  uint64_t cfgVer = 1;
  DecorState decor;
  uint64_t decorVer = 0;
  std::deque<EngineCommand> cmds;
  std::vector<DecorLayoutItem> layout;
  std::vector<SnowmanUpdate> updates;
  std::vector<HWND> overlayHwnds;
  HANDLE wake = nullptr;
  std::thread worker;
  std::atomic<bool> stop{false};
  LaunchOptions opts;
  WindowTracker* tracker = nullptr;
  HWND notify = nullptr;

  // ---------------------------------------------------------- render thread
  EngineConfig c;
  uint64_t seenCfg = 0;
  DecorState d;
  uint64_t seenDecor = 0;
  bool decorLoaded = false;
  std::wstring className;
  bool deviceLost = false;

  ComPtr<ID3D11Device> d3d;
  ComPtr<IDXGIDevice> dxgiDev;
  ComPtr<IDXGIFactory2> dxgiFactory;
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<ID2D1Factory1> d2dFactory;
  ComPtr<ID2D1Device> d2dDevice;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<ID2D1Bitmap1> flakeBmp, squareBmp;
#ifdef SNOW_HAS_SPRITE_BATCH
  ComPtr<ID2D1DeviceContext3> dc3;
  ComPtr<ID2D1SpriteBatch> batch;
  std::vector<D2D1_RECT_F> sprRects;
  std::vector<D2D1_COLOR_F> sprColors;
  std::vector<D2D1_MATRIX_3X2_F> sprXforms;
#endif
  ComPtr<ID2D1GradientStopCollection> pileStops;
  ComPtr<ID2D1LinearGradientBrush> pileBrush;
  std::string pileBrushTheme;
  std::unordered_map<int, ComPtr<IDWriteTextFormat>> fmtLabel, fmtMono, fmtHint;
  gfx::SolidBrush brush;
  gfx::DecorRenderer decorR;
  std::vector<std::wstring> toolLabels;
  std::wstring hintText;
  Lang labelsLang = static_cast<Lang>(-1);

  std::vector<Overlay> overlays;
  std::vector<MonitorInfo> monitors;
  World world;
  ActorSystem actors;
  std::unordered_map<HWND, MotionTracker> motion;
  std::unordered_map<uint64_t, PileGeom> pileGeoms;
  std::unordered_map<uint32_t, SnowmanRt> smRt;
  std::unordered_map<uint32_t, double> treeGrow;
  // Optional PNG overrides from <exe dir>\assets (see assets/README.md).
  struct Sprites {
    ComPtr<ID2D1Bitmap1> tree, snowman, santa, train, yeti;
  } sprites;
  void LoadSprites();
  ComPtr<ID2D1Bitmap1> LoadPng(const std::wstring& path);
  void DrawSprite(ID2D1Bitmap1* bmp, float cx, float bottom, float height, bool mirror, float angle, float alpha);

  std::shared_ptr<const TrackerSnapshot> snap;
  std::vector<HotWindow> hot;
  std::vector<WindowInput> inputs;
  std::vector<const TrackedWindow*> inputSrc;
  std::vector<SnowmanDraw> snowmenDraw;
  std::vector<DecorLayoutItem> layoutLocal;
  // Train hit: snowman id -> (time of the hit, train direction). The tumble math
  // is in core/knock.h; entries vanish when the snowman is back on its feet.
  struct KnockRun {
    double t0 = 0;
    int dir = 1;
    uint32_t seed = 0;  // per-snowman variation, keeps the row from moving in lockstep
  };
  std::unordered_map<uint32_t, KnockRun> knockedAt;
  std::vector<HWND> destroyedTmp;

  // Snowballs: a ball rolled out of a drift stays on the edge after the mouse
  // is released, can be picked up again and stacked into a snowman.
  struct Ball {
    uint32_t id = 0;
    uint64_t surface = 0;
    float localX = 0;   // px from the surface's left end (follows the window)
    float mass = 0, radius0 = 6, radius = 6, spin = 0;
    bool held = false;
    uint32_t atopOf = 0;  // ball id this one rests on (0 = on the edge)
    int stack = 0;        // how many balls rest on this one
    double hitT0 = -1;    // train knock start time, < 0 = not hit
    int hitDir = 1;
  };
  std::vector<Ball> balls;
  uint32_t nextBallId = 1;
  std::vector<SnowballSnowmanSpawn> spawns;
  Ball* FindBall(uint32_t id);
  Ball* BallUnder(POINT pt);
  Vec2 BallPos(const Ball& b) const;
  void KnockBalls(double now, const TrainHitBox& hb);
  HHOOK mouseHook = nullptr;
  static Impl* hookOwner;
  double ballLastNow = 0;

  // Popup menus (Win32 class "#32768") are drawn by the shell above every
  // topmost overlay; painting the taskbar drift over an open tray/context menu
  // hides its items (the user could not click "Exit"), so the taskbar pile is
  // skipped while a menu overlaps it.
  std::vector<RectI> menuRects;
  double menuScanAt = -1e9;
  void ScanPopupMenus(double now);
  bool PopupMenuCovers(const RectI& r) const;

  HANDLE timer = nullptr;
  bool hiResTimer = false;
  double lastTarget = 0, prevSimTime = 0;
  std::array<double, 32> costHist{};
  size_t costIdx = 0;
  double lastLead = 0;
  // debug statistics
  double fpsWindowStart = 0;
  int fpsFrames = 0;
  double fps = 0, lastCostMs = 0;
  std::atomic<double> fpsShared{0};  // `fps` for other threads (settings window)
  double hotSpeed = 0, hotErrPx = 0, hotErrAvg = 0;
  std::unordered_map<HWND, std::pair<double, Vec2d>> lastPrediction;  // target time, predicted pos
  FILE* telemetry = nullptr;
  uint64_t frameNo = 0;
  HANDLE readyEvent = nullptr;
  bool readySignaled = false;
  uint64_t lastSnapSeqPruned = 0;
  int idleFrames = 0;
  struct {
    double start = 0;
    int frames = 0, presents = 0, timeouts = 0, stalledSkips = 0, hiddenWaits = 0;
    int movingFrames = 0;
    double movingSpan = 0;  // seconds covered by consecutive frames while a window moved
  } stats;  // logged once a minute
  double lastFrameSample = 0;
  bool lastFrameMoving = false;
  double lastPeriod = 1.0 / 60;
  void LogStats(bool final = false);  // final: also log a partial interval (shutdown)
  // Live counters for SnowHarness (named shared memory, see shared_stats.h).
  HANDLE sharedMap = nullptr;
  SharedStats* shared = nullptr;
  void OpenSharedStats();
  void PublishOverlayState();
  void ClearAndHide(Overlay& ov);

  // ------------------------------------------------------------------ setup
  void Run();
  bool CreateDevice();
  void ReleaseDevice();
  void RegisterClass();
  void RebuildOverlays();
  void DestroyOverlays();
  bool CreateOverlayResources(Overlay& ov);
  void ApplyVisibility();
  void PullShared();
  void Pump();
  void SleepUntil(double t, bool spin);
  void WaitSwapChains(double timeout);
  FrameTiming Pace();
  int EffectiveFpsCap() const;
  bool SyncLatch() const;
  bool PredictOn() const;
  bool Frame(const FrameTiming& ft);  // true if anything was presented
  void BuildInputs(const FrameTiming& ft);
  void UpdateDecor(double now);
  SimSettings MakeSimSettings() const;
  void ConfigureWorld();
  // drawing
  bool RenderOverlay(Overlay& ov, const FrameTiming& ft);
  bool DrawPile(const Surface& s, const Overlay& ov, const D2D1_MATRIX_3X2_F& base);
  void BuildPileGeom(const Surface& s, PileGeom& g);
  bool DrawParticles(const Overlay& ov);
  bool DrawHarness(const Overlay& ov);
  bool DrawDebug(const Overlay& ov, const FrameTiming& ft);
  bool DrawDecorateUi(const Overlay& ov);
  // Snowball rolling: press on a snow drift, drag to scoop it up and grow.
  static LRESULT CALLBACK MouseHookProc(int code, WPARAM w, LPARAM l);
  bool OnSnowballMouse(UINT msg, POINT pt);  // true = swallow the click
  void UpdateSnowball(double now, float dt);
  bool SnowballSurfaceAt(POINT pt, const Surface*& out, float& localX) const;
  void ClearWindowRect(const WindowInput& w);
  IDWriteTextFormat* Format(std::unordered_map<int, ComPtr<IDWriteTextFormat>>& cache, const wchar_t* family,
                            float size, float scale, bool bold);
  const Theme& CurTheme() const { return GetTheme(c.settings.theme); }
  Color SnowColor() const;
  void EnsureLabels();
};

Engine::Impl* Engine::Impl::hookOwner = nullptr;

// ============================================================== Engine API

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() { Stop(); }

bool Engine::Start(const LaunchOptions& opts, WindowTracker* tracker, HWND notify) {
  Impl& m = *impl_;
  m.opts = opts;
  m.tracker = tracker;
  m.notify = notify;
  m.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  m.stop = false;
  m.worker = std::thread([&m] { m.Run(); });
  return true;
}

void Engine::Stop() {
  Impl& m = *impl_;
  if (!m.worker.joinable()) return;
  m.stop = true;
  SetEvent(m.wake);
  m.worker.join();
  CloseHandle(m.wake);
  m.wake = nullptr;
}

void Engine::SetConfig(const EngineConfig& cfg) {
  Impl& m = *impl_;
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.cfg = cfg;
    ++m.cfgVer;
  }
  if (m.wake) SetEvent(m.wake);
}

void Engine::SetDecor(const DecorState& decor) {
  Impl& m = *impl_;
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.decor = decor;
    ++m.decorVer;
  }
  if (m.wake) SetEvent(m.wake);
}

void Engine::Command(EngineCommand c) {
  Impl& m = *impl_;
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.cmds.push_back(c);
  }
  if (m.wake) SetEvent(m.wake);
}

void Engine::GetDecorLayout(std::vector<DecorLayoutItem>& out) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  out = impl_->layout;
}

void Engine::TakeSnowmanUpdates(std::vector<SnowmanUpdate>& out) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  out.swap(impl_->updates);
  impl_->updates.clear();
}

void Engine::TakeSnowballSpawns(std::vector<SnowballSnowmanSpawn>& out) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  out.swap(impl_->spawns);
  impl_->spawns.clear();
}

std::vector<HWND> Engine::OverlayWindows() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->overlayHwnds;
}

double Engine::Fps() const { return impl_ ? impl_->fpsShared.load() : 0.0; }

namespace {
// Where overlays go in the z-order: the top of the topmost band, or right under
// our settings window while that is topmost (it is while the user works in it).
HWND OverlayInsertAfter(HWND keepAbove) {
  if (keepAbove && IsWindowVisible(keepAbove) && (GetWindowLongW(keepAbove, GWL_EXSTYLE) & WS_EX_TOPMOST))
    return keepAbove;
  return HWND_TOPMOST;
}
}  // namespace

// ============================================================ render thread

void Engine::Impl::RegisterClass() {
  className = L"DesktopSnowOverlay." + opts.instance;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = OverlayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = className.c_str();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
}

bool Engine::Impl::CreateDevice() {
  ReleaseDevice();
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  if (FAILED(hr)) {
    Log("engine: hardware device failed (0x%08lx), using WARP", static_cast<unsigned long>(hr));
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, static_cast<UINT>(std::size(levels)),
                           D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  }
  if (FAILED(hr)) return false;
  if (FAILED(d3d.As(&dxgiDev))) return false;
  // Frame latency of the device itself: no CPU run-ahead.
  ComPtr<IDXGIDevice1> dev1;
  if (SUCCEEDED(dxgiDev.As(&dev1))) dev1->SetMaximumFrameLatency(1);
  if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(dxgiFactory.GetAddressOf()))))
    return false;
  if (FAILED(DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice),
                                      reinterpret_cast<void**>(dcomp.GetAddressOf()))))
    return false;
  D2D1_FACTORY_OPTIONS fo{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                               reinterpret_cast<void**>(d2dFactory.GetAddressOf()))))
    return false;
  if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice))) return false;
  if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
  dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
  dc->SetDpi(96.f, 96.f);
  if (!dwrite)
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
#ifdef SNOW_HAS_SPRITE_BATCH
  if (SUCCEEDED(dc.As(&dc3))) dc3->CreateSpriteBatch(&batch);
#endif
  brush.reset(dc.Get());
  decorR.Reset(dc.Get(), d2dFactory.Get());

  // Soft flake sprite (premultiplied radial falloff) and a solid square for confetti.
  const UINT N = 32;
  std::vector<uint32_t> px(N * N), sq(N * N, 0xFFFFFFFFu);
  for (UINT y = 0; y < N; ++y) {
    for (UINT x = 0; x < N; ++x) {
      float dx = (x + 0.5f) / N * 2.f - 1.f, dy = (y + 0.5f) / N * 2.f - 1.f;
      float r = std::sqrt(dx * dx + dy * dy);
      float a = r < 0.45f ? 1.f : (r < 1.f ? std::pow(1.f - (r - 0.45f) / 0.55f, 1.6f) : 0.f);
      uint32_t A = static_cast<uint32_t>(a * 255.f + 0.5f);
      px[y * N + x] = (A << 24) | (A << 16) | (A << 8) | A;
    }
  }
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  dc->CreateBitmap(D2D1::SizeU(N, N), px.data(), N * 4, &bp, &flakeBmp);
  dc->CreateBitmap(D2D1::SizeU(N, N), sq.data(), N * 4, &bp, &squareBmp);
  pileBrush.Reset();
  pileStops.Reset();
  pileBrushTheme.clear();
  pileGeoms.clear();
  deviceLost = false;
  LoadSprites();
  Log("engine: device ready (sprite batch: %s)",
#ifdef SNOW_HAS_SPRITE_BATCH
      batch ? "yes" : "no"
#else
      "not compiled"
#endif
  );
  return true;
}

void Engine::Impl::ReleaseDevice() {
  for (auto& ov : overlays) {
    ov.bitmap.Reset();
    ov.visual.Reset();
    ov.target.Reset();
    if (ov.waitable) CloseHandle(ov.waitable);
    ov.waitable = nullptr;
    ov.swap.Reset();
  }
  decorR.Reset(nullptr, nullptr);
  brush.reset(nullptr);
#ifdef SNOW_HAS_SPRITE_BATCH
  batch.Reset();
  dc3.Reset();
#endif
  pileGeoms.clear();
  pileBrush.Reset();
  pileStops.Reset();
  flakeBmp.Reset();
  squareBmp.Reset();
  sprites = Sprites();
  fmtLabel.clear();
  fmtMono.clear();
  fmtHint.clear();
  dc.Reset();
  d2dDevice.Reset();
  d2dFactory.Reset();
  dcomp.Reset();
  dxgiFactory.Reset();
  dxgiDev.Reset();
  d3d.Reset();
}

ComPtr<ID2D1Bitmap1> Engine::Impl::LoadPng(const std::wstring& path) {
  ComPtr<ID2D1Bitmap1> out;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES || !dc) return out;
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                              reinterpret_cast<void**>(wic.GetAddressOf()))))
    return out;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) ||
      FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom))) {
    Log("assets: cannot decode %s", Narrow(path).c_str());
    return out;
  }
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  dc->CreateBitmapFromWicBitmap(conv.Get(), &bp, &out);
  if (out) Log("assets: loaded %s", Narrow(path).c_str());
  return out;
}

void Engine::Impl::LoadSprites() {
  sprites = Sprites();
  if (opts.harness) return;
  std::wstring dir = ExeDir() + L"\\assets\\";
  sprites.tree = LoadPng(dir + L"tree.png");
  sprites.snowman = LoadPng(dir + L"snowman.png");
  sprites.santa = LoadPng(dir + L"santa.png");
  sprites.train = LoadPng(dir + L"train.png");
  sprites.yeti = LoadPng(dir + L"yeti.png");
}

// Draws a sprite with its bottom centre at (cx, bottom), scaled to `height`.
void Engine::Impl::DrawSprite(ID2D1Bitmap1* bmp, float cx, float bottom, float height, bool mirror, float angle,
                              float alpha) {
  if (!bmp || height <= 1.f) return;
  D2D1_SIZE_F sz = bmp->GetSize();
  if (sz.height <= 0) return;
  float w = height * sz.width / sz.height;
  D2D1_MATRIX_3X2_F saved;
  dc->GetTransform(&saved);
  D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(mirror ? -1.f : 1.f, 1.f) *
                            D2D1::Matrix3x2F::Rotation(angle * 57.29578f) *
                            D2D1::Matrix3x2F::Translation(cx, bottom);
  dc->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&saved));
  dc->DrawBitmap(bmp, D2D1::RectF(-w / 2, -height, w / 2, 0), alpha, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
  dc->SetTransform(saved);
}

bool Engine::Impl::CreateOverlayResources(Overlay& ov) {
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = static_cast<UINT>(std::max(1, ov.rect.width()));
  sd.Height = static_cast<UINT>(std::max(1, ov.rect.height()));
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  HRESULT hr = dxgiFactory->CreateSwapChainForComposition(d3d.Get(), &sd, nullptr, &ov.swap);
  if (FAILED(hr)) {
    Log("engine: CreateSwapChainForComposition failed 0x%08lx", static_cast<unsigned long>(hr));
    return false;
  }
  ComPtr<IDXGISwapChain2> sc2;
  if (SUCCEEDED(ov.swap.As(&sc2))) {
    sc2->SetMaximumFrameLatency(1);
    ov.waitable = sc2->GetFrameLatencyWaitableObject();
    // The waitable starts signalled. Consume that count before the first frame,
    // otherwise every later wait returns one frame early and two frames queue up
    // (measured: +1 frame of lag).
    if (ov.waitable) WaitForSingleObjectEx(ov.waitable, 1000, FALSE);
  }
  ov.pending = 0;
  ov.stalledSince = 0;
  if (FAILED(dcomp->CreateTargetForHwnd(ov.hwnd, TRUE, &ov.target))) return false;
  if (FAILED(dcomp->CreateVisual(&ov.visual))) return false;
  ov.visual->SetContent(ov.swap.Get());
  ov.target->SetRoot(ov.visual.Get());
  ComPtr<IDXGISurface> surface;
  if (FAILED(ov.swap->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf()))))
    return false;
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &ov.bitmap))) return false;
  ov.hadContent = true;
  return true;
}

void Engine::Impl::DestroyOverlays() {
  for (auto& ov : overlays) {
    ov.bitmap.Reset();
    ov.visual.Reset();
    ov.target.Reset();
    if (ov.waitable) CloseHandle(ov.waitable);
    ov.swap.Reset();
    if (ov.hwnd) DestroyWindow(ov.hwnd);
  }
  overlays.clear();
  if (dcomp) dcomp->Commit();
  std::lock_guard<std::mutex> lock(mu);
  overlayHwnds.clear();
}

void Engine::Impl::RebuildOverlays() {
  DestroyOverlays();
  std::vector<MonitorHandle> mons = EnumerateMonitors();
  monitors.clear();
  const DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                   WS_EX_NOREDIRECTIONBITMAP;
  for (const auto& m : mons) {
    Overlay ov;
    ov.device = m.info.id;
    ov.rect = m.info.rect;
    ov.work = m.info.work;
    ov.scale = m.info.scale;
    ov.primary = m.info.primary;
    ov.hwnd = CreateWindowExW(ex, className.c_str(), L"Desktop Snow", WS_POPUP, ov.rect.left, ov.rect.top,
                              ov.rect.width(), ov.rect.height(), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!ov.hwnd) {
      Log("engine: CreateWindowEx failed %lu", GetLastError());
      continue;
    }
    // Layered + transparent = input passes through; alpha 255 keeps DirectComposition content as is.
    SetLayeredWindowAttributes(ov.hwnd, 0, 255, LWA_ALPHA);
    if (!CreateOverlayResources(ov)) {
      Log("engine: overlay resources failed for %s", ov.device.c_str());
      deviceLost = true;
    }
    monitors.push_back(m.info);
    overlays.push_back(std::move(ov));
  }
  if (dcomp) dcomp->Commit();
  world.setMonitors(monitors);
  {
    std::lock_guard<std::mutex> lock(mu);
    overlayHwnds.clear();
    for (auto& ov : overlays) overlayHwnds.push_back(ov.hwnd);
  }
  Log("engine: %zu overlay(s)", overlays.size());
  for (const auto& m : monitors)
    Log("  monitor %s %dx%d at %d,%d scale %.2f%s", m.id.c_str(), m.rect.width(), m.rect.height(), m.rect.left,
        m.rect.top, m.scale, m.primary ? " primary" : "");
}

void Engine::Impl::ClearAndHide(Overlay& ov) {
  // Leave a transparent frame in the swap chain so that showing the window
  // again can't flash the snow it held before the pause.
  if (ov.bitmap && dc && !deviceLost) {
    dc->SetTarget(ov.bitmap.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) deviceLost = true;
    else if (ov.swap) ov.swap->Present(0, 0);
  }
  ShowWindow(ov.hwnd, SW_HIDE);
}

void Engine::Impl::ApplyVisibility() {
  bool changed = false;
  for (auto& ov : overlays) {
    bool fullscreen =
        std::find(c.hiddenMonitors.begin(), c.hiddenMonitors.end(), ov.device) != c.hiddenMonitors.end();
    bool blocked = c.paused || fullscreen;
    // Decorating needs the palette and the decorations on screen even when the
    // snow itself is paused (maximised browser, fullscreen app).
    bool want = c.decorateMode || !blocked;
    if (want == ov.shown) continue;
    changed = true;
    if (!want) {
      // Hidden overlays cost nothing and never sit above fullscreen games.
      ClearAndHide(ov);
      Log("engine: overlay %s hidden (%s)", ov.device.c_str(), c.paused ? "snow paused" : "fullscreen app");
    } else {
      // Releases that arrived while hidden are stale.
      if (ov.waitable)
        while (WaitForSingleObjectEx(ov.waitable, 0, FALSE) == WAIT_OBJECT_0) {
        }
      ov.pending = 0;
      ov.stalledSince = 0;
      ShowWindow(ov.hwnd, SW_SHOWNOACTIVATE);
      SetWindowPos(ov.hwnd, OverlayInsertAfter(c.keepAbove), 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      ov.fadeStart = opts.harness ? 0 : Now();
      Log("engine: overlay %s shown", ov.device.c_str());
    }
    ov.shown = want;
    ov.hadContent = true;
  }
  if (changed) {
    // Paused monitors spawn no snow and freeze their flakes (same order as `monitors`).
    // Decorating a paused monitor shows the overlay but keeps the snow frozen.
    std::vector<uint8_t> paused(overlays.size());
    for (size_t i = 0; i < overlays.size(); ++i) {
      bool fullscreen =
          std::find(c.hiddenMonitors.begin(), c.hiddenMonitors.end(), overlays[i].device) != c.hiddenMonitors.end();
      paused[i] = (c.paused || fullscreen) ? 1 : 0;
    }
    world.setPausedMonitors(paused);
    PublishOverlayState();
  }
}

void Engine::Impl::OpenSharedStats() {
  std::wstring name = L"Local\\DesktopSnow." + opts.instance + L".Stats";
  sharedMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedStats), name.c_str());
  if (!sharedMap) return;
  shared = static_cast<SharedStats*>(MapViewOfFile(sharedMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedStats)));
  if (!shared) {
    CloseHandle(sharedMap);
    sharedMap = nullptr;
    return;
  }
  ZeroMemory(shared, sizeof(SharedStats));
  shared->version = kSharedStatsVersion;
}

void Engine::Impl::PublishOverlayState() {
  if (!shared) return;
  uint32_t n = static_cast<uint32_t>(std::min<size_t>(overlays.size(), kSharedStatsMaxOverlays));
  for (uint32_t i = 0; i < n; ++i) {
    const Overlay& ov = overlays[i];
    shared->overlay[i] = SharedOverlayInfo{ov.rect.left, ov.rect.top, ov.rect.right, ov.rect.bottom, ov.shown ? 1 : 0, 0};
  }
  shared->overlays = n;
}

void Engine::Impl::Pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

SimSettings Engine::Impl::MakeSimSettings() const {
  const Settings& s = c.settings;
  SimSettings sim;
  sim.snowing = s.effect == "snow";
  sim.confetti = s.effect == "confetti";
  sim.piles = s.piles.enabled;
  sim.taskbarPile = s.piles.onTaskbar;
  sim.density = static_cast<float>(s.snow.density);
  sim.intensity = s.intensity / 100.f * (c.batteryMode ? 0.5f : 1.f);
  sim.windSpeed = s.snow.windSpeed;
  sim.flakeMin = s.snow.flakeMin;
  sim.flakeMax = s.snow.flakeMax;
  sim.fallSpeed = s.snow.fallSpeed;
  sim.sway = s.snow.sway;
  sim.pileMaxHeight = s.piles.maxHeight;
  sim.taskbarMaxHeight = s.piles.taskbarMaxHeight;
  sim.growth = s.piles.growth;
  sim.cornerRadiusOverride = s.piles.cornerRadius;
  sim.scatter.speedOn = s.scatter.speedOn;
  sim.scatter.speedOff = s.scatter.speedOn * s.scatter.hysteresis;
  sim.scatter.accelOn = s.scatter.accelOn;
  sim.scatter.regrowDelay = s.scatter.regrowDelay;
  sim.inherit = s.scatter.inherit;
  sim.windGusts = s.extras.windGusts;
  sim.cursorSweep = s.extras.cursorSweep;
  sim.melt = s.extras.melt;
  sim.sparkle = s.extras.sparkle;
  sim.reducedMotion = c.reducedMotion;
  if (opts.harness) {
    sim.snowing = sim.confetti = false;
    sim.piles = false;
  }
  return sim;
}

void Engine::Impl::ConfigureWorld() {
  world.configure(MakeSimSettings());
  ActorSettings as;
  as.santa = c.settings.extras.santa;
  as.train = c.settings.extras.train;
  as.yeti = c.settings.extras.yeti;
  as.reducedMotion = c.reducedMotion || opts.harness;
  actors.configure(as);
}

void Engine::Impl::PullShared() {
  std::deque<EngineCommand> local;
  bool cfgChanged = false, decorChanged = false;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (cfgVer != seenCfg) {
      c = cfg;
      seenCfg = cfgVer;
      cfgChanged = true;
    }
    if (decorVer != seenDecor) {
      d = decor;
      seenDecor = decorVer;
      decorChanged = true;
    }
    local.swap(cmds);
  }
  if (cfgChanged) ConfigureWorld();
  if (decorChanged) {
    double now = Now();
    // Items present in the first decor state were loaded from disk: no build animation.
    for (const auto& t : d.trees)
      if (!treeGrow.count(t.id)) treeGrow[t.id] = decorLoaded ? now : -100.0;
    for (const auto& s : d.snowmen) {
      bool known = smRt.count(s.id) != 0;
      auto& rt = smRt[s.id];
      if (!known && decorLoaded) rt.buildStart = now;  // newly built snowman
      if (rt.anchorHwnd != ~0ull && (rt.anchorHwnd != s.anchor.hwnd || rt.anchorTaskbar != s.anchor.taskbar))
        rt.bound = nullptr;  // moved to another window in decorate mode
      rt.anchorHwnd = s.anchor.hwnd;
      rt.anchorTaskbar = s.anchor.taskbar;
      rt.lastSnapSeq = 0;  // re-resolve the anchor
    }
    decorLoaded = true;
  }
  for (EngineCommand cmd : local) {
    std::vector<SnowmanSpot> spots;
    for (const auto& item : layoutLocal)
      if (!item.tree && item.visible)
        spots.push_back({item.id, Vec2(item.baseX, item.baseY), World::MonitorIndexAt(monitors, item.baseX, item.baseY - 1)});
    switch (cmd) {
      case EngineCommand::ClearSnow: world.clearSnow(); break;
      case EngineCommand::Santa: actors.trigger(ActorKind::Santa, Now(), monitors, spots); break;
      case EngineCommand::Train: actors.trigger(ActorKind::Train, Now(), monitors, spots); break;
      case EngineCommand::Yeti: actors.trigger(ActorKind::Yeti, Now(), monitors, spots); break;
      case EngineCommand::MonitorsChanged:
        RebuildOverlays();
        for (auto& ov : overlays) ov.shown = false;
        break;
      case EngineCommand::ReassertTopmost:
        for (auto& ov : overlays)
          if (ov.shown)
            SetWindowPos(ov.hwnd, OverlayInsertAfter(c.keepAbove), 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        break;
    }
  }
}

int Engine::Impl::EffectiveFpsCap() const {
  int cap = c.settings.fpsCap;
  if (c.batteryMode || c.remoteSession) cap = std::min(cap, 30);
  return std::max(20, cap);
}

bool Engine::Impl::SyncLatch() const {
  std::string mode = opts.syncOverride.empty() ? c.settings.sync.mode : opts.syncOverride;
  return mode == "latch" && !c.remoteSession;
}

bool Engine::Impl::PredictOn() const {
  if (opts.predictOverride >= 0) return opts.predictOverride == 1;
  return c.settings.sync.predict;
}

void Engine::Impl::SleepUntil(double t, bool spin) {
  for (;;) {
    double rem = t - Now();
    if (rem <= 0) return;
    double coarse = rem - (spin ? (hiResTimer ? 0.0006 : 0.0018) : 0.0);
    if (coarse > 0.0002) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(coarse * 1e7);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      HANDLE hs[2] = {timer, wake};
      DWORD r = MsgWaitForMultipleObjectsEx(2, hs, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      if (r == WAIT_OBJECT_0 + 2) Pump();
      if (stop) return;
      if (!spin && Now() >= t - 0.0005) return;
      continue;
    }
    if (!spin) return;
    // Short busy-wait for sub-millisecond precision (only while a window moves).
    while (Now() < t) YieldProcessor();
    return;
  }
}

void Engine::Impl::WaitSwapChains(double timeout) {
  // Each present releases the frame-latency semaphore once, when DWM takes the
  // frame. Block until every present has been taken (latency 1). With nothing
  // pending, any count left in the semaphore is stale (a release that arrived
  // after a timeout) and is drained, or every later wait returns a frame early.
  const double kStallRetry = 0.5;
  double deadline = Now() + std::max(0.0, timeout);
  for (auto& ov : overlays) {
    if (!ov.waitable) {
      ov.pending = 0;
      continue;
    }
    bool timedOut = false;
    for (;;) {
      bool block = ov.pending > 0 && ov.shown && ov.stalledSince == 0;
      double rem = block ? deadline - Now() : 0.0;
      DWORD ms = rem > 0 ? static_cast<DWORD>(rem * 1000.0 + 0.5) : 0;
      if (WaitForSingleObjectEx(ov.waitable, ms, FALSE) != WAIT_OBJECT_0) {
        timedOut = block;
        break;
      }
      if (ov.pending > 0) --ov.pending;
    }
    if (ov.pending == 0) {
      ov.stalledSince = 0;
    } else if (timedOut) {
      // This overlay isn't being composed (e.g. its display is off): stop
      // presenting to it so one dead output can't hold every frame for the timeout.
      ov.stalledSince = Now();
      ++stats.timeouts;
    } else if (ov.stalledSince > 0 && Now() - ov.stalledSince > kStallRetry) {
      ov.pending = 0;  // a release may be lost for good; retry, a late one drains as stale
      ov.stalledSince = 0;
    }
  }
}

void Engine::Impl::LogStats(bool final) {
  double now = Now();
  if (stats.start == 0) stats.start = now;
  double span = now - stats.start;
  if (span < (final ? 5.0 : 60.0)) return;
  Log("engine: last %.0f s: %d frames (%.1f fps, cap %d%s%s, display %.1f Hz), %d while windows moved "
      "(%.1f fps), %d presents, %d latency-wait timeouts, %d stalled-overlay skips, %d hidden waits",
      span, stats.frames, stats.frames / span, EffectiveFpsCap(), c.batteryMode ? " battery" : "",
      c.remoteSession ? " remote" : "", 1.0 / lastPeriod, stats.movingFrames,
      stats.movingSpan > 0.5 ? stats.movingFrames / stats.movingSpan : 0.0, stats.presents, stats.timeouts,
      stats.stalledSkips, stats.hiddenWaits);
  stats = {};
  stats.start = now;
}

FrameTiming Engine::Impl::Pace() {
  FrameTiming ft;
  // Swap chains ready for a new frame = DWM has latched our previous one.
  WaitSwapChains(0.05);
  double now = Now();
  double period = 1.0 / 60.0, next = 0;
  DCOMPOSITION_FRAME_STATISTICS fs{};
  if (dcomp && SUCCEEDED(dcomp->GetFrameStatistics(&fs)) && fs.timeFrequency.QuadPart > 0) {
    if (fs.currentCompositionRate.Numerator && fs.currentCompositionRate.Denominator)
      period = static_cast<double>(fs.currentCompositionRate.Denominator) / fs.currentCompositionRate.Numerator;
    double f = static_cast<double>(fs.timeFrequency.QuadPart);
    double offset = now - static_cast<double>(fs.currentTime.QuadPart) / f;
    next = static_cast<double>(fs.nextEstimatedFrameTime.QuadPart) / f + offset;
  } else {
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) {
      period = QpcToSeconds(static_cast<int64_t>(ti.qpcRefreshPeriod));
      next = QpcToSeconds(static_cast<int64_t>(ti.qpcVBlank)) + period;
    } else {
      next = now + period;
    }
  }
  period = clampd(period, 1.0 / 400.0, 1.0 / 20.0);
  lastPeriod = period;
  if (next < now - 1.0 || next > now + 1.0) next = now + period;  // nonsense estimate
  while (next <= now) next += period;

  if (tracker) tracker->HotWindows(now, hot);
  bool moving = !hot.empty() || opts.harness;
  // The fps cap is for idle snowfall only: while a window moves every
  // composition needs a fresh pile position, or the pile lags a whole refresh
  // on every skipped one.
  next = ChooseFrameTarget(next, lastTarget, period, EffectiveFpsCap(), moving && !c.remoteSession);
  ft.period = period;
  ft.latch = SyncLatch() && moving;
  if (ft.latch) {
    // Late latch: sample window positions as close to the composition deadline as
    // our measured render cost allows, so the pile and the window land in the same
    // DWM frame. The 90th percentile cost keeps one slow frame from widening the
    // window for the next 32 frames.
    std::array<double, 32> sorted = costHist;
    std::sort(sorted.begin(), sorted.end());
    double cost = sorted[sorted.size() * 9 / 10];
    double margin = (opts.latchMarginOverride >= 0 ? opts.latchMarginOverride : c.settings.sync.latchMarginMs) / 1000.0;
    double lead = clampd(cost + margin, kMinLead, period - 0.001);
    while (next - lead < now) next += period;
    SleepUntil(next - lead, true);
    lastLead = lead;
  } else {
    // vsync: start right after the previous composition; the frame is shown at `next`.
    SleepUntil(next - period, false);
    lastLead = next - Now();
  }
  ft.sample = Now();
  ft.target = next;
  if (tracker) tracker->HotWindows(ft.sample, hot);
  lastTarget = next;
  return ft;
}

void Engine::Impl::BuildInputs(const FrameTiming& ft) {
  inputs.clear();
  inputSrc.clear();
  if (!snap) return;
  bool predict = PredictOn();
  for (const auto& tw : snap->windows) {
    RECT r = tw.frame;
    const HotWindow* h = nullptr;
    for (const auto& hw : hot)
      if (hw.hwnd == tw.hwnd) h = &hw;
    if (h) {
      RECT fr;  // fresh read for moving windows, once per frame
      if (GetFrameBounds(tw.hwnd, fr)) r = fr;
    }
    MotionTracker& mt = motion[tw.hwnd];
    if (!mt.initialized()) mt.reset(ft.sample, Vec2d(r.left, r.top));
    else mt.addSample(ft.sample, Vec2d(r.left, r.top));
    if (h) mt.noteInputEvent(h->lastEvent, h->interval);
    long dx = 0, dy = 0;
    bool resizing = (r.right - r.left) != (tw.frame.right - tw.frame.left) ||
                    (r.bottom - r.top) != (tw.frame.bottom - tw.frame.top);
    if (predict && h && !resizing) {
      bool stepped = (opts.predictorOverride.empty() ? c.settings.sync.predictor : opts.predictorOverride) == "stepped";
      Vec2d p = stepped ? mt.predictStepped(ft.target, ft.sample) : mt.predict(ft.target, ft.sample);
      dx = std::lround(p.x - r.left);
      dy = std::lround(p.y - r.top);
    }
    WindowInput wi;
    wi.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tw.hwnd));
    wi.rect = RectI(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy);
    wi.z = tw.z;
    wi.holdsSnow = tw.holdsSnow;
    wi.isTaskbar = tw.isTaskbar;
    wi.cornerRadius = tw.cornerRadius;
    wi.scale = tw.scale;
    wi.velocity = Vec2(static_cast<float>(mt.velocity().x), static_cast<float>(mt.velocity().y));
    wi.accel = static_cast<float>(mt.accelMagnitude());
    inputs.push_back(wi);
    inputSrc.push_back(&tw);

    if (h) {
      // Estimated error of the previous frame: what we drew for its display time
      // versus where the window was when we looked again (debug readout only).
      auto it = lastPrediction.find(tw.hwnd);
      if (it != lastPrediction.end() && ft.sample >= it->second.first - 0.002) {
        double err = std::hypot(it->second.second.x - r.left, it->second.second.y - r.top);
        hotErrPx = err;
        hotErrAvg = hotErrAvg * 0.95 + err * 0.05;
      }
      lastPrediction[tw.hwnd] = {ft.target, Vec2d(wi.rect.left, wi.rect.top)};
      hotSpeed = mt.speed();
      if (telemetry) {
        std::fprintf(telemetry, "%llu,%.4f,%.4f,%s,%.3f,%p,%ld,%ld,%d,%d,%.1f,%.1f,%.3f\n",
                     static_cast<unsigned long long>(frameNo), ft.sample * 1000.0, ft.target * 1000.0,
                     ft.latch ? "latch" : "vsync", (ft.target - ft.sample) * 1000.0, static_cast<void*>(tw.hwnd),
                     r.left, r.top, wi.rect.left, wi.rect.top, mt.velocity().x, mt.velocity().y, lastCostMs);
      }
    }
  }
  if (snap->seq != lastSnapSeqPruned && (frameNo % 120) == 0) {
    lastSnapSeqPruned = snap->seq;
    // Drop cached pile geometry of windows whose surface no longer exists.
    for (auto it = pileGeoms.begin(); it != pileGeoms.end();) {
      if (!world.surface(it->first)) it = pileGeoms.erase(it);
      else ++it;
    }
    for (auto it = motion.begin(); it != motion.end();) {
      bool alive = false;
      for (const auto& tw : snap->windows) alive = alive || tw.hwnd == it->first;
      if (!alive) {
        lastPrediction.erase(it->first);
        it = motion.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void Engine::Impl::UpdateDecor(double now) {
  snowmenDraw.clear();
  layoutLocal.clear();
  if (opts.harness) return;
  auto monitorFor = [&](const std::string& id) -> const MonitorInfo* {
    for (const auto& m : monitors)
      if (m.id == id) return &m;
    for (const auto& m : monitors)
      if (m.primary) return &m;
    return monitors.empty() ? nullptr : &monitors.front();
  };

  for (const auto& t : d.trees) {
    const MonitorInfo* m = monitorFor(t.monitor);
    if (!m) continue;
    float h = TreeHeightPx(t.scale, m->scale);
    Vec2 base(static_cast<float>(m->work.left) + t.nx * m->work.width(),
              static_cast<float>(m->work.top) + t.ny * m->work.height());
    TreeBox b = TreeBounds(base, h);
    DecorLayoutItem it;
    it.id = t.id;
    it.tree = true;
    it.cls = 0;
    it.left = b.left;
    it.top = b.top;
    it.right = b.right;
    it.bottom = b.bottom;
    it.baseX = base.x;
    it.baseY = base.y;
    it.height = h;
    layoutLocal.push_back(it);
  }

  // Free-standing decorations: placed anywhere on the desktop.
  for (const auto& fd : d.free) {
    const MonitorInfo* m = monitorFor(fd.monitor);
    if (!m) continue;
    const float size = FreeDecorBaseSize(fd.kind) * fd.scale * m->scale;
    const float halfW = 0.5f * size * FreeDecorAspect(fd.kind);
    const float halfH = 0.5f * size;
    const float cx = static_cast<float>(m->work.left) + fd.nx * m->work.width();
    const float cy = static_cast<float>(m->work.top) + fd.ny * m->work.height();
    DecorLayoutItem it;
    it.id = fd.id;
    it.cls = 2;
    it.left = cx - halfW;
    it.top = cy - halfH;
    it.right = cx + halfW;
    it.bottom = cy + halfH;
    it.baseX = cx;
    it.baseY = cy;
    it.height = size;
    layoutLocal.push_back(it);
  }

  // Snowmen: bind to a window (HWND first, then process/class/title heuristics),
  // fall back to the taskbar / screen bottom so they never get lost.
  const WindowInput* taskbar = nullptr;
  for (const auto& w : inputs)
    if (w.isTaskbar && w.holdsSnow && (!taskbar || (!monitors.empty() && w.rect.left == monitors.front().rect.left)))
      taskbar = &w;
  for (const auto& sm : d.snowmen) {
    SnowmanRt& rt = smRt[sm.id];
    if (sm.free) {
      const MonitorInfo* m = monitorFor(sm.monitor);
      if (!m) continue;
      float h = SnowmanHeightPx(sm.scale, m->scale);
      SnowmanDraw dr;
      dr.sm = &sm;
      dr.base = Vec2(static_cast<float>(m->work.left) + sm.nx * m->work.width(),
                     static_cast<float>(m->work.top) + sm.ny * m->work.height());
      dr.height = h;
      dr.build = 1.f;
      dr.surfaceId = 0;
      snowmenDraw.push_back(dr);
      SnowmanBox b = SnowmanBounds(dr.base, h);
      DecorLayoutItem it;
      it.id = sm.id;
      it.cls = 1;
      it.left = b.left;
      it.top = b.top;
      it.right = b.right;
      it.bottom = b.bottom;
      it.baseX = dr.base.x;
      it.baseY = dr.base.y;
      it.height = h;
      it.hwnd = 0;
      layoutLocal.push_back(it);
      continue;
    }
    const WindowInput* target = nullptr;
    const TrackedWindow* targetSrc = nullptr;
    rt.hidden = false;
    if (!sm.anchor.taskbar) {
      if (rt.bound) {
        for (size_t i = 0; i < inputs.size(); ++i)
          if (inputSrc[i]->hwnd == rt.bound) {
            target = &inputs[i];
            targetSrc = inputSrc[i];
          }
        if (!target) {
          DWORD pid = 0;
          if (IsWindow(rt.bound) && GetWindowThreadProcessId(rt.bound, &pid) && ProcessName(pid) == sm.anchor.process)
            rt.hidden = true;  // minimised or on another desktop: keep the binding
          else
            rt.bound = nullptr;
        }
      }
      if (!rt.bound && snap && rt.lastSnapSeq != snap->seq) {
        rt.lastSnapSeq = snap->seq;
        int best = 0;
        size_t bestIdx = 0;
        for (size_t i = 0; i < inputs.size(); ++i) {
          if (!inputs[i].holdsSnow) continue;
          const TrackedWindow& tw = *inputSrc[i];
          WindowIdentity wid{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tw.hwnd)), tw.process, tw.className,
                             tw.title, false};
          int score = AnchorMatchScore(sm.anchor, wid);
          if (score > best) {
            best = score;
            bestIdx = i;
          }
        }
        if (best >= kAnchorMinScore) {
          rt.bound = inputSrc[bestIdx]->hwnd;
          target = &inputs[bestIdx];
          targetSrc = inputSrc[bestIdx];
          uint64_t boundId = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(targetSrc->hwnd));
          if (boundId != sm.anchor.hwnd || targetSrc->title != sm.anchor.title) {
            // Remember the new handle/title so the next launch finds the window faster.
            rt.anchorHwnd = boundId;
            SnowmanUpdate up;
            up.id = sm.id;
            up.relX = sm.relX;
            up.anchorChanged = true;
            up.anchor = sm.anchor;
            up.anchor.hwnd = boundId;
            up.anchor.title = targetSrc->title;
            std::lock_guard<std::mutex> lock(mu);
            updates.push_back(up);
          }
        }
      }
      if (target && !target->holdsSnow) rt.hidden = true;  // maximised: no edge to stand on
    }
    if (rt.hidden) {
      DecorLayoutItem it;
      it.id = sm.id;
      it.visible = false;
      layoutLocal.push_back(it);
      continue;
    }
    SnowmanDraw dr;
    dr.sm = &sm;
    RectI edge;
    float scale = 1;
    if (target) {
      edge = target->rect;
      scale = target->scale;
      dr.surfaceId = target->id;
    } else if (taskbar) {
      edge = taskbar->rect;
      scale = taskbar->scale;
      dr.surfaceId = taskbar->id;
    } else if (!monitors.empty()) {
      const MonitorInfo& m = monitors.front();
      edge = RectI(m.work.left, m.work.bottom, m.work.right, m.work.bottom + 1);
      scale = m.scale;
    } else {
      continue;
    }
    float h = SnowmanHeightPx(sm.scale, scale);
    float rel = sm.relX;
    float lift = 0;
    if (now - rt.hopStart < 0.6) {
      float u = static_cast<float>((now - rt.hopStart) / 0.6);
      rel = lerpf(rt.hopFrom, rt.hopTo, easeInOut(u));
      lift = std::sin(u * kPi) * 0.45f * h;
    }
    float half = 0.3f * h;
    float x = static_cast<float>(edge.left) + rel * edge.width();
    x = clampf(x, edge.left + half, std::max(edge.left + half, edge.right - half));
    // The base sits on the window/taskbar edge with a fixed 2.5 px bite into the
    // snow, independent of how deep the drift has grown: a growing pile must not
    // lift the snowman off its sill and leave it floating.
    const float embed = 2.5f * scale;
    dr.base = Vec2(x, static_cast<float>(edge.top) + embed - lift);
    dr.height = h;
    dr.build = rt.buildStart > 0 ? clampf(static_cast<float>((now - rt.buildStart) / 2.0), 0.f, 1.f) : 1.f;
    if (const Surface* s = world.surface(dr.surfaceId)) dr.wobble = s->wobble;
    dr.lift = lift;
    snowmenDraw.push_back(dr);
    SnowmanBox b = SnowmanBounds(dr.base, h);
    DecorLayoutItem it;
    it.id = sm.id;
    it.left = b.left;
    it.top = b.top;
    it.right = b.right;
    it.bottom = b.bottom;
    it.baseX = dr.base.x;
    it.baseY = dr.base.y;
    it.height = h;
    it.hwnd = dr.surfaceId;
    layoutLocal.push_back(it);
  }

  // Visitors.
  std::vector<SnowmanSpot> spots;
  for (const auto& dr : snowmenDraw)
    spots.push_back({dr.sm->id, dr.base, World::MonitorIndexAt(monitors, dr.base.x, dr.base.y - 1)});
  actors.update(now, static_cast<float>(std::min(0.1, now - prevSimTime)), monitors, spots);
  uint32_t scaredId;
  float dir;
  if (actors.popSnowmanScare(scaredId, dir)) {
    for (const auto& sm : d.snowmen) {
      if (sm.id != scaredId) continue;
      SnowmanRt& rt = smRt[sm.id];
      rt.hopStart = now;
      rt.hopFrom = sm.relX;
      float to = sm.relX + dir * 0.18f;
      if (to > 0.95f || to < 0.05f) to = sm.relX - dir * 0.18f;
      rt.hopTo = clampf(to, 0.05f, 0.95f);
      SnowmanUpdate up;
      up.id = sm.id;
      up.relX = rt.hopTo;
      std::lock_guard<std::mutex> lock(mu);
      updates.push_back(up);
    }
    if (notify) PostMessageW(notify, WM_APP_DECOR_UPDATE, 0, 0);
  }
  Vec2 landing;
  if (actors.popLanding(landing)) world.puff(landing, 40, ScaleAtPoint(static_cast<int>(landing.x), static_cast<int>(landing.y) - 1));
  bool pendingUpdates;
  {
    std::lock_guard<std::mutex> lock(mu);
    layout = layoutLocal;
    pendingUpdates = !updates.empty();
  }
  if (pendingUpdates && notify && (frameNo % 30) == 0) PostMessageW(notify, WM_APP_DECOR_UPDATE, 0, 0);
}

Color Engine::Impl::SnowColor() const {
  const Theme& th = CurTheme();
  Color col = th.snow;
  Color custom;
  if (c.settings.snow.color != "#ffffff" && ParseHexColor(c.settings.snow.color, custom)) col = custom;
  if (c.settings.extras.dayNightTint) col = Mix(col, th.nightTint, 0.35f * c.night);
  return col;
}

void Engine::Impl::ClearWindowRect(const WindowInput& w) {
  dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
  D2D1_RECT_F r = D2D1::RectF(static_cast<float>(w.rect.left), static_cast<float>(w.rect.top),
                              static_cast<float>(w.rect.right), static_cast<float>(w.rect.bottom));
  if (w.cornerRadius > 0.5f) {
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, w.cornerRadius, w.cornerRadius), brush(D2D1::ColorF(0, 0, 0, 0)));
  } else {
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    dc->FillRectangle(r, brush(D2D1::ColorF(0, 0, 0, 0)));
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  }
  dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

void Engine::Impl::BuildPileGeom(const Surface& s, PileGeom& g) {
  g.fill.Reset();
  g.rim.Reset();
  g.version = s.pile.version();
  g.width = s.pile.width();
  g.radius = s.cornerRadius;
  g.peak = s.pile.peak();
  const auto& h = s.pile.heights();
  int n = static_cast<int>(h.size());
  if (n == 0) return;
  float W = s.pile.width(), r = s.cornerRadius, step = s.pile.step();
  auto drop = [&](float x) { return cornerDrop(std::min(x, W - x), r); };
  std::vector<D2D1_POINT_2F> pts(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    float x = std::min(W, (i + 0.5f) * step);
    pts[static_cast<size_t>(i)] = D2D1::Point2F(x, drop(x) - h[static_cast<size_t>(i)]);
  }
  auto contour = [&](ID2D1GeometrySink* sink) {
    for (int i = 1; i < n; ++i) {
      D2D1_POINT_2F a = pts[static_cast<size_t>(i - 1)], b = pts[static_cast<size_t>(i)];
      D2D1_QUADRATIC_BEZIER_SEGMENT seg{a, D2D1::Point2F((a.x + b.x) / 2, (a.y + b.y) / 2)};
      sink->AddQuadraticBezier(&seg);
    }
    sink->AddLine(pts.back());
  };
  if (SUCCEEDED(d2dFactory->CreatePathGeometry(&g.fill))) {
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(g.fill->Open(&sink))) {
      sink->BeginFigure(D2D1::Point2F(0, drop(0)), D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine(pts.front());
      contour(sink.Get());
      sink->AddLine(D2D1::Point2F(W, drop(W)));
      // Back along the base, hugging rounded corners.
      if (r > 0.5f) {
        for (float x = W; x > W - r; x -= 1.f) sink->AddLine(D2D1::Point2F(x, drop(x)));
        sink->AddLine(D2D1::Point2F(W - r, 0));
        sink->AddLine(D2D1::Point2F(r, 0));
        for (float x = r; x > 0; x -= 1.f) sink->AddLine(D2D1::Point2F(x, drop(x)));
      } else {
        sink->AddLine(D2D1::Point2F(0, 0));
      }
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
    }
  }
  if (n >= 2 && SUCCEEDED(d2dFactory->CreatePathGeometry(&g.rim))) {
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(g.rim->Open(&sink))) {
      sink->BeginFigure(pts.front(), D2D1_FIGURE_BEGIN_HOLLOW);
      contour(sink.Get());
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      sink->Close();
    }
  }
}

bool Engine::Impl::DrawPile(const Surface& s, const Overlay& ov, const D2D1_MATRIX_3X2_F& base) {
  if (s.pile.empty()) return false;
  float peak = s.pile.peak();
  RectI bound(s.rect.left, s.rect.top - static_cast<int>(peak) - 3, s.rect.right,
              s.rect.top + static_cast<int>(s.cornerRadius) + 1);
  if (!bound.intersects(ov.rect)) return false;
  PileGeom& g = pileGeoms[s.id];
  // Slow growth changes the shape a little every frame; rebuilding the path at
  // 20 Hz is visually identical. Big changes (scatter, resize) rebuild at once.
  bool shapeChanged = g.version != s.pile.version();
  bool big = g.width != s.pile.width() || g.radius != s.cornerRadius || std::fabs(peak - g.peak) > 1.5f * s.scale;
  double now = Now();
  if (shapeChanged && (big || now - g.builtAt >= 0.05)) {
    BuildPileGeom(s, g);
    g.builtAt = now;
  }
  if (!g.fill) return false;
  const Theme& th = CurTheme();
  if (!pileBrush || pileBrushTheme != th.id) {
    pileBrush.Reset();
    pileStops.Reset();
    D2D1_GRADIENT_STOP stops[2] = {{0.f, gfx::ToD2D(th.pileTop)}, {1.f, gfx::ToD2D(th.pileBottom)}};
    dc->CreateGradientStopCollection(stops, 2, &pileStops);
    if (pileStops)
      dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, -30), D2D1::Point2F(0, 0)),
                                    pileStops.Get(), &pileBrush);
    pileBrushTheme = th.id;
  }
  dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(s.rect.left), static_cast<float>(s.rect.top)) *
                   *D2D1::Matrix3x2F::ReinterpretBaseType(&base));
  float tint = c.settings.extras.dayNightTint ? 0.25f * c.night : 0.f;
  if (pileBrush) {
    pileBrush->SetStartPoint(D2D1::Point2F(0, -std::max(4.f, g.peak)));
    pileBrush->SetEndPoint(D2D1::Point2F(0, s.cornerRadius));
    pileBrush->SetOpacity(1.f);
    dc->FillGeometry(g.fill.Get(), pileBrush.Get());
    if (tint > 0.01f) dc->FillGeometry(g.fill.Get(), brush(WithAlpha(th.nightTint, tint)));
  } else {
    dc->FillGeometry(g.fill.Get(), brush(th.pileTop));
  }
  if (g.rim) dc->DrawGeometry(g.rim.Get(), brush(th.pileEdge, 0.9f), 1.1f * s.scale);
  dc->SetTransform(base);
  return true;
}

bool Engine::Impl::DrawParticles(const Overlay& ov) {
  const auto& ps = world.particles();
  if (ps.empty()) return false;
  Color snowCol = SnowColor();
  const Theme& th = CurTheme();
  bool any = false;
  float l = static_cast<float>(ov.rect.left) - 16, t = static_cast<float>(ov.rect.top) - 16;
  float r = static_cast<float>(ov.rect.right) + 16, b = static_cast<float>(ov.rect.bottom) + 16;
#ifdef SNOW_HAS_SPRITE_BATCH
  if (batch && dc3) {
    sprRects.clear();
    sprColors.clear();
    for (const auto& p : ps) {
      if (p.kind == ParticleKind::Confetti || p.x < l || p.x > r || p.y < t || p.y > b) continue;
      float hs = p.size * 0.5f;
      sprRects.push_back(D2D1::RectF(p.x - hs, p.y - hs, p.x + hs, p.y + hs));
      sprColors.push_back(D2D1::ColorF(snowCol.r, snowCol.g, snowCol.b, p.alpha));
    }
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // required by DrawSpriteBatch
    if (!sprRects.empty()) {
      batch->Clear();
      batch->AddSprites(static_cast<UINT32>(sprRects.size()), sprRects.data(), nullptr, sprColors.data(), nullptr,
                        sizeof(D2D1_RECT_F), 0, sizeof(D2D1_COLOR_F), 0);
      dc3->DrawSpriteBatch(batch.Get(), flakeBmp.Get(), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                           D2D1_SPRITE_OPTIONS_NONE);
      any = true;
    }
    sprRects.clear();
    sprColors.clear();
    sprXforms.clear();
    for (const auto& p : ps) {
      if (p.kind != ParticleKind::Confetti || p.x < l || p.x > r || p.y < t || p.y > b) continue;
      float w = p.size * 0.9f, h = p.size * 0.45f * (0.6f + 0.4f * std::fabs(std::cos(p.angle * 1.7f)));
      sprRects.push_back(D2D1::RectF(-w / 2, -h / 2, w / 2, h / 2));
      Color cc = th.ornaments[p.colorIndex % th.ornaments.size()];
      sprColors.push_back(D2D1::ColorF(cc.r, cc.g, cc.b, 0.95f));
      D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Rotation(p.angle * 57.29578f) * D2D1::Matrix3x2F::Translation(p.x, p.y);
      sprXforms.push_back(m);
    }
    if (!sprRects.empty()) {
      batch->Clear();
      batch->AddSprites(static_cast<UINT32>(sprRects.size()), sprRects.data(), nullptr, sprColors.data(),
                        sprXforms.data(), sizeof(D2D1_RECT_F), 0, sizeof(D2D1_COLOR_F), sizeof(D2D1_MATRIX_3X2_F));
      dc3->DrawSpriteBatch(batch.Get(), squareBmp.Get(), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                           D2D1_SPRITE_OPTIONS_NONE);
      any = true;
    }
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return any;
  }
#endif
  // Fallback without sprite batches (older SDK headers): one bitmap draw per flake.
  D2D1_MATRIX_3X2_F base;
  dc->GetTransform(&base);
  for (const auto& p : ps) {
    if (p.x < l || p.x > r || p.y < t || p.y > b) continue;
    float hs = p.size * 0.5f;
    if (p.kind == ParticleKind::Confetti) {
      Color cc = th.ornaments[p.colorIndex % th.ornaments.size()];
      dc->SetTransform(D2D1::Matrix3x2F::Rotation(p.angle * 57.29578f) * D2D1::Matrix3x2F::Translation(p.x, p.y) *
                       *D2D1::Matrix3x2F::ReinterpretBaseType(&base));
      dc->FillRectangle(D2D1::RectF(-hs, -hs * 0.5f, hs, hs * 0.5f), brush(cc, 0.95f));
      dc->SetTransform(base);
    } else {
      dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), hs * 0.8f, hs * 0.8f), brush(snowCol, p.alpha));
    }
    any = true;
  }
  return any;
}

bool Engine::Impl::DrawHarness(const Overlay& ov) {
  // Measurement mode: 1-px outline just outside each SnowHarnessTarget window,
  // magenta at 50% alpha, drawn with exactly the transform used for piles.
  bool any = false;
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  ID2D1SolidColorBrush* br = brush(D2D1::ColorF(1.f, 0.f, 1.f, 0.5f));
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputSrc[i]->className != "SnowHarnessTarget") continue;
    const RectI& w = inputs[i].rect;
    if (!w.offset(0, 0).intersects(RectI(ov.rect.left - 2, ov.rect.top - 2, ov.rect.right + 2, ov.rect.bottom + 2)))
      continue;
    float L = static_cast<float>(w.left), T = static_cast<float>(w.top), R = static_cast<float>(w.right),
          B = static_cast<float>(w.bottom);
    dc->FillRectangle(D2D1::RectF(L - 1, T - 1, R + 1, T), br);  // row top-1
    dc->FillRectangle(D2D1::RectF(L - 1, B, R + 1, B + 1), br);  // row bottom
    dc->FillRectangle(D2D1::RectF(L - 1, T, L, B), br);          // column left-1
    dc->FillRectangle(D2D1::RectF(R, T, R + 1, B), br);          // column right
    any = true;
  }
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  return any;
}

IDWriteTextFormat* Engine::Impl::Format(std::unordered_map<int, ComPtr<IDWriteTextFormat>>& cache,
                                        const wchar_t* family, float size, float scale, bool bold) {
  int key = static_cast<int>(std::lround(scale * 100));
  auto it = cache.find(key);
  if (it != cache.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> f;
  if (dwrite)
    dwrite->CreateTextFormat(family, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * scale, L"", &f);
  cache[key] = f;
  return f.Get();
}

bool Engine::Impl::DrawDebug(const Overlay& ov, const FrameTiming& ft) {
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  for (size_t i = 0; i < inputs.size(); ++i) {
    const WindowInput& w = inputs[i];
    if (!w.rect.intersects(ov.rect)) continue;
    bool isHot = false;
    for (const auto& h : hot) isHot = isHot || h.hwnd == inputSrc[i]->hwnd;
    D2D1_COLOR_F col = isHot ? D2D1::ColorF(1.f, 0.85f, 0.f, 1.f)
                             : (w.holdsSnow || w.isTaskbar ? D2D1::ColorF(0.2f, 1.f, 0.4f, 0.9f)
                                                           : D2D1::ColorF(0.6f, 0.6f, 0.6f, 0.6f));
    float L = static_cast<float>(w.rect.left), T = static_cast<float>(w.rect.top);
    float R = static_cast<float>(w.rect.right), B = static_cast<float>(w.rect.bottom);
    ID2D1SolidColorBrush* br = brush(col);
    dc->FillRectangle(D2D1::RectF(L - 1, T - 1, R + 1, T), br);
    dc->FillRectangle(D2D1::RectF(L - 1, B, R + 1, B + 1), br);
    dc->FillRectangle(D2D1::RectF(L - 1, T, L, B), br);
    dc->FillRectangle(D2D1::RectF(R, T, R + 1, B), br);
  }
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  // Text on the primary monitor, or on the first visible one while the primary is paused.
  const Overlay* textHost = nullptr;
  for (const auto& o : overlays) {
    if (!o.shown) continue;
    if (!textHost || o.primary) textHost = &o;
    if (o.primary) break;
  }
  if (textHost != &ov) return true;
  IDWriteTextFormat* f = Format(fmtMono, L"Consolas", 12.f, ov.scale, false);
  if (!f) return true;
  WorldStats st = world.stats();
  wchar_t text[768];
  swprintf(text, 768,
           L"Desktop Snow debug\n"
           L"fps %.1f  refresh %.1f Hz  cap %d\n"
           L"sync %s  predict %s  lead %.2f ms\n"
           L"frame cost %.2f ms (worst %.2f)\n"
           L"hot windows %zu  speed %.0f px/s\n"
           L"est. pile error %.1f px (avg %.2f)\n"
           L"surfaces %d/%d  flakes %d  debris %d  bursts %d\n"
           L"wind %.0f  gust %.2f  battery %d  reduced %d",
           fps, 1.0 / ft.period, EffectiveFpsCap(), ft.latch ? L"late-latch" : L"vsync", PredictOn() ? L"on" : L"off",
           lastLead * 1000.0, lastCostMs, *std::max_element(costHist.begin(), costHist.end()) * 1000.0, hot.size(),
           hotSpeed, hotErrPx, hotErrAvg, st.activeSurfaces, st.surfaces, st.flakes, st.debris, st.bursts,
           static_cast<double>(world.wind()), static_cast<double>(world.gust()), c.batteryMode ? 1 : 0,
           c.reducedMotion ? 1 : 0);
  std::wstring all = text;
  if (!c.diagText.empty()) all += L"\npause decision:\n" + Widen(c.diagText);
  int lines = 1;
  for (wchar_t ch : all) lines += ch == L'\n' ? 1 : 0;
  float x = static_cast<float>(ov.work.left) + 12 * ov.scale, y = static_cast<float>(ov.work.top) + 12 * ov.scale;
  D2D1_RECT_F box = D2D1::RectF(x, y, x + 460 * ov.scale, y + (16.5f * static_cast<float>(lines) + 18.f) * ov.scale);
  dc->FillRoundedRectangle(D2D1::RoundedRect(box, 6 * ov.scale, 6 * ov.scale), brush(D2D1::ColorF(0, 0, 0, 0.72f)));
  box.left += 8 * ov.scale;
  box.top += 6 * ov.scale;
  dc->DrawText(all.c_str(), static_cast<UINT32>(all.size()), f, box, brush(D2D1::ColorF(0.9f, 1.f, 0.9f, 1.f)));
  return true;
}

void Engine::Impl::EnsureLabels() {
  if (labelsLang == c.lang && !toolLabels.empty()) return;
  labelsLang = c.lang;
  toolLabels.clear();
  const Str ids[] = {Str::ToolTree,  Str::ToolBauble,  Str::ToolLights, Str::ToolCandyCane, Str::ToolStar,
                     Str::ToolSnowman, Str::ToolHat, Str::ToolScarf,  Str::ToolErase,      Str::ToolDone};
  for (Str s : ids) toolLabels.push_back(Widen(Tr(s, c.lang)));
  hintText = Widen(Tr(Str::TipDecorateHint, c.lang));
}

bool Engine::Impl::DrawDecorateUi(const Overlay& ov) {
  EnsureLabels();
  PaletteLayout pal = LayoutPalette(ov.work, ov.scale, c.paletteLeft, c.paletteTop);
  const wchar_t* labels[static_cast<int>(Tool::Count)];
  for (int i = 0; i < static_cast<int>(Tool::Count); ++i) labels[i] = toolLabels[static_cast<size_t>(i)].c_str();
  gfx::DecorFrame f;
  f.time = Now();
  f.reducedMotion = c.reducedMotion;
  f.theme = &CurTheme();
  f.night = c.night;
  f.dpiScale = ov.scale;
  IDWriteTextFormat* lf = Format(fmtLabel, L"Segoe UI", 11.f, ov.scale, true);
  decorR.DrawPalette(f, pal, c.activeTool, labels, lf);
  if (c.pickerOpen && ov.work.contains(c.pickerX, c.pickerY)) {
    ColorPickerLayout cp = LayoutColorPicker(c.pickerX, c.pickerY, ov.work, ov.scale);
    decorR.DrawColorPicker(f, cp, c.pickerSelected);
  }
  IDWriteTextFormat* hf = Format(fmtHint, L"Segoe UI", 13.f, ov.scale, false);
  if (hf) {
    const RectI hint = HintRect(ov.work, pal.bar, ov.scale);
    const float padX = 12.f * ov.scale, padY = 5.f * ov.scale;
    const float maxW = static_cast<float>(hint.width()) - 2.f * padX;
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS tm{};
    float boxH = static_cast<float>(hint.height());
    if (dwrite && SUCCEEDED(dwrite->CreateTextLayout(hintText.c_str(), static_cast<UINT32>(hintText.size()), hf,
                                                     maxW, 1024.f * ov.scale, layout.GetAddressOf())) &&
        layout && SUCCEEDED(layout->GetMetrics(&tm)))
      boxH = std::max(boxH, tm.height + 2.f * padY);  // the plate grows with the wrapped text
    float top = static_cast<float>(hint.top);
    if (top + boxH > static_cast<float>(ov.work.bottom))
      top = std::max(static_cast<float>(ov.work.top), static_cast<float>(ov.work.bottom) - boxH);
    const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(hint.left), top, static_cast<float>(hint.right), top + boxH);
    dc->FillRoundedRectangle(D2D1::RoundedRect(box, 8 * ov.scale, 8 * ov.scale), brush(D2D1::ColorF(0, 0, 0, 0.6f)));
    if (layout)
      dc->DrawTextLayout(D2D1::Point2F(box.left + padX, top + padY), layout.Get(),
                         brush(D2D1::ColorF(1, 1, 1, 0.95f)));
    else
      dc->DrawText(hintText.c_str(), static_cast<UINT32>(hintText.size()), hf,
                   D2D1::RectF(box.left + padX, top + padY, box.right - padX, box.bottom), brush(D2D1::ColorF(1, 1, 1, 0.95f)));
  }
  return true;
}

bool Engine::Impl::RenderOverlay(Overlay& ov, const FrameTiming& ft) {
  if (!ov.bitmap) return false;
  dc->SetTarget(ov.bitmap.Get());
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  D2D1_MATRIX_3X2_F base =
      D2D1::Matrix3x2F::Translation(-static_cast<float>(ov.rect.left), -static_cast<float>(ov.rect.top));
  dc->SetTransform(base);
  bool content = false;
  // After a pause (fullscreen app closed, snow switched back on) the content
  // fades in instead of popping up.
  bool fading = false;
  if (ov.fadeStart > 0) {
    double u = (Now() - ov.fadeStart) / kFadeIn;
    if (u >= 1.0) {
      ov.fadeStart = 0;
    } else {
      fading = true;
      dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), static_cast<float>(std::max(0.0, u)), nullptr,
                                           D2D1_LAYER_OPTIONS1_NONE),
                    nullptr);
      content = true;  // the opacity changes every frame
    }
  }
  if (opts.harness) {
    content = DrawHarness(ov) || content;
  } else {
    gfx::DecorFrame f;
    f.time = ft.target;
    f.reducedMotion = c.reducedMotion;
    f.theme = &CurTheme();
    f.night = c.settings.extras.dayNightTint ? c.night : 0.f;
    f.dpiScale = ov.scale;
    double now = ft.target;
    // Desktop level: trees and snowmen without a window.
    for (const auto& t : d.trees) {
      for (const auto& it : layoutLocal) {
        if (!it.tree || it.id != t.id) continue;
        RectI box(static_cast<int>(it.left) - 30, static_cast<int>(it.top) - 30, static_cast<int>(it.right) + 30,
                  static_cast<int>(it.bottom) + 30);
        if (!box.intersects(ov.rect)) continue;
        TreeDecor td = t;
        auto g = treeGrow.find(t.id);
        td.grow = g == treeGrow.end() || g->second < 0 ? 1.f : clampf(static_cast<float>((now - g->second) / 1.5), 0.f, 1.f);
        gfx::DecorFrame tf = f;
        if (sprites.tree) {
          float k = easeOutCubic(td.grow);
          DrawSprite(sprites.tree.Get(), it.baseX, it.baseY, it.height * k, false, 0.f, 1.f);
          tf.treeBodyFromSprite = true;
        }
        decorR.DrawTree(tf, td, D2D1::Point2F(it.baseX, it.baseY), it.height, c.decorateMode && c.hoverId == t.id);
        content = true;
      }
    }
    // Free-standing decorations live on the desktop level, above trees? No:
    // drawn after the tree loop so they overlap trees when placed on top.
    for (const auto& fd : d.free) {
      for (const auto& it : layoutLocal) {
        if (it.cls != 2 || it.id != fd.id) continue;
        if (!ov.rect.containsF(it.baseX, it.baseY)) continue;
        decorR.DrawFreeDecor(f, fd, D2D1::Point2F(it.baseX, it.baseY), it.height,
                             c.decorateMode && c.hoverId == fd.id);
        content = true;
      }
    }
    // Train hit box: snowmen standing on the rails get knocked flying. The
    // check is geometric (core/knock.h) and runs before the drawing loop.
    if (!actors.train().active && !knockedAt.empty()) knockedAt.clear();  // a new pass starts clean
    if (c.settings.extras.train && c.settings.extras.trainKnocksSnowmen && actors.train().active) {
      const TrainState& tr = actors.train();
      TrainHitBox hb;
      hb.frontX = tr.x;
      hb.length = tr.length;
      hb.dir = tr.dir;
      hb.groundY = tr.groundY;
      for (const auto& dr : snowmenDraw) {
        if (knockedAt.find(dr.sm->id) != knockedAt.end()) continue;
        if (!TrainHitsSnowman(hb, dr.base.x, dr.base.y)) continue;
        knockedAt[dr.sm->id] = KnockRun{now, tr.dir, dr.sm->id};
      }
      KnockBalls(now, hb);  // snowballs on the rails fly too
    }
    auto drawSnowmenOn = [&](uint64_t surfaceId) {
      for (const auto& dr : snowmenDraw) {
        if (dr.surfaceId != surfaceId) continue;
        float kx = 0, ky = 0, krot = 0;
        auto kit = knockedAt.find(dr.sm->id);
        if (kit != knockedAt.end()) {
          const KnockPose kp = KnockAt(now - kit->second.t0, kit->second.dir, kit->second.seed);
          // The marker stays until the train leaves, otherwise the next
          // overlay/frame would start the tumble again and again.
          if (!kp.done) {
            kx = kp.dx;
            ky = kp.dy;
            krot = kp.rot;
          }
        }
        SnowmanBox b = SnowmanBounds(dr.base, dr.height);
        RectI box(static_cast<int>(b.left) - 20, static_cast<int>(b.top) - 20, static_cast<int>(b.right) + 20,
                  static_cast<int>(b.bottom) + 20);
        if (!box.intersects(ov.rect)) continue;
        const Vec2 base(dr.base.x + kx, dr.base.y + ky);
        if (sprites.snowman)
          DrawSprite(sprites.snowman.Get(), base.x, base.y, dr.height * easeOutCubic(dr.build), false,
                     dr.wobble + krot, 1.f);
        else
          decorR.DrawSnowman(f, *dr.sm, D2D1::Point2F(base.x, base.y), dr.height, dr.build, dr.wobble + krot,
                             c.decorateMode && c.hoverId == dr.sm->id);
        content = true;
      }
    };
    drawSnowmenOn(0);
    // The taskbar's pile (and snowmen standing on the taskbar) belongs to the
    // desktop level: windows drawn below clear it where they are, so it never
    // covers the bottom rows of a maximised or low window (status bars, input
    // fields), only shows where the desktop is behind it.
    for (const WindowInput& w : inputs) {
      if (!w.isTaskbar) continue;
      if (PopupMenuCovers(w.rect)) continue;  // an open tray/context menu owns this area
      if (const Surface* s = world.surface(w.id)) {
        if (s->active && DrawPile(*s, ov, base)) content = true;
      }
      drawSnowmenOn(w.id);
    }
    // Windows from the bottom of the z-order up: each window hides what is
    // beneath it (that is how piles get clipped by windows in front).
    for (size_t k = inputs.size(); k-- > 0;) {
      const WindowInput& w = inputs[k];
      if (!w.rect.intersects(ov.rect)) continue;
      if (content) ClearWindowRect(w);
      if (w.isTaskbar) continue;
      if (const Surface* s = world.surface(w.id)) {
        if (s->active && DrawPile(*s, ov, base)) content = true;
      }
      drawSnowmenOn(w.id);
    }
    // Sparkles: small four-point glints fading in and out on pile tops.
    for (const auto& sp : world.sparkles()) {
      if (!ov.rect.containsF(sp.x, sp.y)) continue;
      float u = static_cast<float>((now - sp.t0) / sp.dur);
      if (u < 0 || u > 1) continue;
      float a = std::sin(u * kPi);
      float s = sp.size * (0.6f + 0.4f * a);
      ID2D1SolidColorBrush* br = brush(CurTheme().sparkle, 0.9f * a);
      dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sp.x, sp.y), s * 0.18f, s), br);
      dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sp.x, sp.y), s, s * 0.18f), br);
      content = true;
    }
    if (DrawParticles(ov)) content = true;
    int idx = static_cast<int>(&ov - overlays.data());
    if (actors.train().active && actors.train().monitor == idx) {
      const TrainState& tr = actors.train();
      if (sprites.train)
        DrawSprite(sprites.train.Get(), tr.x - tr.dir * tr.length * 0.5f, tr.groundY, 90.f * tr.scale, tr.dir < 0, 0.f,
                   1.f);
      else
        decorR.DrawTrain(f, tr);
      content = true;
    }
    if (actors.yeti().active && actors.yeti().monitor == idx) {
      const YetiState& y = actors.yeti();
      if (sprites.yeti)
        DrawSprite(sprites.yeti.Get(), y.x, y.groundY - y.lift, 110.f * y.scale, y.dir < 0, 0.f, 1.f);
      else
        decorR.DrawYeti(f, y);
      content = true;
    }
    if (actors.santa().active && actors.santa().monitor == idx) {
      const SantaState& s = actors.santa();
      if (sprites.santa) {
        float h = 150.f * s.scale;
        D2D1_SIZE_F sz = sprites.santa->GetSize();
        float w = sz.height > 0 ? h * sz.width / sz.height : h;
        float cx = s.fromRight ? s.edgeX + w * 0.5f - s.peek * w * 0.75f : s.edgeX - w * 0.5f + s.peek * w * 0.75f;
        DrawSprite(sprites.santa.Get(), cx, s.y + h * 0.5f, h, !s.fromRight, 0.f, 1.f);
      } else {
        decorR.DrawSanta(f, s);
      }
      content = true;
    }
    for (const auto& b : balls) {
      float kx = 0.f, ky = 0.f;
      if (b.hitT0 >= 0) {
        const KnockPose kp = KnockAt(ft.target - b.hitT0, b.hitDir, b.id);
        if (!kp.done) { kx = kp.dx; ky = kp.dy; }
      }
      const Vec2 p = BallPos(b);
      if (!ov.rect.containsF(p.x + kx, p.y + ky)) continue;
      if (b.radius > 0.5f) {
        decorR.DrawSnowball(f, D2D1::Point2F(p.x + kx, p.y + ky), b.radius, b.spin);
        content = true;
      }
    }
    if (c.decorateMode && DrawDecorateUi(ov)) content = true;
  }
  if ((c.settings.debugOverlay || opts.debug) && !opts.harness && DrawDebug(ov, ft)) content = true;
  if (fading) dc->PopLayer();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  HRESULT hr = dc->EndDraw();
  dc->SetTarget(nullptr);
  if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
    deviceLost = true;
    return false;
  }
  if (!content && !ov.hadContent) return false;  // nothing changed on screen: skip the present
  ov.hadContent = content;
  // Always sync interval 0: DWM composes this swap chain, so it can't tear, and
  // Pace() already times the frame. With interval 1 the frame-latency release
  // came one refresh after the frame's target, halving the frame rate (CI telemetry).
  hr = ov.swap->Present(0, 0);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    Log("engine: device lost on present (0x%08lx)", static_cast<unsigned long>(hr));
    deviceLost = true;
    return false;
  }
  return true;
}


// ---------------------------------------------------------------- snowballs

LRESULT CALLBACK Engine::Impl::MouseHookProc(int code, WPARAM w, LPARAM l) {
  if (code < 0 || !hookOwner) return CallNextHookEx(nullptr, code, w, l);
  if (w == WM_LBUTTONDOWN || w == WM_LBUTTONUP) {
    auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
    POINT pt{ms->pt.x, ms->pt.y};
    if (hookOwner->OnSnowballMouse(static_cast<UINT>(w), pt)) return 1;  // swallow only over a drift
  }
  return CallNextHookEx(nullptr, code, w, l);
}

// ---------------------------------------------------------------- popup menus

void Engine::Impl::ScanPopupMenus(double now) {
  if (now - menuScanAt < 0.12) return;  // cheap: a few times per second at most
  menuScanAt = now;
  menuRects.clear();
  struct Ctx {
    std::vector<RectI>* out;
  } ctx{&menuRects};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t cls[32] = L"";
        if (!GetClassNameW(h, cls, 32)) return TRUE;
        if (std::wcscmp(cls, L"#32768") != 0) return TRUE;  // MENU class
        RECT r;
        if (!GetWindowRect(h, &r)) return TRUE;
        c->out->push_back(RectI(r.left, r.top, r.right, r.bottom));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
}

bool Engine::Impl::PopupMenuCovers(const RectI& r) const {
  for (const RectI& m : menuRects) {
    if (m.intersects(r)) return true;
  }
  return false;
}

bool Engine::Impl::SnowballSurfaceAt(POINT pt, const Surface*& out, float& localX) const {
  out = nullptr;
  localX = 0.f;
  if (!c.settings.extras.snowballs) return false;
  // drawOrder is bottom to top: the last match is the edge the user sees.
  for (const Surface* s : world.drawOrder()) {
    const float lx = static_cast<float>(pt.x) - static_cast<float>(s->rect.left);
    const float h = s->pile.heightAt(std::clamp(lx, 0.f, s->pile.width()));
    const float above = (static_cast<float>(s->rect.top) - h) - static_cast<float>(pt.y);
    if (SnowballGrabAt(s->pile, lx, above, s->scale)) {
      out = s;
      localX = lx;
    }
  }
  return out != nullptr;
}

Engine::Impl::Ball* Engine::Impl::FindBall(uint32_t id) {
  for (auto& b : balls)
    if (b.id == id) return &b;
  return nullptr;
}

Vec2 Engine::Impl::BallPos(const Ball& b) const {
  const Surface* s = world.surface(b.surface);
  if (!s) return Vec2();
  const float lx = std::clamp(b.localX, 0.f, s->pile.width());
  if (b.atopOf) {
    if (const Ball* p = const_cast<Impl*>(this)->FindBall(b.atopOf)) {
      const Vec2 pp = BallPos(*p);
      return Vec2(pp.x, pp.y - p->radius * 0.92f - b.radius * 0.92f);
    }
  }
  float y = static_cast<float>(s->rect.top) - b.radius * 0.55f;  // rests on the sill
  if (b.held) y = static_cast<float>(s->rect.top) - s->pile.heightAt(lx) - b.radius * 0.35f;
  return Vec2(static_cast<float>(s->rect.left) + lx, y);
}

Engine::Impl::Ball* Engine::Impl::BallUnder(POINT pt) {
  for (auto it = balls.rbegin(); it != balls.rend(); ++it) {
    if (it->held) continue;
    const Vec2 p = BallPos(*it);
    const float dx = static_cast<float>(pt.x) - p.x, dy = static_cast<float>(pt.y) - p.y;
    const float r = it->radius + 6.f;
    if (dx * dx + dy * dy <= r * r) return &(*it);
  }
  return nullptr;
}

bool Engine::Impl::OnSnowballMouse(UINT msg, POINT pt) {
  if (c.decorateMode || c.paused || !c.settings.extras.snowballs) {
    for (auto& b : balls) b.held = false;
    return false;
  }
  if (msg == WM_LBUTTONDOWN) {
    // Pick a resting ball up again before grabbing fresh snow.
    if (Ball* picked = BallUnder(pt)) {
      if (picked->atopOf) {
        if (Ball* p = FindBall(picked->atopOf)) p->stack = std::max(0, p->stack - 1);
      }
      picked->atopOf = 0;
      picked->held = true;
      return true;
    }
    const Surface* s = nullptr;
    float lx = 0.f;
    if (!SnowballSurfaceAt(pt, s, lx)) return false;
    Ball b;
    b.id = nextBallId++;
    b.surface = s->id;
    b.radius0 = std::max(4.f, 5.f * s->scale);
    b.radius = b.radius0;
    b.localX = lx;
    b.held = true;
    b.mass = world.carvePile(s->id, std::clamp(lx - b.radius0, 0.f, s->pile.width()),
                             std::clamp(lx + b.radius0, 0.f, s->pile.width()));
    b.radius = SnowballRadius(b.mass, b.radius0);
    balls.push_back(b);
    // Keep the desk tidy: drop the oldest resting ball once there are too many.
    while (balls.size() > 32) {
      auto it = std::find_if(balls.begin(), balls.end(), [](const Ball& x) { return !x.held; });
      if (it == balls.end()) break;
      balls.erase(it);
    }
    return true;
  }
  if (msg == WM_LBUTTONUP) {
    Ball* held = nullptr;
    for (auto& b : balls)
      if (b.held) { held = &b; break; }
    if (!held) return false;
    held->held = false;
    // Put it down on top of another ball when it lands there: three balls make
    // a snowman.
    Ball* host = nullptr;
    for (auto& b : balls) {
      if (&b == held || b.held || b.surface != held->surface || b.atopOf || b.stack >= 2) continue;
      if (b.radius < held->radius * 0.75f) continue;
      if (std::fabs(b.localX - held->localX) <= (b.radius + held->radius) * 0.55f) { host = &b; break; }
    }
    if (host) {
      held->atopOf = host->id;
      held->localX = host->localX;
      ++host->stack;
      if (host->stack >= 2) {
        if (const Surface* s = world.surface(host->surface)) {
          SnowballSnowmanSpawn sp;
          sp.surface = host->surface;
          sp.relX = s->pile.width() > 1.f ? std::clamp(host->localX / s->pile.width(), 0.f, 1.f) : 0.5f;
          sp.scale = s->scale;
          sp.taskbar = s->isTaskbar;
          {
            std::lock_guard<std::mutex> lock(mu);
            spawns.push_back(sp);
          }
          if (notify) PostMessageW(notify, WM_APP_DECOR_UPDATE, 0, 0);
          const uint32_t base = host->id;
          uint32_t mid = 0, top = 0;
          for (const auto& b : balls) {
            if (b.atopOf == base) { if (mid == 0) mid = b.id; else top = b.id; }
          }
          balls.erase(std::remove_if(balls.begin(), balls.end(), [&](const Ball& b) {
                        return b.id == base || b.id == mid || b.id == top;
                      }),
                      balls.end());
        }
      }
    }
    return true;
  }
  return false;
}

void Engine::Impl::UpdateSnowball(double now, float dt) {
  ballLastNow = now;
  (void)dt;
  if (c.decorateMode || c.paused || !c.settings.extras.snowballs) {
    for (auto& b : balls) b.held = false;
    return;
  }
  for (auto& b : balls) {
    if (b.hitT0 >= 0 && now - b.hitT0 > kKnockDuration) b.hitT0 = -1;
    if (!b.held) continue;
    const Surface* s = world.surface(b.surface);
    if (!s) { b.held = false; continue; }
    POINT pt{};
    if (!GetCursorPos(&pt)) return;
    const float lx = std::clamp(static_cast<float>(pt.x) - static_cast<float>(s->rect.left), 0.f,
                                std::max(0.f, s->pile.width()));
    // Carve every column the ball rolls over (interpolated so fast strokes miss
    // nothing); a bigger ball scoops a wider strip, so it grows quickly.
    const float from = b.localX;
    const float half = std::max(b.radius, b.radius0);
    const float step = std::max(3.f, b.radius0);
    const int n = std::max(1, static_cast<int>(std::fabs(lx - from) / step) + 1);
    float collected = 0.f;
    for (int i = 1; i <= n; ++i) {
      const float mid = from + (lx - from) * static_cast<float>(i) / static_cast<float>(n);
      const float a = std::clamp(mid - half, 0.f, s->pile.width());
      const float e = std::clamp(mid + half, 0.f, s->pile.width());
      collected += world.carvePile(b.surface, a, e);
    }
    b.mass += collected;
    b.radius = SnowballRadius(b.mass, b.radius0);
    b.spin += (lx - from) / std::max(4.f, b.radius);
    b.localX = lx;
  }
}

void Engine::Impl::KnockBalls(double now, const TrainHitBox& hb) {
  for (auto& b : balls) {
    if (b.hitT0 >= 0 || b.held) continue;
    const Surface* s = world.surface(b.surface);
    if (!s || !s->isTaskbar) continue;  // the train runs along the taskbar edge
    if (!TrainHitsSnowman(hb, static_cast<float>(s->rect.left) + b.localX, static_cast<float>(s->rect.top))) continue;
    b.hitT0 = now;
    b.hitDir = hb.dir;
  }
}

bool Engine::Impl::Frame(const FrameTiming& ft) {
  ++frameNo;
  double t0 = ft.sample;
  snap = tracker ? tracker->Snapshot() : nullptr;
  BuildInputs(ft);
  if (tracker) {
    tracker->TakeDestroyed(destroyedTmp);
    for (HWND h : destroyedTmp) {
      world.windowDestroyed(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)));
      motion.erase(h);
      lastPrediction.erase(h);
    }
  }
  POINT cp;
  bool cursorOk = GetCursorPos(&cp) != FALSE;
  world.setCursor(cursorOk ? Vec2(static_cast<float>(cp.x), static_cast<float>(cp.y)) : Vec2(), cursorOk);
  double dt = prevSimTime > 0 ? ft.target - prevSimTime : 1.0 / 60;
  world.update(ft.target, static_cast<float>(clampd(dt, 0.0, 0.1)), inputs);
  UpdateSnowball(ft.target, static_cast<float>(clampd(dt, 0.0, 0.1)));
  UpdateDecor(ft.target);
  ScanPopupMenus(ft.target);
  prevSimTime = ft.target;

  bool presented = false;
  for (auto& ov : overlays) {
    if (!ov.shown || deviceLost) continue;
    if (ov.stalledSince > 0) {
      ++stats.stalledSkips;
      continue;
    }
    bool did = RenderOverlay(ov, ft);
    if (did) {
      ++ov.pending;
      ++stats.presents;
      if (shared) InterlockedIncrement64(&shared->presents);
    }
    presented = did || presented;
  }
  ++stats.frames;
  bool movingNow = !hot.empty() || opts.harness;
  if (movingNow) {
    ++stats.movingFrames;
    if (lastFrameMoving && lastFrameSample > 0 && ft.sample - lastFrameSample < 0.1)
      stats.movingSpan += ft.sample - lastFrameSample;
  }
  lastFrameMoving = movingNow;
  lastFrameSample = ft.sample;
  if (shared) InterlockedIncrement64(&shared->frames);
  double cost = Now() - t0;
  costHist[costIdx++ % costHist.size()] = cost;
  lastCostMs = cost * 1000.0;
  ++fpsFrames;
  if (ft.sample - fpsWindowStart >= 1.0) {
    fps = fpsFrames / (ft.sample - fpsWindowStart);
    fpsShared.store(fps);
    fpsFrames = 0;
    fpsWindowStart = ft.sample;
  }
  if (presented && !readySignaled && readyEvent) {
    SetEvent(readyEvent);
    readySignaled = true;
  }
  return presented;
}

void Engine::Impl::Run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  hiResTimer = timer != nullptr;
  if (!timer) {
    timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    timeBeginPeriod(1);
  }
  if (opts.harness) {
    std::wstring name = L"Local\\DesktopSnow." + opts.instance + L".Ready";
    readyEvent = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
  }
  if (!opts.telemetryPath.empty()) {
    if (_wfopen_s(&telemetry, opts.telemetryPath.c_str(), L"wb") != 0) telemetry = nullptr;
    if (telemetry)
      std::fprintf(telemetry,
                   "frame,t_sample_ms,t_target_ms,mode,lead_ms,hwnd,raw_left,raw_top,drawn_left,drawn_top,vx,vy,"
                   "cost_ms\n");
  }
  costHist.fill(0.002);
  OpenSharedStats();
  RegisterClass();
  hookOwner = this;
  mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &Impl::MouseHookProc, GetModuleHandleW(nullptr), 0);
  if (!mouseHook) Log("engine: snowball mouse hook failed (%lu)", GetLastError());
  PullShared();
  world.seed(GetTickCount64());
  actors.seed(GetTickCount64() ^ 0x9E3779B97F4A7C15ULL);
  ConfigureWorld();
  int failures = 0;
  while (!stop && !CreateDevice()) {
    Log("engine: device creation failed, retrying");
    WaitForSingleObject(wake, std::min(5000, 250 << std::min(failures++, 4)));
  }
  RebuildOverlays();

  while (!stop) {
    PullShared();
    Pump();
    if (deviceLost) {
      // Display driver reset / TDR / removed adapter: rebuild everything.
      Log("engine: recreating device");
      DestroyOverlays();
      ReleaseDevice();
      if (!CreateDevice()) {
        WaitForSingleObject(wake, 1000);
        continue;
      }
      RebuildOverlays();
      continue;
    }
    ApplyVisibility();
    bool any = false;
    for (auto& ov : overlays) any = any || ov.shown;
    LogStats();
    if (!any) {
      // Every overlay hidden (snow off, fullscreen app, locked): no simulation,
      // no drawing; wake on a config change or once a second.
      ++stats.hiddenWaits;
      if (shared) InterlockedIncrement64(&shared->hiddenWaits);
      HANDLE hs[1] = {wake};
      MsgWaitForMultipleObjectsEx(1, hs, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      prevSimTime = 0;
      lastFrameSample = 0;
      continue;
    }
    FrameTiming ft = Pace();
    if (stop) break;
    // Nothing on screen and nothing moving (e.g. "Nothing falling", no
    // decorations): back off instead of spinning at the refresh rate.
    if (Frame(ft) || !hot.empty() || opts.harness) {
      idleFrames = 0;
    } else if (++idleFrames > 30) {
      HANDLE hs[1] = {wake};
      MsgWaitForMultipleObjectsEx(1, hs, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
  }

  if (mouseHook) {
    UnhookWindowsHookEx(mouseHook);
    mouseHook = nullptr;
  }
  hookOwner = nullptr;
  LogStats(true);
  DestroyOverlays();
  ReleaseDevice();
  if (telemetry) std::fclose(telemetry);
  if (readyEvent) CloseHandle(readyEvent);
  if (shared) UnmapViewOfFile(shared);
  if (sharedMap) CloseHandle(sharedMap);
  shared = nullptr;
  sharedMap = nullptr;
  if (timer) CloseHandle(timer);
  if (!hiResTimer) timeEndPeriod(1);
  UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));
  CoUninitialize();
}

}  // namespace snow::win
