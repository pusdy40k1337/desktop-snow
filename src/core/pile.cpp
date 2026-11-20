#include "pile.h"

#include <algorithm>
#include <cmath>

#include "geom.h"

namespace snow {

void PileModel::reset(float width, const PileParams& p) {
  p_ = p;
  if (p_.columnStep < 1.f) p_.columnStep = 1.f;
  width_ = std::max(0.f, width);
  int n = std::max(1, static_cast<int>(std::ceil(width_ / p_.columnStep)));
  h_.assign(static_cast<size_t>(n), 0.f);
  ++version_;
}

void PileModel::setParams(const PileParams& p) {
  float oldStep = p_.columnStep;
  p_ = p;
  if (p_.columnStep < 1.f) p_.columnStep = 1.f;
  if (std::fabs(oldStep - p_.columnStep) > 1e-3f && !h_.empty()) {
    std::vector<float> old = h_;
    int n = std::max(1, static_cast<int>(std::ceil(width_ / p_.columnStep)));
    h_.assign(static_cast<size_t>(n), 0.f);
    int on = static_cast<int>(old.size());
    for (int i = 0; i < n; ++i) {
      float src = ((i + 0.5f) * p_.columnStep) / oldStep - 0.5f;
      int i0 = static_cast<int>(std::floor(src));
      float t = src - static_cast<float>(i0);
      int a = std::clamp(i0, 0, on - 1), b = std::clamp(i0 + 1, 0, on - 1);
      h_[static_cast<size_t>(i)] = old[static_cast<size_t>(a)] * (1.f - t) + old[static_cast<size_t>(b)] * t;
    }
  }
  recap();
  ++version_;
}

int PileModel::columnAt(float localX) const {
  int i = static_cast<int>(std::floor(localX / p_.columnStep));
  return std::clamp(i, 0, columns() - 1);
}

float PileModel::heightAt(float localX) const {
  if (h_.empty() || localX < 0.f || localX > width_) return 0.f;
  float u = localX / p_.columnStep - 0.5f;
  int i0 = static_cast<int>(std::floor(u));
  float t = u - static_cast<float>(i0);
  int n = columns();
  float a = h_[static_cast<size_t>(std::clamp(i0, 0, n - 1))];
  float b = h_[static_cast<size_t>(std::clamp(i0 + 1, 0, n - 1))];
  return a * (1.f - t) + b * t;
}

float PileModel::capAt(int i) const {
  float cap = p_.maxHeight;
  if (p_.cornerRadius > 0.f) {
    float c = columnCenter(i);
    float dx = std::min(c, width_ - c);
    if (dx < p_.cornerRadius) cap *= 0.25f + 0.75f * smoothstep(0.f, p_.cornerRadius, dx);
  }
  return std::max(0.f, cap);
}

float PileModel::mass() const {
  float m = 0;
  for (float v : h_) m += v;
  return m;
}

float PileModel::peak() const {
  float m = 0;
  for (float v : h_) m = std::max(m, v);
  return m;
}

float PileModel::deposit(float localX, float massIn, const std::vector<uint8_t>* exposed) {
  if (h_.empty() || massIn <= 0.f) return massIn;
  static const float kKernel[5] = {0.1f, 0.2f, 0.4f, 0.2f, 0.1f};
  int c = columnAt(localX);
  int n = columns();
  float overflow = 0.f;
  for (int k = -2; k <= 2; ++k) {
    float part = massIn * kKernel[k + 2];
    int i = c + k;
    if (i < 0 || i >= n || (exposed && i < static_cast<int>(exposed->size()) && !(*exposed)[static_cast<size_t>(i)])) {
      overflow += part;
      continue;
    }
    float cap = capAt(i);
    float& h = h_[static_cast<size_t>(i)];
    float next = h + part;
    if (next > cap) {
      overflow += next - cap;
      next = cap;
    }
    h = std::max(h, next);
  }
  ++version_;
  return overflow;
}

float PileModel::settle(float heightPerSecond, float dt, const std::vector<uint8_t>* exposed) {
  if (h_.empty() || heightPerSecond <= 0.f || dt <= 0.f) return 0.f;
  float add = heightPerSecond * dt, added = 0.f;
  int n = columns();
  for (int i = 0; i < n; ++i) {
    if (exposed && i < static_cast<int>(exposed->size()) && !(*exposed)[static_cast<size_t>(i)]) continue;
    float cap = capAt(i);
    float& h = h_[static_cast<size_t>(i)];
    if (h >= cap) continue;
    float next = std::min(cap, h + add);
    added += next - h;
    h = next;
  }
  if (added > 0.f) ++version_;
  return added;
}

float PileModel::relax(float dt) {
  int n = columns();
  if (n == 0 || dt <= 0.f) return 0.f;
  const float k = std::min(1.f, p_.relaxRate * dt);
  const float maxDiff = p_.maxSlope * p_.columnStep;
  float spilled = 0.f, maxDelta = 0.f;

  tmp_ = h_;
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 0) {
      for (int i = 0; i + 1 < n; ++i) {
        float& a = h_[static_cast<size_t>(i)];
        float& b = h_[static_cast<size_t>(i + 1)];
        float d = a - b;
        if (d > maxDiff) {
          float t = 0.5f * (d - maxDiff) * k;
          a -= t;
          b += t;
        } else if (d < -maxDiff) {
          float t = 0.5f * (-d - maxDiff) * k;
          b -= t;
          a += t;
        }
      }
    } else {
      for (int i = n - 2; i >= 0; --i) {
        float& a = h_[static_cast<size_t>(i)];
        float& b = h_[static_cast<size_t>(i + 1)];
        float d = a - b;
        if (d > maxDiff) {
          float t = 0.5f * (d - maxDiff) * k;
          a -= t;
          b += t;
        } else if (d < -maxDiff) {
          float t = 0.5f * (-d - maxDiff) * k;
          b -= t;
          a += t;
        }
      }
    }
    // Open ends: virtual empty neighbours, so snow slides off the edges.
    float& first = h_.front();
    if (first > maxDiff) {
      float t = (first - maxDiff) * k * 0.5f;
      first -= t;
      spilled += t;
    }
    float& last = h_.back();
    if (last > maxDiff) {
      float t = (last - maxDiff) * k * 0.5f;
      last -= t;
      spilled += t;
    }
  }

  // Gentle diffusion with reflecting boundaries (mass conserving).
  if (p_.diffusion > 0.f && n > 2) {
    float alpha = std::clamp(p_.diffusion * dt / (p_.columnStep * p_.columnStep), 0.f, 0.2f);
    std::vector<float> src = h_;
    for (int i = 0; i < n; ++i) {
      float c = src[static_cast<size_t>(i)];
      float l = i > 0 ? src[static_cast<size_t>(i - 1)] : c;
      float r = i + 1 < n ? src[static_cast<size_t>(i + 1)] : c;
      h_[static_cast<size_t>(i)] = c + alpha * (l - 2.f * c + r);
    }
  }

  for (int i = 0; i < n; ++i) {
    float& h = h_[static_cast<size_t>(i)];
    if (h < 0.f) h = 0.f;
    float cap = capAt(i);
    if (h > cap) {
      spilled += h - cap;
      h = cap;
    }
    maxDelta = std::max(maxDelta, std::fabs(h - tmp_[static_cast<size_t>(i)]));
  }
  if (maxDelta > 0.01f) ++version_;
  return spilled;
}

float PileModel::resize(float oldLeft, float oldRight, float newLeft, float newRight) {
  float newW = newRight - newLeft;
  if (newW <= 0.f) {
    float m = mass();
    width_ = 0.f;
    h_.assign(1, 0.f);
    ++version_;
    return m;
  }
  int n = columns();
  int nNew = std::max(1, static_cast<int>(std::ceil(newW / p_.columnStep)));
  bool leftSame = std::fabs(newLeft - oldLeft) < 0.5f;
  bool rightSame = std::fabs(newRight - oldRight) < 0.5f;
  if (nNew == n && std::fabs(newW - width_) < 0.5f) {
    width_ = newW;
    return 0.f;
  }
  std::vector<float> next(static_cast<size_t>(nNew), 0.f);
  float spilled = 0.f;
  if (leftSame) {
    for (int i = 0; i < n; ++i) {
      if (i < nNew) next[static_cast<size_t>(i)] = h_[static_cast<size_t>(i)];
      else spilled += h_[static_cast<size_t>(i)];
    }
  } else if (rightSame) {
    for (int k = 0; k < n; ++k) {
      int io = n - 1 - k, in = nNew - 1 - k;
      if (in >= 0) next[static_cast<size_t>(in)] = h_[static_cast<size_t>(io)];
      else spilled += h_[static_cast<size_t>(io)];
    }
  } else {
    // Both ends moved (maximise/restore, snap, DPI change): stretch the profile.
    for (int i = 0; i < nNew; ++i) {
      float src = ((i + 0.5f) / static_cast<float>(nNew)) * static_cast<float>(n) - 0.5f;
      int i0 = static_cast<int>(std::floor(src));
      float t = src - static_cast<float>(i0);
      int a = std::clamp(i0, 0, n - 1), b = std::clamp(i0 + 1, 0, n - 1);
      next[static_cast<size_t>(i)] = h_[static_cast<size_t>(a)] * (1.f - t) + h_[static_cast<size_t>(b)] * t;
    }
  }
  h_.swap(next);
  width_ = newW;
  for (int i = 0; i < nNew; ++i) {
    float cap = capAt(i);
    float& h = h_[static_cast<size_t>(i)];
    if (h > cap) {
      spilled += h - cap;
      h = cap;
    }
  }
  ++version_;
  return spilled;
}

void PileModel::scaleHeights(float s) {
  if (s <= 0.f) return;
  for (float& h : h_) h *= s;
  recap();
  ++version_;
}

static float removeColumns(std::vector<float>& h, int i0, int i1, float fraction, float centre, float half,
                           float step, float massPerParticle, int maxDebris, std::vector<PileDebris>& out) {
  // First pass: total removal so that particle mass is uniform and the debris
  // count is proportional to the removed mass.
  auto weightAt = [&](int i) {
    if (half <= 0.f) return fraction;
    float c = (i + 0.5f) * step;
    float d = std::fabs(c - centre) / half;
    return d >= 1.f ? 0.f : fraction * (1.f - d * d);
  };
  float total = 0.f;
  for (int i = i0; i < i1; ++i) total += h[static_cast<size_t>(i)] * weightAt(i);
  if (total <= 1e-5f) return 0.f;
  int count = massPerParticle > 0.f ? static_cast<int>(std::floor(total / massPerParticle)) : 0;
  if (count == 0 && massPerParticle > 0.f && total >= 0.35f * massPerParticle) count = 1;
  count = std::min(count, std::max(0, maxDebris));
  float per = count > 0 ? total / static_cast<float>(count) : 0.f;
  float acc = 0.f, removed = 0.f;
  int emitted = 0;
  int lastWithSnow = i0;
  float lastHeight = 0.f;
  for (int i = i0; i < i1; ++i) {
    float before = h[static_cast<size_t>(i)];
    float r = before * weightAt(i);
    if (r <= 0.f) continue;
    h[static_cast<size_t>(i)] = before - r;
    removed += r;
    acc += r;
    lastWithSnow = i;
    lastHeight = before;
    while (emitted < count && acc >= per * 0.999f) {
      out.push_back({(i + 0.5f) * step, before, per});
      acc -= per;
      ++emitted;
    }
  }
  while (emitted < count) {  // float round-off
    out.push_back({(lastWithSnow + 0.5f) * step, lastHeight, per});
    ++emitted;
  }
  return removed;
}

float PileModel::scatter(float fraction, float massPerParticle, int maxDebris, std::vector<PileDebris>& out) {
  fraction = clampf(fraction, 0.f, 1.f);
  if (fraction <= 0.f || h_.empty()) return 0.f;
  float removed = removeColumns(h_, 0, columns(), fraction, 0.f, 0.f, p_.columnStep, massPerParticle, maxDebris, out);
  if (fraction >= 0.999f) std::fill(h_.begin(), h_.end(), 0.f);
  ++version_;
  return removed;
}

float PileModel::removeRange(float x0, float x1, float fraction, float massPerParticle, int maxDebris,
                             std::vector<PileDebris>& out) {
  fraction = clampf(fraction, 0.f, 1.f);
  if (fraction <= 0.f || h_.empty() || x1 <= x0) return 0.f;
  int i0 = std::max(0, static_cast<int>(std::floor(x0 / p_.columnStep)));
  int i1 = std::min(columns(), static_cast<int>(std::ceil(x1 / p_.columnStep)));
  if (i1 <= i0) return 0.f;
  float centre = 0.5f * (x0 + x1), half = 0.5f * (x1 - x0) + p_.columnStep;
  float removed = removeColumns(h_, i0, i1, fraction, centre, half, p_.columnStep, massPerParticle, maxDebris, out);
  if (removed > 0.f) ++version_;
  return removed;
}

void PileModel::melt(float pxPerSecond, float dt) {
  if (pxPerSecond <= 0.f || dt <= 0.f) return;
  float d = pxPerSecond * dt;
  bool any = false;
  for (float& h : h_) {
    if (h > 0.f) {
      h = std::max(0.f, h - d);
      any = true;
    }
  }
  if (any) ++version_;
}

void PileModel::clear() {
  std::fill(h_.begin(), h_.end(), 0.f);
  ++version_;
}

void PileModel::recap() {
  for (int i = 0; i < columns(); ++i) h_[static_cast<size_t>(i)] = std::min(h_[static_cast<size_t>(i)], capAt(i));
}

}  // namespace snow
