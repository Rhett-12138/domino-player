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
