#pragma once
#include <string>

// 拼接目录与文件名：无论 base 末尾有无分隔符都得到合法路径。
// 前端注入的模型目录常不带末尾分隔符，不能依赖调用方自行补全。
inline std::string joinModelPath(const std::string& base, const char* name) {
  if (base.empty()) return name;
  const char last = base.back();
  if (last == '/' || last == '\\') return base + name;
  return base + "/" + name;
}

// 应用配置：路径等可注入（测试可用临时目录覆盖），避免硬编码
struct Config {
  std::string modelBase;   // NCNN 模型目录（末尾分隔符可有可无）
  std::string dataRoot;    // 任务数据根目录（其下建 sr-tasks/）

  std::string srParam() const { return joinModelPath(modelBase, "RealESRGAN_General_x4_v3.param"); }
  std::string srBin() const { return joinModelPath(modelBase, "RealESRGAN_General_x4_v3.bin"); }

  // 默认配置：模型目录取仓库内固定路径，数据根取 %APPDATA%/DominoPlayer
  static Config defaults();
};
