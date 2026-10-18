#pragma once
#include <string>

// 离线任务可选的模型库（实时播放仍用通用模型）
enum class SRModel {
  Realistic = 0,     // 真人/实拍，标准
  Anime = 1,         // 动画，标准
  RealisticHQ = 2,   // 真人/实拍，高质量
  AnimeHQ = 3,       // 动画，高质量
};

inline bool srModelIsAnime(SRModel m) {
  return m == SRModel::Anime || m == SRModel::AnimeHQ;
}

inline bool srModelIsHQ(SRModel m) {
  return m == SRModel::RealisticHQ || m == SRModel::AnimeHQ;
}

inline const char* srModelParamName(SRModel m) {
  switch (m) {
    case SRModel::Anime: return "realesr-animevideov3-x4.param";
    case SRModel::RealisticHQ: return "realesrgan-x4plus.param";
    case SRModel::AnimeHQ: return "realesrgan-x4plus-anime.param";
    case SRModel::Realistic:
    default: return "RealESRGAN_General_x4_v3.param";
  }
}

inline const char* srModelBinName(SRModel m) {
  switch (m) {
    case SRModel::Anime: return "realesr-animevideov3-x4.bin";
    case SRModel::RealisticHQ: return "realesrgan-x4plus.bin";
    case SRModel::AnimeHQ: return "realesrgan-x4plus-anime.bin";
    case SRModel::Realistic:
    default: return "RealESRGAN_General_x4_v3.bin";
  }
}

inline const char* srModelId(SRModel m) {
  switch (m) {
    case SRModel::Anime: return "anime";
    case SRModel::RealisticHQ: return "realistic_hq";
    case SRModel::AnimeHQ: return "anime_hq";
    case SRModel::Realistic:
    default: return "realistic";
  }
}

inline SRModel srModelFromId(const std::string& id) {
  if (id == "anime") return SRModel::Anime;
  if (id == "realistic_hq") return SRModel::RealisticHQ;
  if (id == "anime_hq") return SRModel::AnimeHQ;
  return SRModel::Realistic;
}

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

  std::string srParam() const { return joinModelPath(modelBase, srModelParamName(SRModel::Realistic)); }
  std::string srBin() const { return joinModelPath(modelBase, srModelBinName(SRModel::Realistic)); }
  std::string srParam(SRModel m) const { return joinModelPath(modelBase, srModelParamName(m)); }
  std::string srBin(SRModel m) const { return joinModelPath(modelBase, srModelBinName(m)); }

  // 默认配置：模型目录取仓库内固定路径，数据根取 %APPDATA%/DominoPlayer
  static Config defaults();
};
