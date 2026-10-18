#include "config.h"
#include "player.h"
#include "frame_output.h"
#include "audio_interface.h"
#include "decode.h"
#include "encode.h"
#include "sr.h"

#include <ncnn/gpu.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

static double nowSec() {
  return std::chrono::duration<double>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  const std::string assets = argc > 1 ? argv[1] : "tests/assets";
  const std::string src = assets + "/test_long.mp4";
  const std::string offlineSrc = argc > 2 ? argv[2] : assets + "/test_av.mp4";
  const int offlineTarget = argc > 3 ? atoi(argv[3]) : 1920;
  Config cfg = Config::defaults();

  // 1) 直接测 SR 推理：720p 源 → 目标宽 / 4 的模型输入
  SR sr;
  if (!sr.init(cfg.srParam().c_str(), cfg.srBin().c_str())) {
    printf("SR init failed (model/GPU unavailable)\n");
    return 1;
  }
  MediaStream ms;
  if (!ms.open(src.c_str())) { printf("open source failed: %s\n", src.c_str()); return 1; }
  std::vector<uint8_t> rgb((size_t)ms.vw * ms.vh * 3);
  double pts = 0;
  if (ms.nextVideoFrame(rgb.data(), &pts) != 0) { printf("decode failed\n"); return 1; }

  printf("direct SR (%dx%d source):\n", ms.vw, ms.vh);
  printf("  target  inWxInH      ms/frame   theory fps\n");
  for (int target : { 1920, 2560, 3840 }) {
    const int inW = target / 4;
    std::vector<uint8_t> out;
    for (int i = 0; i < 3; i++) sr.process(rgb.data(), ms.vw, ms.vh, inW, out);
    const int iters = 20;
    double t0 = nowSec();
    int ow = 0;
    for (int i = 0; i < iters; i++) ow = sr.process(rgb.data(), ms.vw, ms.vh, inW, out);
    double dt = nowSec() - t0;
    double msPer = dt * 1000.0 / iters;
    printf("  %5d   %4dx%-4d   %8.2f   %8.2f   (out %d)\n", target, inW,
           (ms.vh * inW / ms.vw) & ~1, msPer, 1000.0 / msPer, ow);
  }
  sr.clear();
  ms.close();

  // 2) 离线质量档：同一 720p 源，比较 Fast/Balanced/FullSource 输入
  printf("\nOffline encode (2s -> %d, CRF18):\n", offlineTarget);
  printf("  mode       time(s)   out MB   path\n");
  const std::string shortSrc = offlineSrc;
  struct ModeInfo { SRInputQuality q; const char* name; };
  const ModeInfo modes[] = {
    { SRInputQuality::Fast, "fast" },
    { SRInputQuality::Balanced, "balanced" },
    { SRInputQuality::FullSource, "full" },
  };
  for (const auto& m : modes) {
    EncodeOptions eo;
    eo.verbose = false;
    eo.modelBase = cfg.modelBase;
    eo.srInput = m.q;
    std::string out = (std::filesystem::temp_directory_path() /
      ("domino_offline_" + std::string(m.name) + ".mp4")).string();
    double t0 = nowSec();
    bool ok = encodeOffline(shortSrc.c_str(), out.c_str(), offlineTarget, eo);
    double dt = nowSec() - t0;
    std::error_code ec;
    auto sz = std::filesystem::file_size(out, ec);
    printf("  %-9s  %7.2f   %6.1f   %s\n", m.name, dt,
           ec ? 0.0 : sz / (1024.0 * 1024.0), out.c_str());
    if (!ok) printf("  ^ encode failed\n");
  }

  // 3) 端到端 Player：解码 + SR + 呈现（无 CPU 美颜）
  printf("\nPlayer end-to-end (3.0s each, dummy audio, CPU beauty off):\n");
  printf("  target  presented  fps\n");
  for (int target : { 1920, 2560, 3840 }) {
    EngineFrameOutput out;
    Player p(out);
    p.setAudioSink(std::make_unique<DummyAudioSink>());
    p.setConfig(cfg);
    p.setAutoDegradeEnabled(false);   // 基准测指定档位，不自动降档
    p.setSRTarget(0, target);
    p.setNoSR(false);
    if (!p.open(src.c_str())) {
      printf("  %5d   open failed\n", target);
      continue;
    }
    // SR 在 VideoWorker 里异步初始化
    for (int i = 0; i < 200 && !p.srReady() && !p.srFailed(); i++) {
      p.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!p.srReady()) {
      printf("  %5d   SR unavailable (failed=%d)\n", target, p.srFailed() ? 1 : 0);
      continue;
    }
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 3.0) {
      p.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    long long frames = p.frameIdx();
    printf("  %5d   %9lld   %6.2f\n", target, frames, frames / dt);
    p.close();
  }

  ncnn::destroy_gpu_instance();
  return 0;
}
