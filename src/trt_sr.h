#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IGpuFrameSink;

// 可选 TensorRT-RTX 后端：仅用于真实 GPU，初始化失败时调用方回退 NCNN。
// 内部使用 CUDA driver API（nvcuda.dll），不要求安装 CUDA Toolkit。
class TensorRtSR {
public:
  TensorRtSR();
  ~TensorRtSR();
  TensorRtSR(const TensorRtSR&) = delete;
  TensorRtSR& operator=(const TensorRtSR&) = delete;

  // onnxPath: FP16 ONNX；cacheDir: engine 缓存目录；
  // cacheKey: 模型标识；maxInW/H: 动态 shape profile 上限。
  bool init(const char* onnxPath, const char* cacheDir, const char* cacheKey,
            int scale, int maxInW, int maxInH);
  bool ready() const;
  // 与 SR::process 相同：RGB24 输入，按 inW 等比缩放后推理，返回输出宽度；
  // 失败返回 -1，调用方必须回退源帧。
  int process(const uint8_t* rgb, int w, int h, int inW,
              std::vector<uint8_t>& outRgb);
  bool outputSize(int srcW, int srcH, int inW, int& outW, int& outH) const;
  int processGpu(const uint8_t* rgb, int w, int h, int inW, int slot,
                 void* textures[3], IGpuFrameSink* lockSink = nullptr);
  // D3D 纹理尺寸/实例即将变化时，先注销 cudart 资源，避免 D3D 纹理先销毁。
  void resetGpuInterop();
  void clear();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
