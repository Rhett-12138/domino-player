#pragma once
#include <cstddef>

// 音频输出抽象（可移植，无平台依赖）。Player 只依赖本接口；
// Windows 用 WasapiAudioOut（audio.cpp），Linux/移动端可另写实现注入。
class AudioSink {
public:
  virtual ~AudioSink() = default;

  // 初始化并查询设备 mix 格式，暴露 deviceRate/deviceChannels；成功返回 true
  virtual bool init() = 0;
  // 启动引擎（创建播放线程）
  virtual bool start() = 0;
  // 停止引擎并回收线程
  virtual void stop() = 0;
  // 立即暂停（静音）
  virtual void pause() = 0;
  virtual void resume() = 0;
  // 推入交错 float 数据（帧数=每声道样本数，设备格式）
  virtual void push(const float* interleaved, int frames) = 0;
  // 清空环形缓冲
  virtual void flush() = 0;
  // 环形缓冲中待播帧数
  virtual size_t bufferedFrames() = 0;
  // 已呈现秒数
  virtual double clock() const = 0;

  int deviceRate = 48000;
  int deviceChannels = 2;
};
