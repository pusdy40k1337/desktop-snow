// Which DWM composition the next frame targets. Portable, unit-tested.
#pragma once
#include <algorithm>

namespace snow {

// next:       earliest composition the frame can still make (seconds)
// lastTarget: composition the previous frame targeted
// period:     composition period
// fpsCap:     frame cap for idle snowfall; ignored while `uncapped`
// uncapped:   a window is moving (or measurement mode): every composition gets a
//             fresh frame. A cap below the refresh rate makes the overlay skip every
//             other composition, and a dragged window then shows a pile one whole
//             refresh behind on those frames (measured: 50 fps on a 100 Hz display).
inline double ChooseFrameTarget(double next, double lastTarget, double period, int fpsCap, bool uncapped) {
  double minGap = period * 0.5;  // never target the same composition twice
  if (!uncapped && fpsCap > 0) minGap = std::max(minGap, 1.0 / fpsCap - period * 0.5);
  while (next - lastTarget < minGap) next += period;
  return next;
}

}  // namespace snow
