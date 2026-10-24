#include "sr.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

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
    net_.opt.use_packing_layout = true;
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
  if (out.c < 3) return -1;
  const int outW = out.w;
  const int outH = out.h;
  outRgb.resize((size_t)outW * outH * 3);
  auto convertRows = [&](int y0, int y1) {
    const float* r = out.channel(0);
    const float* g = out.channel(1);
    const float* b = out.channel(2);
    for (int y = y0; y < y1; y++) {
      const float* rp = r + (size_t)y * outW;
      const float* gp = g + (size_t)y * outW;
      const float* bp = b + (size_t)y * outW;
      uint8_t* dp = outRgb.data() + (size_t)y * outW * 3;
      for (int x = 0; x < outW; x++) {
        float vr = rp[x] * 255.f;
        float vg = gp[x] * 255.f;
        float vb = bp[x] * 255.f;
        vr = (std::min)(255.f, (std::max)(0.f, vr));
        vg = (std::min)(255.f, (std::max)(0.f, vg));
        vb = (std::min)(255.f, (std::max)(0.f, vb));
        dp[x * 3 + 0] = (uint8_t)vr;
        dp[x * 3 + 1] = (uint8_t)vg;
        dp[x * 3 + 2] = (uint8_t)vb;
      }
    }
  };
  int threads = (int)std::thread::hardware_concurrency();
  threads = (std::max)(1, (std::min)(8, threads));
  if ((size_t)outW * outH < 200000) threads = 1;
  if (threads <= 1) {
    convertRows(0, outH);
  } else {
    std::vector<std::thread> workers;
    workers.reserve((size_t)threads);
    for (int i = 0; i < threads; i++) {
      int y0 = outH * i / threads;
      int y1 = outH * (i + 1) / threads;
      workers.emplace_back(convertRows, y0, y1);
    }
    for (auto& t : workers) t.join();
  }
  return out.w;
}

void SR::clear() {
  if (ready_) {
    net_.clear();
    ready_ = false;
  }
}
