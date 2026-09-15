#pragma once
#include <cmath>

// 基础美颜/滤镜的纯数学实现（与 src/render.cpp 中 HLSL 的 skinProb/applyFilter 镜像一致）。
// 用途：headless 单测覆盖；改动时两处需同步。
namespace beautymath {

// 肤色判定：偏暖 + 通道差适中 + 亮度范围。输入/输出均为 0..1。
inline float skinProb(float r, float g, float b) {
  float rg = r - g;
  float rb = r - b;
  if (rg < 0.02f || rb < rg * 0.6f) return 0.0f;      // 不偏暖则非肤色
  float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  float d = mx - mn;
  float amp = 1.0f - std::fabs(d - 0.18f) * 3.5f;       // 通道差在 ~0.18 附近
  float lum = 1.0f - std::fabs(r - 0.60f) * 1.8f;       // 亮度范围（更宽容）
  if (amp < 0.f) amp = 0.f; else if (amp > 1.f) amp = 1.f;
  if (lum < 0.f) lum = 0.f; else if (lum > 1.f) lum = 1.f;
  return amp * lum;
}

// 磨皮：肤色区向模糊色混合（强化 1.2x）。
inline float blurBlend(float c, float blur, float skin, float strength) {
  float k = skin * strength * 1.2f;
  if (k > 1.f) k = 1.f;
  return c + (blur - c) * k;
}

// 美白：肤色区向白靠拢（80%）。
inline float whiteBlend(float c, float skin, float strength) {
  return c + skin * strength * (1.0f - c) * 0.8f;
}

// 滤镜（镜像 applyFilter）。idx/amt 0..1。修改需同步 render.cpp。
inline void applyFilter(float& r, float& g, float& b, float idx, float amt) {
  float l = 0.299f * r + 0.587f * g + 0.114f * b;
  float fr = r, fg = g, fb = b;
  if (idx < 0.5f) {            // 原图
  } else if (idx < 1.5f) {     // 黑白
    fr = fg = fb = l;
  } else if (idx < 2.5f) {     // 日系清新：提亮、偏青、低对比
    fr = r * 1.06f + 0.045f; fg = g; fb = b * 1.12f + 0.045f;
    fr = l + (fr - l) * 0.88f; fg = l + (fg - l) * 0.88f; fb = l + (fb - l) * 0.88f;
  } else if (idx < 3.5f) {     // 暖色
    fr = r * 1.12f; fg = g * 1.04f; fb = b * 0.90f;
  } else if (idx < 4.5f) {     // 冷色
    fr = r * 0.94f; fg = g * 1.02f; fb = b * 1.12f;
  } else if (idx < 5.5f) {     // 复古胶片：降对比、偏黄
    fr = fg = fb = l;
    fr = r + (fr - r) * 0.30f; fg = g + (fg - g) * 0.30f; fb = b + (fb - b) * 0.30f;
    fr *= 1.08f; fb *= 0.90f;
  } else {                     // 粉调：提亮、偏粉
    fr = r * 1.10f + 0.05f; fg = g * 0.98f; fb = b * 1.02f + 0.05f;
  }
  r = r + (fr - r) * amt;
  g = g + (fg - g) * amt;
  b = b + (fb - b) * amt;
  if (r > 1.f) r = 1.f; if (g > 1.f) g = 1.f; if (b > 1.f) b = 1.f;
  if (r < 0.f) r = 0.f; if (g < 0.f) g = 0.f; if (b < 0.f) b = 0.f;
}

} // namespace beautymath
