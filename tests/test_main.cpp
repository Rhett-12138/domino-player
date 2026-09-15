#include "test_common.h"
#include <filesystem>

// 用法: domino_tests [--assets <dir>] [--data <dir>]
// 默认 assets=./tests/assets(工作目录下), data=系统临时目录
int main(int argc, char** argv) {
  testcfg::assetsDir() = "tests/assets";
  testcfg::dataDir() = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/domino_tests";
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) testcfg::assetsDir() = argv[++i];
    else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) testcfg::dataDir() = argv[++i];
  }
  std::error_code ec;
  std::filesystem::create_directories(testcfg::dataDir(), ec);
  return runAllTests();
}
