#include "test_util.h"
#include <cstring>
#include <cstdlib>

// 全局测试参数：资源目录、数据目录（由 test_main 解析命令行设置）
namespace testcfg {
inline std::string& assetsDir() { static std::string d; return d; }   // 测试视频所在目录
inline std::string& dataDir() { static std::string d; return d; }     // 任务临时数据目录
}

inline std::string assetPath(const char* name) {
  return testcfg::assetsDir() + "/" + name;
}
