// Tiny dependency-free test framework.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tf {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back(Case{n, f}); }
};

inline int& failures() {
  static int f = 0;
  return f;
}

inline bool& current_failed() {
  static bool b = false;
  return b;
}

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  current_failed() = true;
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
}

inline int env_int(const char* name, int def) {
  const char* v = std::getenv(name);
  return v && *v ? std::atoi(v) : def;
}

inline bool verbose() { return env_int("LT_VERBOSE", 0) != 0; }

}  // namespace tf

#define TEST(name)                                         \
  static void name();                                      \
  static ::tf::Registrar tf_reg_##name(#name, &name);      \
  static void name()

#define CHECK(cond)                                  \
  do {                                               \
    if (!(cond)) ::tf::fail(__FILE__, __LINE__, #cond); \
  } while (0)

#define REQUIRE(cond)                                  \
  do {                                                 \
    if (!(cond)) {                                     \
      ::tf::fail(__FILE__, __LINE__, #cond);           \
      return;                                          \
    }                                                  \
  } while (0)

#define CHECK_MSG(cond, msg)                                                \
  do {                                                                      \
    if (!(cond)) ::tf::fail(__FILE__, __LINE__, std::string(#cond) + " :: " + (msg)); \
  } while (0)

#define REQUIRE_MSG(cond, msg)                                                \
  do {                                                                        \
    if (!(cond)) {                                                            \
      ::tf::fail(__FILE__, __LINE__, std::string(#cond) + " :: " + (msg));    \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    auto tf_a = (a);                                                                          \
    auto tf_b = (b);                                                                          \
    if (!(tf_a == tf_b))                                                                      \
      ::tf::fail(__FILE__, __LINE__, std::string(#a " == " #b " :: ") + ::tf::show(tf_a) + " vs " + ::tf::show(tf_b)); \
  } while (0)

namespace tf {
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return s ? show(std::string(s)) : "null"; }
inline std::string show(bool b) { return b ? "true" : "false"; }
template <typename T>
std::string show(const T& v) {
  return std::to_string(v);
}
}  // namespace tf
