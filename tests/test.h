// Tiny dependency-free unit test framework.
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace snowtest {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}
inline int& Failures() {
  static int f = 0;
  return f;
}
inline int& Checks() {
  static int c = 0;
  return c;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline void Fail(const char* file, int line, const std::string& msg) {
  ++Failures();
  std::printf("  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace snowtest

#define SNOW_CAT2(a, b) a##b
#define SNOW_CAT(a, b) SNOW_CAT2(a, b)
#define TEST(name)                                                                  \
  static void SNOW_CAT(test_fn_, name)();                                           \
  static snowtest::Registrar SNOW_CAT(test_reg_, name)(#name, &SNOW_CAT(test_fn_, name)); \
  static void SNOW_CAT(test_fn_, name)()

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++snowtest::Checks();                                               \
    if (!(cond)) snowtest::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_MSG(cond, msg)                                                          \
  do {                                                                                \
    ++snowtest::Checks();                                                             \
    if (!(cond)) snowtest::Fail(__FILE__, __LINE__, std::string("CHECK(" #cond "): ") + (msg)); \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                              \
  do {                                                                                     \
    ++snowtest::Checks();                                                                  \
    double snow_a_ = static_cast<double>(a), snow_b_ = static_cast<double>(b);             \
    if (!(std::fabs(snow_a_ - snow_b_) <= static_cast<double>(eps))) {                     \
      char snow_buf_[256];                                                                 \
      std::snprintf(snow_buf_, sizeof snow_buf_, "CHECK_NEAR(" #a ", " #b ") %.6g vs %.6g (eps %.3g)", \
                    snow_a_, snow_b_, static_cast<double>(eps));                           \
      snowtest::Fail(__FILE__, __LINE__, snow_buf_);                                       \
    }                                                                                      \
  } while (0)
