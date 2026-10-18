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

// 离线质量档：FullSource 不应把 1080p 源先缩到 640/960
TEST(SRModelRegistry) {
  EXPECT_STREQ(srModelParamName(SRModel::Realistic), "RealESRGAN_General_x4_v3.param");
  EXPECT_STREQ(srModelParamName(SRModel::Anime), "realesr-animevideov3-x4.param");
  EXPECT_STREQ(srModelParamName(SRModel::RealisticHQ), "realesrgan-x4plus.param");
  EXPECT_STREQ(srModelParamName(SRModel::AnimeHQ), "realesrgan-x4plus-anime.param");
  EXPECT_STREQ(srModelParamName(SRModel::AnimeX2), "realesr-animevideov3-x2.param");
  EXPECT_STREQ(srModelParamName(SRModel::AnimeX3), "realesr-animevideov3-x3.param");
  EXPECT_TRUE(srModelFromId("realistic_hq") == SRModel::RealisticHQ);
  EXPECT_TRUE(srModelFromId("anime_hq") == SRModel::AnimeHQ);
  EXPECT_TRUE(srModelFromId("anime_x2") == SRModel::AnimeX2);
  EXPECT_TRUE(srModelFromId("anime_x3") == SRModel::AnimeX3);
  EXPECT_EQ(srModelScale(SRModel::AnimeX2), 2);
  EXPECT_EQ(srModelScale(SRModel::AnimeX3), 3);
  EXPECT_TRUE(selectSRModel(SRModel::Anime, 1280, 1920) == SRModel::AnimeX2);
  EXPECT_TRUE(selectSRModel(SRModel::Anime, 1280, 2560) == SRModel::AnimeX2);
  EXPECT_TRUE(selectSRModel(SRModel::Anime, 1280, 3840) == SRModel::AnimeX3);
  EXPECT_TRUE(selectSRModel(SRModel::AnimeHQ, 1280, 3840) == SRModel::AnimeHQ);
  EXPECT_TRUE(selectSRModel(SRModel::Realistic, 1280, 3840) == SRModel::Realistic);
}

TEST(EncodeSRInputWidthQuality) {
  EXPECT_EQ(chooseSRInputWidth(1920, 2560, SRInputQuality::Fast), 640);
  EXPECT_EQ(chooseSRInputWidth(1920, 2560, SRInputQuality::Balanced), 1440);
  EXPECT_EQ(chooseSRInputWidth(1920, 2560, SRInputQuality::FullSource), 1920);
  EXPECT_EQ(chooseSRInputWidth(1920, 3840, SRInputQuality::FullSource), 1920);
  EXPECT_EQ(chooseSRInputWidth(1280, 1920, SRInputQuality::FullSource), 1280);
  // 源太大时限制中间帧（避免 2K 源 → 10240 宽）
  EXPECT_EQ(chooseSRInputWidth(2560, 3840, SRInputQuality::FullSource), 1920);
  // 源太小、4x 也不够目标宽时，仍用 target/4 保证输出尺寸
  EXPECT_EQ(chooseSRInputWidth(320, 1920, SRInputQuality::FullSource), 480);
  // 2x/3x：输入按 scale 反推，FullSource 仍优先源宽
  EXPECT_EQ(chooseSRInputWidth(1280, 2560, SRInputQuality::Fast, 2), 1280);
  EXPECT_EQ(chooseSRInputWidth(1280, 1920, SRInputQuality::FullSource, 2), 1280);
  EXPECT_EQ(chooseSRInputWidth(1280, 3840, SRInputQuality::FullSource, 3), 1280);
  EXPECT_EQ(chooseSRInputWidth(1920, 3840, SRInputQuality::FullSource, 2), 1920);
}

// 解码统计视频帧数（用于确认编码没有因 send EAGAIN 丢帧）
static int countVideoFrames(const char* path) {
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return -1;
  if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return -1; }
  int vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vsi < 0) { avformat_close_input(&fmt); return -1; }
  const AVCodec* dec = avcodec_find_decoder(fmt->streams[vsi]->codecpar->codec_id);
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(ctx, fmt->streams[vsi]->codecpar);
  avcodec_open2(ctx, dec, nullptr);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* fr = av_frame_alloc();
  int n = 0;
  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vsi && avcodec_send_packet(ctx, pkt) == 0) {
      while (avcodec_receive_frame(ctx, fr) == 0) n++;
    }
    av_packet_unref(pkt);
  }
  avcodec_send_packet(ctx, nullptr);
  while (avcodec_receive_frame(ctx, fr) == 0) n++;
  av_packet_free(&pkt);
  av_frame_free(&fr);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);
  return n;
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
// 非整数帧率：29.97 源编码后仍应保持 30000/1001，不能被取整成 30fps
TEST(EncodeNonIntegerFps) {
  std::string src = assetPath("test_2997.mp4");
  std::string out = tempOut("encode_2997.mp4");
  std::error_code ec;
  std::filesystem::remove(out, ec);
  EncodeOptions eo;
  eo.verbose = false;
  eo.modelBase = Config::defaults().modelBase;
  EXPECT_TRUE(encodeOffline(src.c_str(), out.c_str(), 320, eo));

  double srcDur = probeDuration(src.c_str());
  EXPECT_NEAR(probeDuration(out.c_str()), srcDur, 0.1);
  EXPECT_EQ(countVideoFrames(out.c_str()), 59);   // 旧实现丢 2 帧（57）

  AVFormatContext* fmt = nullptr;
  EXPECT_TRUE(avformat_open_input(&fmt, out.c_str(), nullptr, nullptr) >= 0);
  if (fmt) {
    avformat_find_stream_info(fmt, nullptr);
    int vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    EXPECT_TRUE(vsi >= 0);
    if (vsi >= 0) {
      // fMP4 的 avg_frame_rate 受时长估算影响，用 r_frame_rate 判断实际帧率
      AVRational r = fmt->streams[vsi]->r_frame_rate;
      double fps = av_q2d(r);
      EXPECT_NEAR(fps, 30000.0 / 1001.0, 0.01);   // 旧实现为 30，会超出容差
      // 视频与音频起点应对齐（关闭 B 帧后不应再有 ~66ms 偏移）
      double vst = fmt->streams[vsi]->start_time != AV_NOPTS_VALUE
        ? fmt->streams[vsi]->start_time * av_q2d(fmt->streams[vsi]->time_base) : 0;
      int asi = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
      if (asi >= 0 && fmt->streams[asi]->start_time != AV_NOPTS_VALUE) {
        double ast = fmt->streams[asi]->start_time * av_q2d(fmt->streams[asi]->time_base);
        EXPECT_NEAR(vst, ast, 0.02);
      }
    }
    avformat_close_input(&fmt);
  }
}

// 源起点非 0（.ts 常见 start_time≈2.8s）：音视频都必须归一到 0，不能保留固定错位
TEST(EncodeStartOffsetNormalized) {
  std::string src = assetPath("test_offset.ts");
  std::string out = tempOut("encode_offset.mp4");
  std::error_code ec;
  std::filesystem::remove(out, ec);
  EncodeOptions eo;
  eo.verbose = false;
  eo.modelBase = Config::defaults().modelBase;
  EXPECT_TRUE(encodeOffline(src.c_str(), out.c_str(), 1280, eo));
  EXPECT_NEAR(probeDuration(out.c_str()), probeDuration(src.c_str()), 0.1);
  EXPECT_EQ(countVideoFrames(out.c_str()), 60);

  AVFormatContext* fmt = nullptr;
  EXPECT_TRUE(avformat_open_input(&fmt, out.c_str(), nullptr, nullptr) >= 0);
  if (fmt) {
    avformat_find_stream_info(fmt, nullptr);
    int vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int asi = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    EXPECT_TRUE(vsi >= 0 && asi >= 0);
    if (vsi >= 0 && asi >= 0) {
      double vst = fmt->streams[vsi]->start_time != AV_NOPTS_VALUE
        ? fmt->streams[vsi]->start_time * av_q2d(fmt->streams[vsi]->time_base) : 0;
      double ast = fmt->streams[asi]->start_time != AV_NOPTS_VALUE
        ? fmt->streams[asi]->start_time * av_q2d(fmt->streams[asi]->time_base) : 0;
      EXPECT_NEAR(vst, 0.0, 0.02);
      EXPECT_NEAR(ast, 0.0, 0.02);
      EXPECT_NEAR(vst, ast, 0.02);
    }
    avformat_close_input(&fmt);
  }
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
