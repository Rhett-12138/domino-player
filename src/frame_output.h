#pragma once
#include "engine.h"
#include "player.h"
#include <vector>
#include <atomic>
#include <mutex>

// 引擎帧输出（IFrameOutput 实现）：收到 Player 的帧后应用美颜/滤镜（CPU，beauty_math 镜像），
// 存为待消费帧并置"有新帧"标志。前端（Flutter 桥 / 其他）每帧 poll 并上传。
// 说明：CPU 实现用于 PoC/可移植；后续可替换为 GPU 着色器版本（接口不变）。
class EngineFrameOutput : public IFrameOutput {
public:
  EngineFrameOutput() = default;

  void presentFrame(const uint8_t* rgb, int w, int h) override;
  void beginPresent() override {}
  void endPresent() override {}

  // 美颜/滤镜参数（前端/桥设置，上屏前应用）
  void setBeautyParams(const EngineBeautyParams& bp) {
    bp_ = bp;
    beautyActive_ = bp.smooth > 0.001f || bp.white > 0.001f || bp.filterIdx != 0;
  }

  // 是否有新帧（前台每帧检查；取走后复位）
  bool hasNewFrame() const { return newFrame_.load(); }
  void clearNewFrame() { newFrame_.store(false); }

  // 当前帧读取：调用方必须持有 lock() 再读 frame()/width()/height()。
  // 美颜在 presentFrame 中于锁外计算，锁内仅交换缓冲（memcpy 量级），
  // 解码/超分/美颜全程不阻塞上屏线程。
  std::mutex& lock() { return mu_; }
  const uint8_t* frame() const { return outRgb_.data(); }
  int width() const { return w_; }
  int height() const { return h_; }

  // 滤镜缩略图：把当前原始帧降采样到小图，逐滤镜生成一份 RGB24 预览。
  // count 为滤镜数量（含原图）。数据仅保留最近一次调用。
  void makeFilterPreviews(int count);
  int previewCount() const { return (int)previews_.size(); }
  int previewWidth() const { return pw_; }
  int previewHeight() const { return ph_; }
  const uint8_t* previewBuffer(int idx) const {
    if (idx < 0 || idx >= (int)previews_.size()) return nullptr;
    return previews_[(size_t)idx].data();
  }

private:
  static void applyBeautyCpu(const uint8_t* src, int w, int h,
                             const EngineBeautyParams& bp, std::vector<uint8_t>& dst);
  static void downscale(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh);

  EngineBeautyParams bp_;
  bool beautyActive_ = false;
  std::vector<uint8_t> outRgb_;   // 当前上屏帧（锁内交换更新）
  std::vector<uint8_t> tmpRgb_;   // 空闲帧缓冲（复用容量）
  std::atomic<bool> newFrame_{ false };
  int w_ = 0, h_ = 0;
  std::mutex mu_;                 // 保护 outRgb_/w_/h_

  std::vector<std::vector<uint8_t>> previews_;
  int pw_ = 80, ph_ = 45;
};
