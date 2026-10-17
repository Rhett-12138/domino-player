#include "test_common.h"
#include "encode.h"
#include "config.h"
#include "log.h"
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdio>

// 用 avformat 读取输出文件，返回时长；失败返回 -1
static double probeDuration(const char* path) {
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return -1;
  avformat_find_stream_info(fmt, nullptr);
  double d = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
  int vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (d <= 0 && vsi >= 0) d = (double)fmt->streams[vsi]->duration * av_q2d(fmt->streams[vsi]->time_base);
  avformat_close_input(&fmt);
  return d;
}

static std::string tempOut(const char* name) {
  return testcfg::dataDir() + "/" + name;
}

// 全片编码（无超分：目标宽=源宽），校验产物可打开、时长正确
TEST(EncodeFullNoSR) {
  std::string out = tempOut("encode_full.mp4");
  std::error_code ec;
  std::filesystem::remove(out, ec);
  EncodeOptions eo;
  eo.verbose = false;
  eo.modelBase = Config::defaults().modelBase;
  EXPECT_TRUE(encodeOffline(assetPath("test_av.mp4").c_str(), out.c_str(), 1280, eo));
  EXPECT_NEAR(probeDuration(out.c_str()), 2.0, 0.3);
}
TEST(RemuxFaststart) {
  std::string src = tempOut("encode_full.mp4");
  std::string out = tempOut("remux.mp4");
  std::error_code ec;
  std::filesystem::remove(out, ec);
  float prog = -1;
  EXPECT_TRUE(remuxFaststart(src.c_str(), out.c_str(),
    [](float p, void* ud) { *(float*)ud = p; }, &prog));
  EXPECT_NEAR(probeDuration(out.c_str()), probeDuration(src.c_str()), 0.1);
  EXPECT_TRUE(prog > 0.0f && prog <= 1.0f);  // 进度回调被调用
}

// 检查点滞后：分段起点比主文件末尾早约 0.5s 时，仍必须追加（不能把整段当“已合并”丢掉）
TEST(MergeCheckpointLagKeepsTail) {
  std::string main = tempOut("lag_main.mp4");
  std::string part = tempOut("lag_part.mp4");
  std::string merged = tempOut("lag_merged.mp4");
  std::error_code ec;
  for (auto& f : { main, part, merged }) std::filesystem::remove(f, ec);

  EncodeProgress prog;
  EncodeOptions eo;
  eo.verbose = false;
  eo.prog = &prog;
  eo.modelBase = Config::defaults().modelBase;
  std::thread t([&] { encodeOffline(assetPath("test_long.mp4").c_str(), main.c_str(), 1280, eo); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  prog.cancel.store(true);
  t.join();

  double mainDur = probeDuration(main.c_str());
  EXPECT_TRUE(mainDur > 0.05 && mainDur < 14.0);

  EncodeOptions eo2;
  eo2.verbose = false;
  eo2.modelBase = Config::defaults().modelBase;
  eo2.startTime = mainDur > 0.5 ? mainDur - 0.5 : 0.0;   // 模拟检查点滞后
  eo2.startFrame = 0;
  EXPECT_TRUE(encodeOffline(assetPath("test_long.mp4").c_str(), part.c_str(), 1280, eo2));

  EXPECT_TRUE(mergeToMp4(main.c_str(), part.c_str(), merged.c_str()));
  double d = probeDuration(merged.c_str());
  EXPECT_TRUE(d > mainDur + 1.0);              // 尾巴没有被整段丢弃
  EXPECT_TRUE(d < 17.0);                       // 也不应重复成 30s
}

// 断点续编 + 合并：先编码 ~0.6s 后取消（部分产物），再续编剩余，合并后应为完整时长
TEST(ResumeAndMerge) {
  std::string main = tempOut("resume_main.mp4");
  std::string part = tempOut("resume_part.mp4");
  std::string merged = tempOut("resume_merged.mp4");
  std::error_code ec;
  for (auto& f : { main, part, merged }) std::filesystem::remove(f, ec);

  EncodeProgress prog;
  EncodeOptions eo;
  eo.verbose = false;
  eo.prog = &prog;
  eo.modelBase = Config::defaults().modelBase;

  // 1) 子线程编码 test_long(15s)，约 0.6s 后取消 → 部分 main
  std::atomic<bool> started{ false };
  std::thread t([&] {
    started.store(true);
    encodeOffline(assetPath("test_long.mp4").c_str(), main.c_str(), 1280, eo);
  });
  while (!started.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  prog.cancel.store(true);
  t.join();
  EXPECT_TRUE(std::filesystem::exists(main));
  double mainDur = probeDuration(main.c_str());
  EXPECT_TRUE(mainDur > 0.1 && mainDur < 15.0);  // 部分文件时长在 (0.1, 15) 之间

  // 2) 续编：从检查点继续
  double resumeTime = prog.curTime.load();
  long long startFrame = prog.frame.load();
  EXPECT_TRUE(resumeTime > 0.05);
  EncodeOptions eo2;
  eo2.verbose = false;
  eo2.modelBase = Config::defaults().modelBase;
  eo2.startTime = resumeTime;
  eo2.startFrame = startFrame;
  EXPECT_TRUE(encodeOffline(assetPath("test_long.mp4").c_str(), part.c_str(), 1280, eo2));

  // 3) 合并
  EXPECT_TRUE(mergeToMp4(main.c_str(), part.c_str(), merged.c_str()));
  double mergedDur = probeDuration(merged.c_str());
  EXPECT_NEAR(mergedDur, 15.0, 0.5);  // 合并后接近完整时长
}
