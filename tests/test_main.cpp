#include <cstdio>
#include <cstring>
#include "test.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failedCases = 0;
  for (auto& c : snowtest::Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    int before = snowtest::Failures();
    std::printf("[ RUN  ] %s\n", c.name);
    c.fn();
    ++run;
    if (snowtest::Failures() != before) {
      ++failedCases;
      std::printf("[ FAIL ] %s\n", c.name);
    } else {
      std::printf("[  OK  ] %s\n", c.name);
    }
  }
  std::printf("\n%d test cases, %d checks, %d failed cases\n", run, snowtest::Checks(), failedCases);
  return failedCases == 0 ? 0 : 1;
}
