#pragma once
#include <ncnn/net.h>
#include <vector>

// NCNN Vulkan 超分封装
class SR {
public:
  bool init(const char* paramPath, const char* binPath);
  bool ready() const { return ready_; }
  int process(const uint8_t* rgb, int w, int h, int inW, std::vector<uint8_t>& outRgb);
  void clear();

private:
  ncnn::Net net_;
  bool ready_ = false;
};
