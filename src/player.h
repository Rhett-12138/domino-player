#pragma once
#include "engine.h"
#include "decode.h"
#include "sr.h"
#include "audio_interface.h"
#include "config.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

// 平滑呈现截止时间：把目标墙钟时刻对齐到整数个 vsync，避免 30fps/60Hz 量化抖动
inline double calcPresentDeadline(double baseWall, long long presentedFrames, double frameDur, double vsync) {
  double target = baseWall + (double)presentedFrames * frameDur;
  return ceil(target / vsync) * vsync;
}

// 播放核心：解码 + 超分 + 音频 + 呈现调度（平滑 vsync 对齐）+ A/V 同步。
// 独立于 UI 与具体显示后端（通过 IFrameOutput 输出视频帧）；直播任务协调由上层负责。
class Player {
public:
  Player(IFrameOutput& out);
  ~Player();

  // ---- 配置 ----
  void setConfig(const Config& c) { cfg_ = c; }
  void setNoSR(bool b) { noSR_ = b; }
  void setSRTarget(int presetIdx, int targetW) { srPresetIdx_ = presetIdx; srTargetW_ = targetW; }
  void setInW(int w) { inW_ = w; }   // 手动 SR 输入宽（--in）

  // ---- 打开/关闭 ----
  bool open(const std::string& path);
  void close();
  bool hasVideo() const { return vid_.vw > 0; }
  void shutdown();   // 停止音频/关闭解码/释放超分与 GPU 实例

  // ---- 状态查询 ----
  double position() const { return playhead_; }
  double duration() const { return duration_; }
  double fps() const { return fps_; }
  bool paused() const { return paused_.load(); }
  double volume() const { return volume_.load(); }
  bool hasAudio() const { return hasAudio_; }
  int srcW() const { return vid_.vw; }
  int srcH() const { return vid_.vh; }
  int srOutW() const { return srOutW_; }
  int srOutH() const { return srOutH_; }
  double fpsDisplay() const { return fpsDisplay_; }
  long long frameIdx() const { return frameIdx_; }
  bool drewVideo() const { return drewVideo_; }
  bool eof() const { return eof_; }
  bool noSR() const { return noSR_; }
  int srTargetW() const { return srTargetW_; }

  // ---- 控制 ----
  void setPaused(bool p);
  void requestSeek(double t);
  void setVolume(double v) { volume_.store(v); }

  // ---- 美颜/滤镜（引擎状态，帧输出在上屏前应用） ----
  void setBeauty(float smooth, float white) { bp_.smooth = smooth; bp_.white = white; }
  void setFilter(int idx, float amt) { bp_.filterIdx = idx; bp_.filterAmt = amt; }
  const EngineBeautyParams& beauty() const { return bp_; }

  // ---- 每帧推进（主循环调用） ----
  // 便捷入口：处理 seek + 预取/呈现 + EOF；适合外部前端（Flutter 桥）每帧调用
  void pump();
  // 处理挂起的 seek（若有）；返回是否发生了 seek（需要重置调度）
  bool processSeek();
  // 预取解码+SR 下一帧（如需要）
  void prefetch();
  // 是否有已处理好的帧且已到呈现截止时间（惰性初始化 baseWall）
  bool readyToPresent();
  // 呈现当前帧：上传纹理 + 推进播放时钟；返回本帧是否应启动音频
  bool presentFrame();
  // EOF 处理：返回后续动作
  enum class EofAction { None, Pause, LivePresent, LiveDone };
  EofAction handleEof();
  // 直播：更新 seek 时长/进度（跟随任务编码）
  void updateLiveProgress(double frames, double fps);

  // ---- 呈现（经 IFrameOutput 输出），中间可插入 UI ----
  void beginRender();       // 绑定后缓冲并绘制视频四边形
  void endRender();         // Present（含 vsync 测量）

  // ---- 音频 ----
  void startAudio();
  void stopAudio();
  // 注入自定义音频输出（默认 WASAPI；Linux/移动端在 open 前注入）
  void setAudioSink(std::unique_ptr<AudioSink> s) { if (s) audio_ = std::move(s); }

  // ---- 直播状态 ----
  void setLiveTask(const std::string& taskId, const std::string& path) { liveTaskId_ = taskId; livePlayPath_ = path; }
  void clearLiveTask() { liveTaskId_.clear(); }
  bool liveActive() const { return !liveTaskId_.empty(); }
  const std::string& livePlayPath() const { return livePlayPath_; }
  void seekTo(double t) { if (t >= 0 && t <= duration_) seekInternal(t); }

  std::string titleText() const;
  int vsyncPeriod() const;   // 供 UI 展示/调试
  const std::string& currentPath() const { return currentPath_; }

private:
  static void audioFeedStatic(Player* self);
  void audioFeed();
  void seekInternal(double t);
  void ensureSR();
  double nowWall() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart_).count(); }

  IFrameOutput& out_;        // 视频输出（Flutter 像素纹理）
  Config cfg_;
  MediaStream vid_, aud_;
  SR sr_;
  std::unique_ptr<AudioSink> audio_;   // 音频输出（默认 WASAPI；可注入）

  std::atomic<bool> paused_{ false };
  std::atomic<double> volume_{ 1.0 };
  double playhead_ = 0;
  double duration_ = 0;
  double fps_ = 30;
  bool hasAudio_ = false;
  bool noSR_ = true;
  int srPresetIdx_ = 1;
  int srTargetW_ = 1920;
  int inW_ = 0;                 // 手动 SR 输入宽（--in）
  bool srInited_ = false;

  std::vector<uint8_t> rgbBuf_; // 解码帧缓冲（源尺寸）
  std::vector<uint8_t> srgb_;   // 处理后帧（可能为超分输出）
  int srOutW_ = 0, srOutH_ = 0;
  double fpsDisplay_ = 0;
  long long frameIdx_ = 0;
  bool drewVideo_ = false;
  bool eof_ = false;
  int frameCount_ = 0;
  double lastFps_ = 0;
  std::chrono::steady_clock::time_point fpsT0_;

  // 音频喂送
  std::atomic<bool> audioStarted_{ false };
  std::atomic<bool> resetAudio_{ false };
  std::atomic<bool> stopFeed_{ false };
  std::thread audioThread_;

  // seek
  std::atomic<bool> seekPending_{ false };
  double seekTarget_ = 0;
  bool eofSeek_ = false;   // 该 seek 是 EOF 内部回卷，不重置 eof_

  // 平滑呈现调度
  std::chrono::steady_clock::time_point tStart_;
  double baseWall_ = 0;
  long long presentedFrames_ = 0;
  bool havePending_ = false;
  bool pendingEOF_ = false;
  double pendingPts_ = 0;
  double vsync_ = 1.0 / 60.0;
  double lastPresTime_ = 0;
  std::vector<double> presIntervals_;
  double frameDur_ = 1.0 / 30.0;

  // 直播 / 当前打开路径
  std::string liveTaskId_;
  std::string livePlayPath_;
  std::string currentPath_;

  // 美颜/滤镜
  EngineBeautyParams bp_;
};
