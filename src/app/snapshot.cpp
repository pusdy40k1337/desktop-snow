// Offscreen render of every decoration and visitor with the real Direct2D
// artwork code, saved as PNG (one per theme). CI runs it so the artwork can be
// inspected without a Windows PC: DesktopSnow.exe --render-decor <dir>
#include "snapshot.h"

#include <d3d11.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../core/decor.h"
#include "../core/strings.h"
#include "../core/theme.h"
#include "gfx_common.h"
#include "gfx_decor.h"

namespace snow::win {

namespace {

using gfx::ComPtr;

constexpr UINT kW = 1600, kH = 900;

bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, const std::wstring& path) {
  D2D1_BITMAP_PROPERTIES1 rp = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1Bitmap1> readback;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(kW, kH), nullptr, 0, &rp, &readback))) return false;
  if (FAILED(readback->CopyFromBitmap(nullptr, target, nullptr))) return false;
  D2D1_MAPPED_RECT m{};
  if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  // The background is opaque, so premultiplied and straight alpha are the same.
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.GetAddressOf()))) &&
            SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(kW, kH)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(frame->WritePixels(kH, m.pitch, m.pitch * kH, m.bits)) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(enc->Commit());
  readback->Unmap();
  return ok;
}

TreeDecor MakeTree(int variant) {
  TreeDecor t;
  t.id = static_cast<uint32_t>(variant + 1);
  if (variant == 0) return t;  // bare tree
  t.star = true;
  t.lights = true;
  int n = 0;
  for (float v = 0.22f; v < 0.92f; v += 0.11f) {
    for (float u = 0.12f; u < 0.9f; u += 0.13f) {
      float uu = u + (static_cast<int>(v * 100) % 2 ? 0.06f : 0.f);
      if (!TreeFoliageContains(uu, v) || (n++ % (variant == 1 ? 2 : 3)) != 0) continue;
      Ornament o;
      o.u = uu;
      o.v = v;
      o.color = n % 6;
      o.kind = variant == 2 && n % 2 ? OrnamentKind::CandyCane : OrnamentKind::Bauble;
      t.ornaments.push_back(o);
    }
  }
  return t;
}

}  // namespace

int RenderDecorSnapshots(const std::wstring& dir, const char* buildId) {
  CreateDirectoryW(dir.c_str(), nullptr);
  ComPtr<ID3D11Device> d3d;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  const char* driver = "hardware GPU";
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  if (FAILED(hr)) {
    driver = "WARP (software)";
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  }
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<ID2D1Factory1> factory;
  ComPtr<ID2D1Device> dev;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<IDWriteFactory> dw;
  D2D1_FACTORY_OPTIONS fo{};
  if (FAILED(hr) || FAILED(d3d.As(&dxgi)) ||
      FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                               reinterpret_cast<void**>(factory.GetAddressOf()))) ||
      FAILED(factory->CreateDevice(dxgi.Get(), &dev)) ||
      FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) {
    std::printf("render-decor: Direct2D setup failed (0x%08lx)\n", static_cast<unsigned long>(hr));
    return 2;
  }
  dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
  dc->SetDpi(96.f, 96.f);
  std::wstring adapterName = Widen(driver);
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC ad{};
  if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
    adapterName = std::wstring(ad.Description) + L" (" + Widen(driver) + L")";
  D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(kW, kH), nullptr, 0, &tp, &target))) return 2;
  ComPtr<IDWriteTextFormat> caption, label;
  dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, 16.f, L"", &caption);
  dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, 11.f, L"", &label);
  std::vector<std::wstring> toolLabels;
  for (int i = 0; i < static_cast<int>(Tool::Count); ++i)
    toolLabels.push_back(Widen(Tr(static_cast<Str>(static_cast<int>(Str::ToolTree) + i), Lang::En)));
  std::vector<const wchar_t*> labelPtrs;
  for (const auto& s : toolLabels) labelPtrs.push_back(s.c_str());

  int written = 0;
  for (const std::string& themeId : ThemeIds()) {
    const Theme& th = GetTheme(themeId);
    gfx::DecorRenderer r;
    r.Reset(dc.Get(), factory.Get());
    gfx::SolidBrush brush;
    brush.reset(dc.Get());
    gfx::DecorFrame f;
    f.time = 1.25;
    f.theme = &th;
    f.dpiScale = 1.f;
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // A dark "desktop" on the left, a light app window on the right: artwork
    // has to read on both.
    ComPtr<ID2D1GradientStopCollection> stops;
    ComPtr<ID2D1LinearGradientBrush> sky;
    D2D1_GRADIENT_STOP gs[2] = {{0.f, D2D1::ColorF(0.05f, 0.09f, 0.20f)}, {1.f, D2D1::ColorF(0.13f, 0.22f, 0.38f)}};
    dc->CreateGradientStopCollection(gs, 2, &stops);
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kH)),
                                  stops.Get(), &sky);
    dc->FillRectangle(D2D1::RectF(0, 0, kW, kH), sky.Get());
    const D2D1_RECT_F win = D2D1::RectF(900, 330, 1560, 700);
    dc->FillRectangle(win, brush(D2D1::ColorF(0.94f, 0.95f, 0.97f)));
    dc->FillRectangle(D2D1::RectF(win.left, win.top, win.right, win.top + 32), brush(D2D1::ColorF(1, 1, 1)));
    dc->DrawRectangle(win, brush(D2D1::ColorF(0.55f, 0.6f, 0.68f)), 1.f);
    dc->FillRectangle(D2D1::RectF(0, kH - 48, kW, kH), brush(D2D1::ColorF(0.12f, 0.12f, 0.14f)));  // taskbar

    PaletteLayout pal = LayoutPalette(RectI(0, 0, static_cast<int>(kW), static_cast<int>(kH) - 48), 1.f);
    r.DrawPalette(f, pal, Tool::Bauble, labelPtrs.data(), label.Get());

    const float treeH = TreeHeightPx(1.f, 1.f);
    for (int i = 0; i < 3; ++i) {
      TreeDecor t = MakeTree(i);
      float h = treeH * (i == 2 ? 1.25f : 1.f);
      r.DrawTree(f, t, D2D1::Point2F(120.f + i * 240.f, kH - 48.f), h, false);
    }
    const float smH = SnowmanHeightPx(1.f, 1.f);
    for (int i = 0; i < 4; ++i) {
      SnowmanDecor sm;
      sm.id = static_cast<uint32_t>(10 + i);
      sm.hat = i;
      sm.scarf = i;
      r.DrawSnowman(f, sm, D2D1::Point2F(win.left + 90.f + i * 150.f, win.top), smH, 1.f, i == 3 ? 0.12f : 0.f,
                    i == 1);
    }
    SnowmanDecor building;
    building.hat = 1;
    r.DrawSnowman(f, building, D2D1::Point2F(960.f, kH - 48.f), smH, 0.55f, 0.f, false);  // half built

    SantaState santa;
    santa.active = true;
    santa.fromRight = true;
    santa.edgeX = static_cast<float>(kW);
    santa.y = 200.f;
    santa.peek = 1.f;
    santa.wave = 0.6f;
    r.DrawSanta(f, santa);
    TrainState train;
    train.active = true;
    train.x = 1520.f;
    train.groundY = kH - 48.f;
    train.dir = 1;
    train.wheel = 0.4f;
    train.length = 420.f;
    train.t = 2.0;
    r.DrawTrain(f, train);
    YetiState yeti;
    yeti.active = true;
    yeti.x = 820.f;
    yeti.groundY = kH - 48.f;
    yeti.dir = 1;
    yeti.walk = 0.3f;
    yeti.phase = 1;
    r.DrawYeti(f, yeti);

    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    std::wstring text = L"Desktop Snow " + Widen(buildId) + L" \x2014 theme " + Widen(themeId) +
                        L" \x2014 Direct2D on " + adapterName;
    if (caption)
      dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), caption.Get(), D2D1::RectF(16, 8, kW - 16, 40),
                   brush(D2D1::ColorF(1, 1, 1, 0.9f)));
    hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    std::wstring path = dir + L"\\decor-" + Widen(themeId) + L".png";
    if (SUCCEEDED(hr) && SavePng(dc.Get(), target.Get(), path)) {
      ++written;
      std::printf("render-decor: wrote %s\n", Narrow(path).c_str());
    } else {
      std::printf("render-decor: FAILED for theme %s (0x%08lx)\n", themeId.c_str(), static_cast<unsigned long>(hr));
    }
    r.Reset(nullptr, nullptr);
  }
  std::printf("render-decor: %d image(s), Direct2D on %s\n", written, driver);
  return written == static_cast<int>(ThemeIds().size()) ? 0 : 1;
}

}  // namespace snow::win
