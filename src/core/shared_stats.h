// Live counters the overlay publishes in a named shared-memory block
// ("Local\DesktopSnow.<instance>.Stats") so SnowHarness can see whether it is
// rendering without parsing logs. Plain data; writers use 32/64-bit aligned stores.
#pragma once
#include <cstdint>

namespace snow {

constexpr uint32_t kSharedStatsVersion = 1;
constexpr int kSharedStatsMaxOverlays = 8;

struct SharedOverlayInfo {
  int32_t left, top, right, bottom;  // monitor rectangle, physical px
  int32_t shown;                     // 1 = overlay window visible and rendering
  int32_t reserved;
};

struct SharedStats {
  uint32_t version;
  uint32_t overlays;  // entries used in `overlay`
  int64_t frames;     // simulated + rendered frames since start
  int64_t presents;   // swap-chain presents since start
  int64_t hiddenWaits;
  SharedOverlayInfo overlay[kSharedStatsMaxOverlays];
};

}  // namespace snow
