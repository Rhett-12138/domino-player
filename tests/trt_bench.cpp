#include "config.h"
#include "decode.h"
#include "trt_sr.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static double nowSec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  const std::string assets = argc > 1 ? argv[1] : "tests/assets";
  Config cfg = Config::defaults();
  TensorRtSR trt;
  const std::string onnx = cfg.srOnnx(SRModel::Realistic);
  if (!trt.init(onnx.c_str(), cfg.trtCacheDir().c_str(), "realistic", 4, 1920, 1080)) {
    printf("TRT init failed: %s\n", onnx.c_str());
    return 1;
  }
  MediaStream ms;
  if (!ms.open((assets + "/test_long.mp4").c_str())) { printf("open failed\n"); return 1; }
  std::vector<uint8_t> rgb((size_t)ms.vw * ms.vh * 3);
  double pts = 0;
  if (ms.nextVideoFrame(rgb.data(), &pts) != 0) return 1;
  printf("source %dx%d\n", ms.vw, ms.vh);
  for (int target : {1920, 2560, 3840}) {
    std::vector<uint8_t> out;
    int inW = target / 4;
    for (int i = 0; i < 3; i++) trt.process(rgb.data(), ms.vw, ms.vh, inW, out);
    const int iters = 20;
    double t0 = nowSec();
    int ow = 0;
    for (int i = 0; i < iters; i++) ow = trt.process(rgb.data(), ms.vw, ms.vh, inW, out);
    double msPer = (nowSec() - t0) * 1000.0 / iters;
    printf("target %d in %d ms/frame %.2f out %d\n", target, inW, msPer, ow);
  }
  trt.clear();
  ms.close();
  return 0;
}
