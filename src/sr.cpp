#include "sr.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool SR::init(const char* paramPath, const char* binPath) {
  net_.opt.use_vulkan_compute = true;
  net_.opt.use_fp16_packed = true;
  net_.opt.use_fp16_storage = true;
  // 对支持的 Vulkan 设备启用 FP16 算术、cooperative matrix 和共享内存优化；
  // 不支持的驱动会由 NCNN 自动回退到普通路径。可用环境变量关闭快路径排查驱动问题。
  const char* noFast = std::getenv("DOMINO_NO_VULKAN_FASTPATH");
  if (!(noFast && noFast[0] && std::strcmp(noFast, "0") != 0)) {
    net_.opt.use_fp16_arithmetic = true;
    net_.opt.use_cooperative_matrix = true;
    net_.opt.use_shader_local_memory = true;
  }
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
  // Real-ESRGAN 用 data，SRMD 用 input；从模型元数据取实际 blob 名，避免猜测。
  const char* inName = net_.input_names().empty() ? "data" : net_.input_names().front();
  const char* outName = net_.output_names().empty() ? "output" : net_.output_names().front();
  if (ex.input(inName, in) != 0) return -1;
  ncnn::Mat out;
  if (ex.extract(outName, out) != 0) return -1;
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
