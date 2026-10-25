#pragma once
#include "engine.h"

#include <flutter_texture_registrar.h>

#include <cstdint>
#include <memory>

// D3D11/HLSL 美颜帧输出（Windows 前端专用）。
// 通过 DXGI shared handle 交给 Flutter GPU surface；初始化失败时调用方回退 CPU。
class GpuFrameOutput : public IFrameOutput, public IGpuFrameSink {
public:
  GpuFrameOutput();
  ~GpuFrameOutput() override;

  bool ok() const;

  void presentFrame(const uint8_t* rgb, int w, int h) override;
  void beginPresent() override {}
  void endPresent() override {}

  bool acquireGpuSrFrame(int w, int h, int& slot, void* textures[3]) override;
  void releaseGpuSrFrame(int slot) override;
  bool presentGpuSrFrame(int slot, int w, int h) override;
  void lockGpuCopy() override;
  void unlockGpuCopy() override;
  bool gpuFatal() const override;

  void setBeautyParams(const EngineBeautyParams& bp);
  bool hasNewFrame() const;
  void clearNewFrame();
  int width() const;
  int height() const;

  // 滤镜缩略图复用 CPU（80×45 小图），避免把大帧回读
  void makeFilterPreviews(int count);
  int previewCount() const;
  int previewWidth() const;
  int previewHeight() const;
  const uint8_t* previewBuffer(int idx) const;

  // Flutter 光栅线程取当前共享纹理。
  // 契约：descriptor 由本对象持有，handle 在 release_callback 触发前有效；
  // 每次 acquire 对应一次 release，期间不要覆盖该 descriptor。
  const FlutterDesktopGpuSurfaceDescriptor* acquireDescriptor();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
