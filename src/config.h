#pragma once
#include <string>

// 应用配置：路径等可注入（测试可用临时目录覆盖），避免硬编码
struct Config {
  std::string modelBase;   // NCNN 模型目录（末尾带 '/'）
  std::string dataRoot;    // 任务数据根目录（其下建 sr-tasks/）

  std::string srParam() const { return modelBase + "RealESRGAN_General_x4_v3.param"; }
  std::string srBin() const { return modelBase + "RealESRGAN_General_x4_v3.bin"; }

  // 默认配置：模型目录取仓库内固定路径，数据根取 %APPDATA%/DominoPlayer
  static Config defaults();
};
