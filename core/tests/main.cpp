// Test runner: `lt_tests [substring...]` runs every test whose name contains
// one of the substrings (all tests when none is given).
#include <chrono>
#include <cstring>

#include "framework.hpp"

int main(int argc, char** argv) {
  int ran = 0;
  int failed_cases = 0;
  auto t_all = std::chrono::steady_clock::now();
  for (const auto& c : tf::registry()) {
    bool selected = argc < 2;
    for (int i = 1; i < argc; ++i) {
      if (std::strstr(c.name, argv[i])) selected = true;
    }
    if (!selected) continue;
    ++ran;
    tf::current_failed() = false;
    auto t0 = std::chrono::steady_clock::now();
    c.fn();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-4s %-48s %8.1f ms\n", tf::current_failed() ? "FAIL" : "ok", c.name, ms);
    if (tf::current_failed()) ++failed_cases;
  }
  double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_all).count();
  std::printf("\n%d test(s), %d failed, %d check failure(s), %.1f ms\n", ran, failed_cases, tf::failures(), total);
  if (ran == 0) {
    std::printf("no test matched\n");
    return 2;
  }
  return failed_cases ? 1 : 0;
}
