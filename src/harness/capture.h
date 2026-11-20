// Screen capture for SnowHarness: DXGI Desktop Duplication with a GDI BitBlt
// fallback. Coordinates are physical virtual-screen pixels.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace snowharness {

enum class CaptureMode { Auto, DesktopDuplication, Gdi };
enum class CaptureBackend { None, DesktopDuplication, Gdi };

struct CapturedFrame {
  std::vector<uint8_t> pixels;  // top-down BGRA, stride = width * 4
  int originX = 0;              // absolute screen position of pixel (0,0)
  int originY = 0;
  int width = 0;
  int height = 0;
  int stride = 0;
  int64_t qpc = 0;  // DD: LastPresentTime of the image; GDI: middle of the BitBlt
};

enum class CaptureResult { Frame, Timeout, Error };

class ScreenCapture {
 public:
  ScreenCapture();
  ~ScreenCapture();
  ScreenCapture(const ScreenCapture&) = delete;
  ScreenCapture& operator=(const ScreenCapture&) = delete;

  // monitorRect is the capture monitor in physical virtual-screen pixels;
  // refreshHz paces the GDI fallback (it samples at twice that rate).
  bool Init(CaptureMode mode, HMONITOR monitor, const RECT& monitorRect, double refreshHz);
  CaptureBackend backend() const;
  const std::string& description() const;  // backend details and fallback reason
  int accessLostCount() const;

  // Waits up to timeoutMs for a new screen image and copies `roi` (clamped to
  // the monitor). GDI only returns images that differ from the previous one
  // for the same ROI.
  CaptureResult Next(const RECT& roi, uint32_t timeoutMs, CapturedFrame& out);
  // Makes the next GDI grab count as new even if the image is unchanged.
  void ResetChangeDetection();
  // One-shot synchronous GDI grab regardless of the backend (static scenes).
  bool GrabGdi(const RECT& roi, CapturedFrame& out);
  // Frees the duplication and GPU objects so they don't load DWM during later
  // phases; backend(), description() and accessLostCount() stay valid.
  void Release();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* CaptureBackendName(CaptureBackend b);

}  // namespace snowharness
