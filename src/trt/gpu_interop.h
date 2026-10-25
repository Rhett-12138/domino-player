#pragma once
#include <memory>

// CUDA Runtime API 的 D3D11 互操作封装（仅 Windows 可选路径）。
// 设计约束：本文件不包含 D3D11/CUDA 头，只接收不透明的 D3D11 texture 指针，
// 因此 domino_core 仍可在没有 D3D11 的平台上编译；实际调用方是 Flutter 的
// GpuFrameOutput。运行期动态加载 cudart64_12.dll，缺失时返回 false 并由
// Player 回退 CPU 路径。
class CudaD3D11Interop {
public:
  CudaD3D11Interop();
  ~CudaD3D11Interop();
  CudaD3D11Interop(const CudaD3D11Interop&) = delete;
  CudaD3D11Interop& operator=(const CudaD3D11Interop&) = delete;

  // 首次调用时加载 cudart；失败返回 false（不抛异常）。
  bool available();
  // 将 TensorRT 的 FP16 NCHW 输出直接写入 3 个 D3D11 R16_FLOAT 纹理。
  // textures 为 ID3D11Texture2D*；w/h 为单平面尺寸；srcDevice 为 FP16 平面起点。
  bool copyToTextures(const void* srcDevice, int slot, int w, int h, void* textures[3]);
  void reset();
  void shutdown();   // 注销资源并销毁 cudart stream（释放 CUDA context 前调用）

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
