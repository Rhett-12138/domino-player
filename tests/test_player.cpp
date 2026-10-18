#include "test_common.h"
#include "player.h"
#include "frame_output.h"
#include "audio_interface.h"
#include "config.h"
#include <cmath>
#include <thread>
#include <chrono>
#include <memory>

// 哑音频输出：不碰 WASAPI，headless 可跑；bufferedFrames 返回大值让音频喂食线程休眠
class DummyAudioSink : public AudioSink {
public:
  bool init() override { return true; }
  bool start() override { return true; }
  void stop() override {}
  void pause() override {}
  void resume() override {}
  void push(const float*, int) override {}
  void flush() override {}
  size_t bufferedFrames() override { return (size_t)deviceRate * 60; }
  double clock() const override { return 0; }
};

// 会推进的假音频时钟：clock() 以 2x 墙钟速度前进，用来验证视频由音频时钟驱动
class ClockAudioSink : public AudioSink {
public:
  bool init() override { return true; }
  bool start() override { t0_ = std::chrono::steady_clock::now(); base_ = 0; running_ = true; return true; }
  void stop() override { pause(); }
  void pause() override {
    if (!running_) return;
    base_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    running_ = false;
  }
  void resume() override {
    if (running_) return;
    t0_ = std::chrono::steady_clock::now();
    running_ = true;
  }
  void push(const float*, int) override {}
  void flush() override {}
  size_t bufferedFrames() override { return (size_t)deviceRate * 60; }
  double clock() const override {
    double e = base_;
    if (running_) e += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    return e * 2.0;   // 故意快于墙钟
  }
private:
  std::chrono::steady_clock::time_point t0_{};
  double base_ = 0;
  bool running_ = false;
};

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// vsync 对齐的呈现截止时间：30fps/60Hz 下目标时刻应被对齐到 1/60 整数倍
TEST(PresentDeadlineAligned) {
  const double vsync = 1.0 / 60.0;
  const double frameDur = 1.0 / 30.0;
  double base = 100.0;
  // 第 0 帧目标 100.0s，已对齐
  EXPECT_NEAR(calcPresentDeadline(base, 0, frameDur, vsync), 100.0, 1e-9);
  // 第 1 帧目标 100.0333s，向上对齐到 100.0333s（60Hz 下 30fps 每两 tick 一帧）
  double d1 = calcPresentDeadline(base, 1, frameDur, vsync);
  EXPECT_NEAR(d1, 100.0 + 2.0 * vsync, 1e-9);
  EXPECT_NEAR((d1 - base) / vsync, 2.0, 1e-6);   // 刻度数为整数（容忍 FP 1-ulp）
  // 第 2 帧目标 100.0667s
  double d2 = calcPresentDeadline(base, 2, frameDur, vsync);
  EXPECT_NEAR(d2, 100.0 + 4.0 * vsync, 1e-9);
  EXPECT_NEAR((d2 - base) / vsync, 4.0, 1e-6);
}

// 截止时间不小于目标时刻（不提前呈现）
TEST(PresentDeadlineNoEarly) {
  const double vsync = 1.0 / 60.0;
  for (int n = 0; n < 200; n++) {
    double d = calcPresentDeadline(0.0, n, 1.0 / 30.0, vsync);
    double target = n / 30.0;
    EXPECT_TRUE(d >= target - 1e-9);
    EXPECT_TRUE(d - target < vsync + 1e-9);   // 最多晚一个 tick
  }
}

// 播放状态机冒烟（headless，dummy 音频）：open → 呈现帧 → 暂停 → seek → EOF 循环行为
TEST(PlayerSmoke) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_av.mp4").c_str()));
  EXPECT_FALSE(p.paused());
  EXPECT_NEAR(p.duration(), 2.0, 0.3);

  // 播放 ~3s：30fps/2s 视频应走完
  for (int i = 0; i < 300 && !p.eof(); i++) { p.pump(); sleepMs(10); }
  EXPECT_TRUE(p.frameIdx() >= 55);              // 呈现了几乎全部帧
  EXPECT_NEAR(p.fpsDisplay(), 30.0, 10.0);      // 接近源帧率
  EXPECT_TRUE(p.eof());
  // EOF：暂停在结尾（seek 回起点在下次播放时执行）
  EXPECT_TRUE(p.paused());
  EXPECT_TRUE(p.position() >= 1.8);

  // seek 到中部 → 取消暂停 → 播放推进（BACKWARD seek 可能回落到关键帧，只要继续播放即可）
  p.setPaused(false);
  p.requestSeek(1.0);
  long long framesBefore = p.frameIdx();
  for (int i = 0; i < 30; i++) { p.pump(); sleepMs(10); }
  EXPECT_TRUE(p.frameIdx() > framesBefore);        // 继续播放
  EXPECT_TRUE(p.position() >= 0.0 && p.position() <= 1.8);  // 位置合法
  EXPECT_FALSE(p.paused());
  EXPECT_FALSE(p.eof());
  p.close();
  EXPECT_FALSE(p.hasVideo());
}

// 暂停后恢复：呈现基准应重置，不能按 pump 频率快进追暂停期间丢失的墙钟
TEST(PlayerPauseResumeClock) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_long.mp4").c_str()));

  auto playFor = [&](int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < ms / 1000.0) {
      p.pump();
      sleepMs(10);
    }
  };

  playFor(700);
  double before = p.position();
  p.setPaused(true);
  sleepMs(1200);
  p.setPaused(false);
  playFor(800);
  double delta = p.position() - before;
  EXPECT_TRUE(delta > 0.5);   // 恢复后确实在播放
  EXPECT_TRUE(delta < 1.2);   // 不应把暂停的 1.2s 也快进掉（旧实现约 2.0+）
  p.close();
}

// 有音频时钟时应由它驱动视频：假时钟 2x 墙钟，0.6s 墙钟应推进约 1.2s 内容
TEST(PlayerAudioClockDrivesVideo) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<ClockAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_long.mp4").c_str()));

  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.6) {
    p.pump();
    sleepMs(5);
  }
  double pos = p.position();
  EXPECT_TRUE(pos > 0.85);   // 若回退墙钟则约 0.6（fail）
  EXPECT_TRUE(pos < 1.7);
  p.close();
}

// SR 初始化失败：应置失败标志并回退源帧持续播放，不能每帧重试/黑屏
TEST(PlayerSRFailsFallsBackToSource) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  Config cfg = Config::defaults();
  cfg.modelBase = testcfg::dataDir() + "/no_such_model";
  p.setConfig(cfg);
  p.setSRTarget(0, 1920);
  p.setNoSR(false);
  EXPECT_TRUE(p.open(assetPath("test_av.mp4").c_str()));
  // SR 初始化在 VideoWorker 里异步进行
  for (int i = 0; i < 100 && !p.srFailed(); i++) { p.pump(); sleepMs(5); }
  EXPECT_TRUE(p.srFailed());
  EXPECT_FALSE(p.srReady());
  for (int i = 0; i < 160; i++) { p.pump(); sleepMs(5); }
  EXPECT_TRUE(p.frameIdx() > 10);
  {
    std::lock_guard<std::mutex> lk(out.lock());
    EXPECT_EQ(out.width(), 1280);   // 回退源帧，不显示 1080p
  }
  p.close();
}

// 暂停中 seek：worker 仍应执行 seek（不能自旋），恢复后从目标附近继续
TEST(PlayerSeekWhilePaused) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_long.mp4").c_str()));
  for (int i = 0; i < 30; i++) { p.pump(); sleepMs(5); }

  p.setPaused(true);
  p.requestSeek(5.0);
  for (int i = 0; i < 20; i++) { p.pump(); sleepMs(5); }
  p.setPaused(false);
  for (int i = 0; i < 200 && p.position() < 4.5; i++) { p.pump(); sleepMs(5); }
  EXPECT_TRUE(p.position() >= 4.5);
  EXPECT_TRUE(p.position() <= 6.0);
  p.close();
}

// 连续 seek：worker generation 应丢弃旧帧，位置不能回到 seek 前
TEST(PlayerSeekGeneration) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_long.mp4").c_str()));
  for (int i = 0; i < 30; i++) { p.pump(); sleepMs(5); }

  p.requestSeek(10.0);
  for (int i = 0; i < 240 && p.position() < 9.0; i++) { p.pump(); sleepMs(5); }
  EXPECT_TRUE(p.position() >= 9.0);

  p.requestSeek(1.0);
  for (int i = 0; i < 240 && p.position() > 2.0; i++) { p.pump(); sleepMs(5); }
  EXPECT_TRUE(p.position() <= 2.0);
  p.close();
}

// Ticker 停摆后恢复：应丢弃积压帧直接追到当前媒体时间，而不是按 tick 频率快进
TEST(PlayerDropsLateFramesAfterStall) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_long.mp4").c_str()));

  for (int i = 0; i < 30; i++) { p.pump(); sleepMs(10); }   // ~0.3s
  double before = p.position();
  long long framesBefore = p.frameIdx();
  sleepMs(1000);                                            // UI 停摆 1s，不 pump
  // VideoWorker 异步跳过积压帧；给 worker 最多 1s 追上
  for (int i = 0; i < 200 && p.position() - before < 0.8; i++) { p.pump(); sleepMs(5); }
  double delta = p.position() - before;
  EXPECT_TRUE(delta > 0.8);                                 // 追到当前时钟附近
  EXPECT_TRUE(p.frameIdx() - framesBefore <= 8);            // 只呈现少量最新帧
  p.close();
}

// EOF 内部回卷：下一帧处理 seek 后位置回 0，但 eof_ 必须保留；重新播放才清除
TEST(PlayerEofInternalRewind) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_av.mp4").c_str()));
  for (int i = 0; i < 300 && !p.eof(); i++) { p.pump(); sleepMs(10); }
  EXPECT_TRUE(p.eof());
  p.pump();                       // 处理 EOF 内部 seek(0)
  EXPECT_TRUE(p.eof());           // 仍处于 EOF，供前端判断循环
  EXPECT_TRUE(p.position() <= 0.05);
  p.setPaused(false);             // 重新播放才脱离 EOF
  EXPECT_FALSE(p.eof());
  p.close();
}

// 外部 seek 应清除 EOF；EOF 内部回卷则保留 EOF 供前端做循环
TEST(PlayerExternalSeekClearsEof) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());
  EXPECT_TRUE(p.open(assetPath("test_av.mp4").c_str()));
  for (int i = 0; i < 300 && !p.eof(); i++) { p.pump(); sleepMs(10); }
  EXPECT_TRUE(p.eof());
  p.requestSeek(0.5);
  p.pump();
  EXPECT_FALSE(p.eof());
  p.close();
}

// 未打开文件时 pump/close/seek 应安全（幂等）
TEST(PlayerIdempotentBeforeOpen) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.pump();
  p.requestSeek(5.0);
  p.setPaused(true);
  p.close();
  p.close();
}

// 关闭后重新打开另一文件（前端"打开"按钮路径），时长/分辨率应切换
TEST(PlayerReopen) {
  EngineFrameOutput out;
  Player p(out);
  p.setAudioSink(std::make_unique<DummyAudioSink>());
  p.setNoSR(true);
  p.setConfig(Config::defaults());

  EXPECT_TRUE(p.open(assetPath("test_av.mp4").c_str()));
  EXPECT_NEAR(p.duration(), 2.0, 0.3);
  EXPECT_EQ(p.srcW(), 1280);

  p.close();
  EXPECT_FALSE(p.hasVideo());

  EXPECT_TRUE(p.open(assetPath("test_long.mp4").c_str()));
  EXPECT_NEAR(p.duration(), 15.0, 0.5);
  EXPECT_EQ(p.srcW(), 1280);
  EXPECT_FALSE(p.paused());

  // 重开后播放推进
  for (int i = 0; i < 60; i++) { p.pump(); sleepMs(10); }
  EXPECT_TRUE(p.frameIdx() > 0);
  EXPECT_TRUE(p.position() > 0.0);
  p.close();
}
