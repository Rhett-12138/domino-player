#pragma once
// ============================================================
// Domino Core 引擎契约
// ------------------------------------------------------------
// 这是"引擎"对外的统一接口头：任何前端（当前 Flutter / 未来移动端 C++）
// 通过本头 + domino_core 库驱动播放与任务。
//
// 组成：
//   - IFrameOutput  视频输出抽象（当前 Flutter 像素纹理实现）
//   - Player        播放引擎（解码/超分/美颜/滤镜/音频/呈现调度）—— 见 player.h
//   - TaskCenter    任务中心（离线超分/断点续编/转存）          —— 见 task.h
//   - encodeOffline 离线编码                                    —— 见 encode.h
//   - Config        路径配置（可注入）                          —— 见 config.h
//   - beautymath    美颜/滤镜纯数学（可单测，与着色器镜像）      —— 见 beauty_math.h
//
// 使用示意（任意前端）：
//   Config cfg = Config::defaults();
//   MyFrameOutput out;                 // 前端实现 IFrameOutput
//   Player player(out);                // 播放引擎
//   TaskCenter tasks; tasks.init(cfg.dataRoot);
//   player.setConfig(cfg);
//   player.open("video.mp4");
//   while (running) {
//     player.pump();                   // 解码/超分/调度，到点回调 out.presentFrame
//     out.beginPresent();              // 绘制当前帧
//     ... 前端 UI ...
//     out.endPresent();
//   }
// ============================================================

#include "config.h"
#include "decode.h"
#include "sr.h"
#include "encode.h"
#include "task.h"
#include "beauty_math.h"

// 引擎美颜/滤镜参数（前端经 Player::setBeauty/setFilter 设置，帧输出上屏前应用）
struct EngineBeautyParams {
  float smooth = 0.f;      // 磨皮 0..1
  float white = 0.f;       // 美白 0..1
  int filterIdx = 0;       // 滤镜索引（0=原图…）
  float filterAmt = 1.f;   // 滤镜强度 0..1
};

// 视频输出抽象：播放引擎每呈现一帧回调 presentFrame；前台用它上屏。
// 当前实现：EngineFrameOutput（CPU 美颜 + 帧缓冲，见 frame_output.h），Flutter 经纹理上传上屏。
class IFrameOutput {
public:
  virtual ~IFrameOutput() = default;
  // 呈现一帧（RGB24, w×h）。播放引擎在"到点"时调用。
  virtual void presentFrame(const uint8_t* rgb, int w, int h) = 0;
  // 前台每帧绘制：重画当前帧（双缓冲防闪烁）
  virtual void beginPresent() = 0;
  // 前台每帧收尾：交换/上屏（引擎据此测 vsync）
  virtual void endPresent() = 0;
};
