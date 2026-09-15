#include "config.h"
#include <cstdlib>

// 默认模型目录：优先环境变量 DOMINO_MODEL_DIR，其次构建时注入的仓库路径（CMake 定义），
// 最后回退相对路径。避免在源码里硬编码任何机器/用户路径。
#ifndef DOMINO_DEFAULT_MODEL_DIR
#define DOMINO_DEFAULT_MODEL_DIR "assets/models/sr/ncnn/"
#endif

Config Config::defaults() {
  Config c;
  const char* env = getenv("DOMINO_MODEL_DIR");
  c.modelBase = (env && *env) ? env : DOMINO_DEFAULT_MODEL_DIR;
  const char* a = getenv("APPDATA");
  c.dataRoot = std::string(a ? a : ".") + "/DominoPlayer";
  return c;
}
