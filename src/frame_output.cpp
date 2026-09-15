#include "frame_output.h"
#include "beauty_math.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>

// CPU 基础美颜/滤镜（镜像 HLSL 逻辑，beauty_math 为准）。
// 磨皮：可分离高斯模糊（仅肤色区混合）；美白：肤色区向白靠拢；滤镜：预设。
static void gaussian1D(const uint8_t* src, int w, int h, int radius, bool horizontal,
                       std::vector<uint8_t>& dst) {
  const float sigma = 0.6f + (float)radius * 0.8f;
  std::vector<float> kernel;
  float sum = 0.f;
  for (int i = -radius; i <= radius; i++) {
    float v = std::exp(-(float)(i * i) / (2.f * sigma * sigma));
    kernel.push_back(v);
    sum += v;
  }
  for (auto& k : kernel) k /= sum;
  int n = w * h * 3;
  dst.resize((size_t)n);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      for (int c = 0; c < 3; c++) {
        float acc = 0.f;
        for (int i = -radius; i <= radius; i++) {
          int sx = horizontal ? x + i : x;
          int sy = horizontal ? y : y + i;
          if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
          if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
          acc += kernel[(size_t)(i + radius)] * src[((size_t)sy * w + sx) * 3 + c];
        }
        dst[((size_t)y * w + x) * 3 + c] = (uint8_t)acc;
      }
    }
  }
}

void EngineFrameOutput::applyBeautyCpu(const uint8_t* src, int w, int h,
                                       const EngineBeautyParams& bp, std::vector<uint8_t>& dst) {
  const int n = w * h;
  dst.resize((size_t)n * 3);
  const bool doSmooth = bp.smooth > 0.001f;
  const bool doWhite = bp.white > 0.001f;
  const bool doFilter = bp.filterIdx != 0 && bp.filterAmt > 0.001f;
  if (!doSmooth && !doWhite && !doFilter) {          // 无任何效果：直接拷贝
    std::memcpy(dst.data(), src, (size_t)n * 3);
    return;
  }
  std::vector<uint8_t> blur;
  int radius = doSmooth ? (int)std::lround(bp.smooth * 4.0f) : 0;
  if (doSmooth && radius > 0) {
    gaussian1D(src, w, h, radius, true, blur);
    gaussian1D(blur.data(), w, h, radius, false, blur);
  }
  const uint8_t* b = (doSmooth && !blur.empty()) ? blur.data() : src;
  const bool needSkin = doSmooth || doWhite;          // 仅滤镜时跳过肤色判定（主要开销）
  const float fIdx = (float)bp.filterIdx, fAmt = bp.filterAmt;

  auto work = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
      for (int x = 0; x < w; x++) {
        const int i = y * w + x;
        float r = src[i * 3 + 0] / 255.f, g = src[i * 3 + 1] / 255.f, b2 = src[i * 3 + 2] / 255.f;
        if (needSkin) {
          float skin = beautymath::skinProb(r, g, b2);
          if (doSmooth) {
            float br = b[i * 3 + 0] / 255.f, bg = b[i * 3 + 1] / 255.f, bb = b[i * 3 + 2] / 255.f;
            r = beautymath::blurBlend(r, br, skin, bp.smooth);
            g = beautymath::blurBlend(g, bg, skin, bp.smooth);
            b2 = beautymath::blurBlend(b2, bb, skin, bp.smooth);
          }
          if (doWhite) {
            r = beautymath::whiteBlend(r, skin, bp.white);
            g = beautymath::whiteBlend(g, skin, bp.white);
            b2 = beautymath::whiteBlend(b2, skin, bp.white);
          }
        }
        if (doFilter) beautymath::applyFilter(r, g, b2, fIdx, fAmt);
        dst[i * 3 + 0] = (uint8_t)(r * 255.f + 0.5f);
        dst[i * 3 + 1] = (uint8_t)(g * 255.f + 0.5f);
        dst[i * 3 + 2] = (uint8_t)(b2 * 255.f + 0.5f);
      }
    }
  };

  // 大图按行分块并行（各线程写不同行，只读共享 src/blur）
  // 注：用 (std::min) 括号形式避免 Windows min 宏展开
  unsigned hw = std::thread::hardware_concurrency();
  int threads = (n >= 400000 && hw > 1) ? (int)(std::min<unsigned>)(hw, 8u) : 1;
  if (threads <= 1) {
    work(0, h);
  } else {
    std::vector<std::thread> ts;
    int band = (h + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
      int y0 = t * band, y1 = (std::min)(h, y0 + band);
      if (y0 >= y1) break;
      ts.emplace_back(work, y0, y1);
    }
    for (auto& th : ts) th.join();
  }
}

void EngineFrameOutput::presentFrame(const uint8_t* rgb, int w, int h) {
  // 美颜计算（重活）在锁外进行；无美颜时走 memcpy 快路径。
  if (beautyActive_) {
    applyBeautyCpu(rgb, w, h, bp_, tmpRgb_);
  } else {
    tmpRgb_.assign(rgb, rgb + (size_t)w * h * 3);
  }
  rawRgb_.assign(rgb, rgb + (size_t)w * h * 3);
  std::lock_guard<std::mutex> lk(mu_);
  outRgb_.swap(tmpRgb_);   // 交换缓冲：旧帧进 tmpRgb_ 复用，零分配
  w_ = w;
  h_ = h;
  newFrame_.store(true);
}

void EngineFrameOutput::downscale(const uint8_t* src, int sw, int sh,
                                  uint8_t* dst, int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    int sy = (int)((uint64_t)y * sh / dh);
    for (int x = 0; x < dw; x++) {
      int sx = (int)((uint64_t)x * sw / dw);
      size_t si = ((size_t)sy * sw + sx) * 3;
      size_t di = ((size_t)y * dw + x) * 3;
      dst[di + 0] = src[si + 0];
      dst[di + 1] = src[si + 1];
      dst[di + 2] = src[si + 2];
    }
  }
}

void EngineFrameOutput::makeFilterPreviews(int count) {
  if (rawRgb_.empty() || w_ <= 0 || h_ <= 0 || count < 1) return;
  std::vector<uint8_t> small((size_t)pw_ * ph_ * 3);
  downscale(rawRgb_.data(), w_, h_, small.data(), pw_, ph_);
  // 基础图：当前美颜参数、无滤镜
  EngineBeautyParams base = bp_;
  base.filterIdx = 0;
  std::vector<uint8_t> baseImg;
  applyBeautyCpu(small.data(), pw_, ph_, base, baseImg);
  previews_.assign((size_t)count, {});
  for (int i = 0; i < count; i++) {
    EngineBeautyParams p;
    p.filterIdx = i;
    p.filterAmt = 1.f;
    applyBeautyCpu(baseImg.data(), pw_, ph_, p, previews_[(size_t)i]);
  }
}
