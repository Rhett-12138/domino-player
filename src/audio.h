#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

#include "audio_interface.h"

// WASAPI 共享模式音频输出（Windows 实现 AudioSink）
class WasapiAudioOut : public AudioSink {
public:
  bool init() override;
  bool start() override;
  void stop() override;
  void pause() override;
  void resume() override;
  void push(const float* interleaved, int frames) override;
  void flush() override;
  size_t bufferedFrames() override;
  double clock() const override;

private:
  static DWORD WINAPI renderThread(LPVOID p);
  void renderLoop();

  IMMDevice* dev_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  IAudioClock* clockI_ = nullptr;
  HANDLE evt_ = nullptr;
  HANDLE thread_ = nullptr;
  UINT32 bufferFrames_ = 0;
  std::atomic<bool> running_{ false };

  std::mutex mu_;
  std::vector<float> ring_;
  size_t ringPos_ = 0;      // 读指针
  size_t ringFill_ = 0;     // 已写帧数
  std::atomic<uint64_t> writtenFrames_{ 0 }; // 累计写入设备帧数
};

// 默认音频输出工厂（Windows = WASAPI；Linux/移动端平台实现替换）
inline std::unique_ptr<AudioSink> createDefaultAudioSink() {
  return std::make_unique<WasapiAudioOut>();
}
