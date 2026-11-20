#include "capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace snowharness {

using Microsoft::WRL::ComPtr;

namespace {

int64_t QpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

int64_t QpcFrequency() {
  LARGE_INTEGER v;
  QueryPerformanceFrequency(&v);
  return v.QuadPart;
}

std::string HrText(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

std::string Utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  s.resize(static_cast<size_t>(n - 1));
  return s;
}

RECT Intersect(const RECT& a, const RECT& b) {
  RECT r;
  r.left = std::max(a.left, b.left);
  r.top = std::max(a.top, b.top);
  r.right = std::min(a.right, b.right);
  r.bottom = std::min(a.bottom, b.bottom);
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void PrepareFrame(CapturedFrame& out, const RECT& r) {
  out.originX = r.left;
  out.originY = r.top;
  out.width = r.right - r.left;
  out.height = r.bottom - r.top;
  out.stride = out.width * 4;
  out.pixels.resize(static_cast<size_t>(out.stride) * static_cast<size_t>(out.height));
}

void CopyRows(CapturedFrame& out, const uint8_t* src, size_t srcPitch) {
  const size_t rowBytes = static_cast<size_t>(out.stride);
  for (int y = 0; y < out.height; ++y)
    std::memcpy(out.pixels.data() + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * srcPitch,
                rowBytes);
}

}  // namespace

const char* CaptureBackendName(CaptureBackend b) {
  switch (b) {
    case CaptureBackend::None: return "none";
    case CaptureBackend::DesktopDuplication: return "dd";
    case CaptureBackend::Gdi: return "gdi";
  }
  return "?";
}

struct ScreenCapture::Impl {
  CaptureMode mode = CaptureMode::Auto;
  CaptureBackend backend = CaptureBackend::None;
  std::string description;
  HMONITOR monitor = nullptr;
  RECT monitorRect{};
  double refreshHz = 60.0;
  int64_t freq = QpcFrequency();
  int accessLost = 0;

  // Desktop Duplication
  ComPtr<IDXGIOutput1> output;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGIOutputDuplication> dup;
  ComPtr<ID3D11Texture2D> staging;
  RECT outputRect{};
  bool systemMemory = false;
  std::string ddName;
  int64_t failingSince = 0;  // QPC of the first failed re-duplication, 0 if fine

  // GDI
  HDC screenDC = nullptr;
  HDC memDC = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ oldBitmap = nullptr;
  uint8_t* dibBits = nullptr;
  int dibW = 0;
  int dibH = 0;
  std::vector<uint8_t> previous;
  RECT previousRect{};
  int64_t nextTick = 0;
  HANDLE timer = nullptr;

  ~Impl() {
    ReleaseDd();
    ReleaseGdi();
    if (timer) CloseHandle(timer);
  }

  // ---- Desktop Duplication ----

  bool InitDd(std::string* why) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
      *why = "CreateDXGIFactory1 failed " + HrText(hr);
      return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> found;
    for (UINT a = 0; !found; ++a) {
      ComPtr<IDXGIAdapter1> candidate;
      if (factory->EnumAdapters1(a, candidate.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
      for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> out;
        if (candidate->EnumOutputs(o, out.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC od{};
        if (FAILED(out->GetDesc(&od))) continue;
        if (od.Monitor == monitor || SameRect(od.DesktopCoordinates, monitorRect)) {
          adapter = candidate;
          found = out;
          DXGI_ADAPTER_DESC1 ad{};
          adapter->GetDesc1(&ad);
          ddName = Utf8(od.DeviceName) + " on " + Utf8(ad.Description);
          break;
        }
      }
    }
    if (!found) {
      *why = "no DXGI output matches the test monitor";
      return false;
    }
    hr = found.As(&output);
    if (FAILED(hr)) {
      *why = "IDXGIOutput1 unavailable " + HrText(hr);
      return false;
    }
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                           device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr)) {
      *why = "D3D11CreateDevice failed " + HrText(hr);
      return false;
    }
    return CreateDuplication(why);
  }

  bool CreateDuplication(std::string* why) {
    dup.Reset();
    staging.Reset();
    HRESULT hr = output->DuplicateOutput(device.Get(), dup.GetAddressOf());
    if (FAILED(hr)) {
      const char* hint = "";
      if (hr == E_ACCESSDENIED) hint = " (access denied: secure desktop, UAC prompt or non-interactive session)";
      if (hr == DXGI_ERROR_UNSUPPORTED) hint = " (unsupported on this adapter/session)";
      if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) hint = " (too many duplication clients)";
      if (hr == DXGI_ERROR_SESSION_DISCONNECTED) hint = " (session disconnected)";
      *why = "DuplicateOutput failed " + HrText(hr) + hint;
      dup.Reset();
      return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
      *why = "output is rotated (only identity rotation is supported)";
      dup.Reset();
      return false;
    }
    if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
      *why = "desktop format " + std::to_string(static_cast<int>(dd.ModeDesc.Format)) +
             " is not B8G8R8A8_UNORM (HDR enabled?)";
      dup.Reset();
      return false;
    }
    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    outputRect = od.DesktopCoordinates;
    systemMemory = dd.DesktopImageInSystemMemory != FALSE;
    if (!systemMemory) {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = dd.ModeDesc.Width;
      td.Height = dd.ModeDesc.Height;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      hr = device->CreateTexture2D(&td, nullptr, staging.GetAddressOf());
      if (FAILED(hr)) {
        *why = "staging texture creation failed " + HrText(hr);
        dup.Reset();
        return false;
      }
    }
    return true;
  }

  void ReleaseDd() {
    if (dup) dup->ReleaseFrame();  // harmless if no frame is held
    dup.Reset();
    staging.Reset();
    context.Reset();
    device.Reset();
    output.Reset();
  }

  CaptureResult NextDd(const RECT& roi, uint32_t timeoutMs, CapturedFrame& out) {
    const int64_t deadline = QpcNow() + static_cast<int64_t>(timeoutMs) * freq / 1000;
    for (;;) {
      const int64_t now = QpcNow();
      const UINT remaining = now >= deadline ? 0 : static_cast<UINT>((deadline - now) * 1000 / freq);
      if (!dup) {
        std::string why;
        if (!CreateDuplication(&why)) {
          // Transient after a mode change or secure-desktop switch; give it 2 s
          // before falling back to GDI for the rest of the run.
          if (failingSince == 0) failingSince = now;
          if (mode == CaptureMode::Auto && now - failingSince > 2 * freq) {
            std::string gdiWhy;
            if (InitGdi(&gdiWhy)) {
              description += "; switched to GDI after losing duplication: " + why;
              backend = CaptureBackend::Gdi;
              ReleaseDd();
              return NextGdi(roi, remaining, out);
            }
          }
          if (remaining == 0) return CaptureResult::Timeout;
          Sleep(std::min<UINT>(remaining, 50));
          continue;
        }
        failingSince = 0;
      }
      DXGI_OUTDUPL_FRAME_INFO info{};
      ComPtr<IDXGIResource> resource;
      HRESULT hr = dup->AcquireNextFrame(remaining, &info, resource.GetAddressOf());
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CaptureResult::Timeout;
      if (FAILED(hr)) {
        // Recreate on the next attempt (ACCESS_LOST is the documented case).
        if (hr == DXGI_ERROR_ACCESS_LOST) ++accessLost;
        dup.Reset();
        staging.Reset();
        if (hr != DXGI_ERROR_ACCESS_LOST) return CaptureResult::Error;
        if (remaining == 0) return CaptureResult::Timeout;
        continue;
      }
      if (info.LastPresentTime.QuadPart == 0) {  // pointer-only update: no new desktop image
        dup->ReleaseFrame();
        if (remaining == 0) return CaptureResult::Timeout;
        continue;
      }
      const RECT r = Intersect(roi, outputRect);
      if (r.right <= r.left || r.bottom <= r.top) {
        dup->ReleaseFrame();
        return CaptureResult::Error;
      }
      PrepareFrame(out, r);
      out.qpc = info.LastPresentTime.QuadPart;
      const int ox = r.left - outputRect.left;
      const int oy = r.top - outputRect.top;
      bool ok = false;
      if (systemMemory) {
        DXGI_MAPPED_RECT mapped{};
        if (SUCCEEDED(dup->MapDesktopSurface(&mapped))) {
          const size_t pitch = static_cast<size_t>(mapped.Pitch);
          CopyRows(out, mapped.pBits + static_cast<size_t>(oy) * pitch + static_cast<size_t>(ox) * 4, pitch);
          dup->UnMapDesktopSurface();
          ok = true;
        }
      } else {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) {
          D3D11_BOX box{static_cast<UINT>(ox), static_cast<UINT>(oy), 0, static_cast<UINT>(ox + out.width),
                        static_cast<UINT>(oy + out.height), 1};
          context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
          D3D11_MAPPED_SUBRESOURCE mapped{};
          if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            CopyRows(out, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch);
            context->Unmap(staging.Get(), 0);
            ok = true;
          }
        }
      }
      dup->ReleaseFrame();
      return ok ? CaptureResult::Frame : CaptureResult::Error;
    }
  }

  // ---- GDI ----

  bool InitGdi(std::string* why) {
    if (memDC) return true;
    if (!timer) {
      timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    screenDC = GetDC(nullptr);
    if (!screenDC) {
      *why = "GetDC(nullptr) failed";
      return false;
    }
    memDC = CreateCompatibleDC(screenDC);
    if (!memDC) {
      *why = "CreateCompatibleDC failed";
      ReleaseGdi();
      return false;
    }
    RECT probe{monitorRect.left, monitorRect.top, monitorRect.left + 8, monitorRect.top + 8};
    CapturedFrame f;
    if (!Blt(probe, f)) {
      *why = "BitBlt from the screen failed (error " + std::to_string(GetLastError()) + ")";
      ReleaseGdi();
      return false;
    }
    return true;
  }

  void ReleaseGdi() {
    if (memDC && oldBitmap) SelectObject(memDC, oldBitmap);
    if (dib) DeleteObject(dib);
    if (memDC) DeleteDC(memDC);
    if (screenDC) ReleaseDC(nullptr, screenDC);
    dib = nullptr;
    memDC = nullptr;
    screenDC = nullptr;
    oldBitmap = nullptr;
    dibBits = nullptr;
    dibW = dibH = 0;
  }

  bool EnsureDib(int w, int h) {
    if (dib && w <= dibW && h <= dibH) return true;
    const int nw = std::max(w, dibW);
    const int nh = std::max(h, dibH);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = nw;
    bmi.bmiHeader.biHeight = -nh;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP fresh = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!fresh || !bits) return false;
    HGDIOBJ prev = SelectObject(memDC, fresh);
    if (!oldBitmap) oldBitmap = prev;
    if (dib) DeleteObject(dib);
    dib = fresh;
    dibBits = static_cast<uint8_t*>(bits);
    dibW = nw;
    dibH = nh;
    return true;
  }

  bool Blt(const RECT& roi, CapturedFrame& out) {
    const RECT r = Intersect(roi, monitorRect);
    if (r.right <= r.left || r.bottom <= r.top) return false;
    if (!EnsureDib(r.right - r.left, r.bottom - r.top)) return false;
    PrepareFrame(out, r);
    const int64_t t0 = QpcNow();
    const BOOL ok = BitBlt(memDC, 0, 0, out.width, out.height, screenDC, r.left, r.top, SRCCOPY | CAPTUREBLT);
    const int64_t t1 = QpcNow();
    if (!ok) return false;
    GdiFlush();
    CopyRows(out, dibBits, static_cast<size_t>(dibW) * 4);
    out.qpc = t0 + (t1 - t0) / 2;
    return true;
  }

  void WaitUntil(int64_t target) {
    const int64_t now = QpcNow();
    if (target <= now) return;
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -std::max<int64_t>(1, (target - now) * 10000000 / freq);
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
      }
    }
    Sleep(static_cast<DWORD>((target - now) * 1000 / freq));
  }

  CaptureResult NextGdi(const RECT& roi, uint32_t timeoutMs, CapturedFrame& out) {
    const int64_t period = std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(freq) / (2.0 * refreshHz)));
    const int64_t deadline = QpcNow() + static_cast<int64_t>(timeoutMs) * freq / 1000;
    for (;;) {
      const int64_t now = QpcNow();
      if (nextTick == 0 || nextTick < now - period) nextTick = now;
      WaitUntil(nextTick);
      nextTick += period;
      if (!Blt(roi, out)) return CaptureResult::Error;
      const RECT r{out.originX, out.originY, out.originX + out.width, out.originY + out.height};
      const bool same = !previous.empty() && SameRect(r, previousRect) && previous.size() == out.pixels.size() &&
                        std::memcmp(previous.data(), out.pixels.data(), previous.size()) == 0;
      if (!same) {
        previous = out.pixels;
        previousRect = r;
        return CaptureResult::Frame;
      }
      if (QpcNow() >= deadline) return CaptureResult::Timeout;
    }
  }
};

ScreenCapture::ScreenCapture() : impl_(std::make_unique<Impl>()) {}
ScreenCapture::~ScreenCapture() = default;

bool ScreenCapture::Init(CaptureMode mode, HMONITOR monitor, const RECT& monitorRect, double refreshHz) {
  Impl& m = *impl_;
  m.mode = mode;
  m.monitor = monitor;
  m.monitorRect = monitorRect;
  m.refreshHz = refreshHz > 1 ? refreshHz : 60.0;
  std::string ddWhy;
  if (mode != CaptureMode::Gdi) {
    if (m.InitDd(&ddWhy)) {
      m.backend = CaptureBackend::DesktopDuplication;
      m.description = "DXGI Desktop Duplication (" + m.ddName + (m.systemMemory ? ", system-memory image" : "") + ")";
      return true;
    }
    m.ReleaseDd();
    if (mode == CaptureMode::DesktopDuplication) {
      m.description = "Desktop Duplication unavailable: " + ddWhy;
      return false;
    }
  }
  std::string gdiWhy;
  if (m.InitGdi(&gdiWhy)) {
    m.backend = CaptureBackend::Gdi;
    char rate[32];
    std::snprintf(rate, sizeof rate, "%.0f", 2.0 * m.refreshHz);
    m.description = std::string("GDI BitBlt at ~") + rate + " Hz";
    if (!ddWhy.empty()) m.description += " (Desktop Duplication unavailable: " + ddWhy + ")";
    return true;
  }
  m.description = "no capture backend works: " + (ddWhy.empty() ? std::string() : "DD: " + ddWhy + "; ") +
                  "GDI: " + gdiWhy;
  return false;
}

CaptureBackend ScreenCapture::backend() const { return impl_->backend; }
const std::string& ScreenCapture::description() const { return impl_->description; }
int ScreenCapture::accessLostCount() const { return impl_->accessLost; }

void ScreenCapture::Release() {
  auto fresh = std::make_unique<Impl>();
  fresh->backend = impl_->backend;
  fresh->description = impl_->description;
  fresh->accessLost = impl_->accessLost;
  impl_ = std::move(fresh);
}

CaptureResult ScreenCapture::Next(const RECT& roi, uint32_t timeoutMs, CapturedFrame& out) {
  switch (impl_->backend) {
    case CaptureBackend::DesktopDuplication: return impl_->NextDd(roi, timeoutMs, out);
    case CaptureBackend::Gdi: return impl_->NextGdi(roi, timeoutMs, out);
    case CaptureBackend::None: break;
  }
  return CaptureResult::Error;
}

void ScreenCapture::ResetChangeDetection() { impl_->previous.clear(); }

bool ScreenCapture::GrabGdi(const RECT& roi, CapturedFrame& out) {
  std::string why;
  if (!impl_->InitGdi(&why)) return false;
  return impl_->Blt(roi, out);
}

}  // namespace snowharness
