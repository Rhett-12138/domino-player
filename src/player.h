#pragma once
#include "engine.h"
#include "decode.h"
#include "sr.h"
#include "audio_interface.h"
#include "config.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <mutex>

// 旧版 vsync 量化截止时间：当前播放按媒体 PTS/音频时钟呈现，保留给需要 vsync 量化的前端与纯函数测试
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
  // 必须在 open 前设置；播放中修改会与 VideoWorker 的模型路径读取竞争
  void setConfig(const Config& c) { cfg_ = c; }
  // setNoSR(true) 是“用户关闭”，会清空档位与自动状态；
  // setSRTarget(t>0) 会启用 SR 并重置自动降档（手动重选 = 重新尝试）。
  void setNoSR(bool b);
  void setSRTarget(int presetIdx, int targetW);
  void setInW(int w) { inW_.store(w); }   // 手动 SR 输入宽（--in）

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
  bool noSR() const { return noSR_.load(); }
  int srTargetW() const { return srTargetW_.load(); }
  int srCeilingW() const { return srCeilingW_.load(); }
  bool srAutoDegraded() const { return srAutoDegraded_.load(); }
  bool srReady() const { return srInited_.load(); }   // 异步：open 后可能短暂 false
  bool srFailed() const { return srFailed_.load(); }

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
  // 从 VideoWorker 队列取一帧（UI 线程调用，不跑解码/SR）
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
  struct VideoFrame {
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    double pts = 0;
  };

  static void audioFeedStatic(Player* self);
  void audioFeed();
  static void videoLoopStatic(Player* self);
  void videoLoop();
  void startVideo();
  void stopVideo();
  void clearVideoQueue();
  void seekInternal(double t);
  void ensureSR();
  void maybeAutoDegrade(double now);
  void resetClockAnchors();
  double currentMediaClock();
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
  std::atomic<bool> noSR_{ true };
  std::atomic<int> srPresetIdx_{ 1 };
  std::atomic<int> srCeilingW_{ 0 };   // 用户选择的档位（自动降档不改）
  std::atomic<int> srTargetW_{ 1920 }; // 当前实际档位
  std::atomic<bool> srAutoDegraded_{ false };
  std::atomic<int> inW_{ 0 };    // 手动 SR 输入宽（--in）
  std::atomic<bool> srInited_{ false };
  std::atomic<bool> srFailed_{ false };
  double lagHighSince_ = 0;
  double autoCooldownUntil_ = 0;
  double lastAutoCheck_ = 0;

  int srOutW_ = 0, srOutH_ = 0;
  double fpsDisplay_ = 0;
  long long frameIdx_ = 0;
  bool drewVideo_ = false;
  bool eof_ = false;
  int frameCount_ = 0;
  double lastFps_ = 0;
  std::chrono::steady_clock::time_point fpsT0_;

  // 视频工作线程：解码 + SR，UI 只消费队列
  std::thread videoThread_;
  std::atomic<bool> videoStop_{ false };
  std::atomic<bool> videoEof_{ false };
  std::atomic<uint64_t> seekGen_{ 0 };
  std::atomic<bool> seekReq_{ false };
  std::atomic<double> seekTargetVideo_{ 0 };
  std::atomic<double> dropUntilPts_{ 0 };
  std::mutex videoMu_;
  std::condition_variable videoCv_;
  std::deque<VideoFrame> videoFrames_;
  std::vector<std::vector<uint8_t>> freeBufs_;
  std::vector<uint8_t> srcBuf_;   // worker 解码缓冲
  VideoFrame pendingFrame_;       // UI 当前待呈现帧

  // 音频喂送
  std::atomic<bool> audioStarted_{ false };
  std::atomic<bool> resetAudio_{ false };
  std::atomic<bool> stopFeed_{ false };
  std::thread audioThread_;
  std::mutex audMu_;   // 喂食线程解码与 UI seek 互斥

  // seek
  std::atomic<bool> seekPending_{ false };
  double seekTarget_ = 0;
  bool eofSeek_ = false;   // 该 seek 是 EOF 内部回卷，不重置 eof_

  // 呈现调度：媒体 PTS 为准，音频时钟优先，不可用时回退墙钟
  std::chrono::steady_clock::time_point tStart_;
  double fallbackWallBase_ = 0;    // 0 = 尚未锚定
  double fallbackMediaBase_ = 0;
  double audioClockBase_ = 0;
  double audioMediaBase_ = 0;
  double lastAudioClock_ = 0;
  double lastAudioClockWall_ = 0;
  double lastPumpWall_ = 0;
  bool audioClockValid_ = false;
  long long presentedFrames_ = 0;
  bool havePending_ = false;
  bool pendingEOF_ = false;
  double pendingPts_ = 0;
  double vsync_ = 1.0 / 60.0;
  double lastPresTime_ = 0;
  std::vector<double> presIntervals_;

  // 直播 / 当前打开路径
  std::string liveTaskId_;
  std::string livePlayPath_;
  std::string currentPath_;

  // 美颜/滤镜
  EngineBeautyParams bp_;
};
