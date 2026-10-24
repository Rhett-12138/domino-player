#pragma once
#include <ncnn/net.h>
#include <vector>

// NCNN Vulkan 超分封装
class SR {
public:
  bool init(const char* paramPath, const char* binPath);
  bool ready() const { return ready_; }
  // 离线流水线拆分：infer 只做输入转换 + GPU 推理，convert 可在独立线程执行。
  int infer(const uint8_t* rgb, int w, int h, int inW, ncnn::Mat& out);
  static bool convert(const ncnn::Mat& out, std::vector<uint8_t>& outRgb);
  int process(const uint8_t* rgb, int w, int h, int inW, std::vector<uint8_t>& outRgb);
  void clear();

private:
  ncnn::Net net_;
  bool ready_ = false;
};
