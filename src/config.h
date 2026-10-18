#pragma once
#include <cstdint>
#include <string>

// 离线任务可选的模型库（实时播放仍用通用模型）
enum class SRModel {
  Realistic = 0,     // 真人/实拍，标准 x4
  Anime = 1,         // 动画，标准 x4
  RealisticHQ = 2,   // 真人/实拍，高质量 x4
  AnimeHQ = 3,       // 动画，高质量 x4
  AnimeX2 = 4,       // 动画，标准 x2
  AnimeX3 = 5,       // 动画，标准 x3
};

inline bool srModelIsAnime(SRModel m) {
  return m == SRModel::Anime || m == SRModel::AnimeHQ ||
         m == SRModel::AnimeX2 || m == SRModel::AnimeX3;
}

inline bool srModelIsHQ(SRModel m) {
  return m == SRModel::RealisticHQ || m == SRModel::AnimeHQ;
}

inline int srModelScale(SRModel m) {
  switch (m) {
    case SRModel::AnimeX2: return 2;
    case SRModel::AnimeX3: return 3;
    default: return 4;
  }
}

inline const char* srModelParamName(SRModel m) {
  switch (m) {
    case SRModel::Anime: return "realesr-animevideov3-x4.param";
    case SRModel::AnimeX2: return "realesr-animevideov3-x2.param";
    case SRModel::AnimeX3: return "realesr-animevideov3-x3.param";
    case SRModel::RealisticHQ: return "realesrgan-x4plus.param";
    case SRModel::AnimeHQ: return "realesrgan-x4plus-anime.param";
    case SRModel::Realistic:
    default: return "RealESRGAN_General_x4_v3.param";
  }
}

inline const char* srModelBinName(SRModel m) {
  switch (m) {
    case SRModel::Anime: return "realesr-animevideov3-x4.bin";
    case SRModel::AnimeX2: return "realesr-animevideov3-x2.bin";
    case SRModel::AnimeX3: return "realesr-animevideov3-x3.bin";
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
    case SRModel::AnimeX2: return "anime_x2";
    case SRModel::AnimeX3: return "anime_x3";
    case SRModel::Realistic:
    default: return "realistic";
  }
}

inline SRModel srModelFromId(const std::string& id) {
  if (id == "anime") return SRModel::Anime;
  if (id == "realistic_hq") return SRModel::RealisticHQ;
  if (id == "anime_hq") return SRModel::AnimeHQ;
  if (id == "anime_x2") return SRModel::AnimeX2;
  if (id == "anime_x3") return SRModel::AnimeX3;
  return SRModel::Realistic;
}

// 按目标/源比例选标准模型：动画优先 2x/3x，真人目前只有 4x 标准模型。
// HQ 档保持 4x（高质量模型没有 2x/3x 资产）。
inline SRModel selectSRModel(SRModel preferred, int srcW, int targetW) {
  if (srModelIsHQ(preferred)) return preferred;
  if (srcW <= 0 || targetW <= srcW) return preferred;
  const int64_t tw = targetW;
  const int64_t sw = srcW;
  if (srModelIsAnime(preferred)) {
    if (tw <= sw * 2) return SRModel::AnimeX2;
    if (tw <= sw * 3) return SRModel::AnimeX3;
    return SRModel::Anime;
  }
  // 真人暂只有 4x 标准/高质量模型（SRMD x2/x3 与当前 NCNN 不兼容，未启用）。
  return preferred;
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
