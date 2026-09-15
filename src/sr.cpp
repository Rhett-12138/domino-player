#include "sr.h"
#include <cstdio>
#include <cstring>

bool SR::init(const char* paramPath, const char* binPath) {
  net_.opt.use_vulkan_compute = true;
  net_.opt.use_fp16_packed = true;
  net_.opt.use_fp16_storage = true;
  if (net_.load_param(paramPath) != 0 || net_.load_model(binPath) != 0) return false;
  ready_ = true;
  return true;
}

int SR::process(const uint8_t* rgb, int w, int h, int inW, std::vector<uint8_t>& outRgb) {
  if (!ready_) return -1;
  int inH = (h * inW / w) & ~1;
  const float norm[3] = { 1.f / 255.f, 1.f / 255.f, 1.f / 255.f };
  ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb, ncnn::Mat::PIXEL_RGB, w, h, inW, inH);
  in.substract_mean_normalize(0, norm);
  ncnn::Extractor ex = net_.create_extractor();
  ex.input("data", in);
  ncnn::Mat out;
  if (ex.extract("output", out) != 0) return -1;
  for (int q = 0; q < out.c; q++) {
    float* p = out.channel(q);
    for (int i = 0; i < out.w * out.h; i++) p[i] *= 255.f;
  }
  outRgb.resize((size_t)out.w * out.h * 3);
  out.to_pixels(outRgb.data(), ncnn::Mat::PIXEL_RGB);
  return out.w;
}

void SR::clear() {
  if (ready_) {
    net_.clear();
    ready_ = false;
  }
}
