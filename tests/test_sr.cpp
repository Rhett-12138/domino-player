#include "test_common.h"
#include "sr.h"
#include "trt_sr.h"
#include "config.h"
#include "trt/convert_avx2.h"
#include <ncnn/gpu.h>
#include <intrin.h>
#include <vector>

// SR 冒烟：加载模型并处理一张小图，输出尺寸应为 4 倍
TEST(SrSmoke) {
  Config c = Config::defaults();
  SR sr;
  if (!sr.init(c.srParam().c_str(), c.srBin().c_str())) {
    printf("  SKIP: sr init failed (无 GPU/模型不可用)\n");
    return;
  }
  const int inW = 320, inH = 180;
  std::vector<uint8_t> rgb((size_t)inW * inH * 3, 128);  // 灰色图
  std::vector<uint8_t> out;
  int w = sr.process(rgb.data(), inW, inH, inW, out);
  EXPECT_TRUE(w > 0);
  EXPECT_TRUE(w >= inW * 3);                       // 4x 模型放大（边界裁剪）
  EXPECT_TRUE(out.size() >= (size_t)w * inH * 3);  // 输出像素足够
  sr.clear();
  ncnn::destroy_gpu_instance();  // 释放 Vulkan 全局实例，避免退出时崩溃
}

// TensorRT 是可选后端：模型缺失/无 GPU 时必须安全失败，由调用方回退 NCNN。
TEST(TrtMissingModelFails) {
  TensorRtSR trt;
  EXPECT_FALSE(trt.init("Z:/missing/model.onnx", "Z:/missing/cache", "test", 4, 960, 540));
  EXPECT_FALSE(trt.ready());
}

// CPU 输入转换快路径：AVX2/F16C 存在时 0/255 必须转成精确 half 0/1。
TEST(TrtRgb24ToHalfPlanarAvx2Basic) {
  int regs[4] = {};
  __cpuid(regs, 1);
  const bool f16c = (regs[2] & (1 << 29)) != 0;
  __cpuidex(regs, 7, 0);
  const bool avx2 = (regs[1] & (1 << 5)) != 0;
  if (!f16c || !avx2) {
    printf("  SKIP: AVX2/F16C unavailable\n");
    return;
  }
  const uint8_t src[12] = {
    255, 0, 0,
    0, 255, 0,
    0, 0, 255,
    255, 255, 255,
  };
  uint16_t r[4] = {}, g[4] = {}, b[4] = {};
  trtRgb24ToHalfPlanarAvx2(src, 4, 1, r, g, b);
  EXPECT_EQ(r[0], 0x3C00);
  EXPECT_EQ(g[1], 0x3C00);
  EXPECT_EQ(b[2], 0x3C00);
  EXPECT_EQ(r[3], 0x3C00);
  EXPECT_EQ(g[3], 0x3C00);
  EXPECT_EQ(b[3], 0x3C00);
  EXPECT_EQ(g[0], 0);
  EXPECT_EQ(b[0], 0);
}
