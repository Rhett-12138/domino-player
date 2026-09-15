#pragma once
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

// 轻量测试框架：TEST(name) 注册，runAllTests() 执行；断言失败计入并继续。
struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& testRegistry() { static std::vector<TestCase> r; return r; }

#define TEST(name) \
  static void test_##name(); \
  static struct reg_##name { reg_##name() { testRegistry().push_back({#name, test_##name}); } } regobj_##name; \
  static void test_##name()

inline int& testFailures() { static int f = 0; return f; }
inline int& testPasses() { static int p = 0; return p; }

#define EXPECT_TRUE(cond) \
  do { if (cond) { ++testPasses(); } else { ++testFailures(); \
    printf("  FAIL %s:%d EXPECT_TRUE(%s)\n", __FILE__, __LINE__, #cond); } } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b) \
  do { auto va = (a); auto vb = (b); if (va == vb) { ++testPasses(); } else { ++testFailures(); \
    printf("  FAIL %s:%d EXPECT_EQ(%s, %s)  got %lld vs %lld\n", __FILE__, __LINE__, #a, #b, \
      (long long)va, (long long)vb); } } while (0)

#define EXPECT_STREQ(a, b) \
  do { std::string sa_((a) ? (a) : ""); std::string sb_((b) ? (b) : ""); \
    if (sa_ == sb_) { ++testPasses(); } else { ++testFailures(); \
      printf("  FAIL %s:%d EXPECT_STREQ got '%s' vs '%s'\n", __FILE__, __LINE__, sa_.c_str(), sb_.c_str()); } } while (0)

#define EXPECT_NEAR(a, b, eps) \
  do { double va = (a); double vb = (b); if (fabs(va - vb) <= (eps)) { ++testPasses(); } else { ++testFailures(); \
    printf("  FAIL %s:%d EXPECT_NEAR(%s, %s) got %.4f vs %.4f\n", __FILE__, __LINE__, #a, #b, va, vb); } } while (0)

inline int runAllTests() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  for (auto& t : testRegistry()) {
    int before = testFailures();
    printf("== %s\n", t.name);
    fflush(stdout);
    t.fn();
    if (testFailures() == before) printf("  PASS\n");
    fflush(stdout);
  }
  printf("\n%d passed, %d failed\n", testPasses(), testFailures());
  fflush(stdout);
  return testFailures() == 0 ? 0 : 1;
}
