// Small deterministic PRNG (PCG32). Deterministic seeds keep unit tests stable.
#pragma once
#include <cstdint>

namespace snow {

class Rng {
 public:
  explicit Rng(uint64_t seed = 0x853c49e6748fea9bULL) { reseed(seed); }
  void reseed(uint64_t seed) {
    state_ = 0;
    inc_ = (seed << 1u) | 1u;
    next();
    state_ += seed;
    next();
  }
  uint32_t next() {
    uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
  }
  // [0, 1)
  float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
  float range(float a, float b) { return a + (b - a) * uniform(); }
  int rangeInt(int a, int bInclusive) {
    if (bInclusive <= a) return a;
    return a + static_cast<int>(next() % static_cast<uint32_t>(bInclusive - a + 1));
  }
  bool chance(float p) { return uniform() < p; }

 private:
  uint64_t state_ = 0, inc_ = 0;
};

}  // namespace snow
