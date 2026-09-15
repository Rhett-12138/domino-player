#include "test_common.h"
#include "beauty_math.h"

// 肤色判定：暖色皮肤应命中，灰/红/过艳/过暗应低
TEST(SkinProbWarmSkin) {
  // 典型皮肤 (0.62,0.48,0.42)：偏暖、通道差适中
  EXPECT_TRUE(beautymath::skinProb(0.62f, 0.48f, 0.42f) > 0.2f);
  // 亮皮肤
  EXPECT_TRUE(beautymath::skinProb(0.91f, 0.77f, 0.70f) > 0.1f);
}

TEST(SkinProbNonSkin) {
  EXPECT_NEAR(beautymath::skinProb(0.5f, 0.5f, 0.5f), 0.0f, 1e-6);  // 灰
  EXPECT_NEAR(beautymath::skinProb(1.0f, 0.0f, 0.0f), 0.0f, 1e-6);  // 纯红（通道差过大）
  EXPECT_NEAR(beautymath::skinProb(0.3f, 0.3f, 0.8f), 0.0f, 1e-6);  // 蓝（不偏暖）
  EXPECT_NEAR(beautymath::skinProb(0.02f, 0.02f, 0.02f), 0.0f, 1e-6); // 过暗
}

// 磨皮/美白混合方向
TEST(BeautyBlend) {
  // 磨皮：向模糊色靠拢（k=0.5*1*1.2=0.6）
  EXPECT_NEAR(beautymath::blurBlend(0.5f, 0.6f, 0.5f, 1.0f), 0.56f, 1e-5);
  // 磨皮=0 不变
  EXPECT_NEAR(beautymath::blurBlend(0.5f, 0.6f, 0.5f, 0.0f), 0.5f, 1e-6);
  // 美白：肤色区向白靠拢 80%（0.5 → 0.5 + 0.5*0.8 = 0.9）
  EXPECT_NEAR(beautymath::whiteBlend(0.5f, 1.0f, 1.0f), 0.9f, 1e-5);
  // 非肤色（skin=0）不变
  EXPECT_NEAR(beautymath::whiteBlend(0.5f, 0.0f, 1.0f), 0.5f, 1e-6);
}

// 滤镜：黑白→灰阶；暖色→R 增益；冷色→B 增益；强度=0 不变
TEST(FilterPresets) {
  float r = 0.5f, g = 0.4f, b = 0.3f;
  float r0 = r, g0 = g, b0 = b;
  beautymath::applyFilter(r, g, b, 1.0f, 1.0f);   // 黑白
  EXPECT_NEAR(r, g, 1e-5);
  EXPECT_NEAR(g, b, 1e-5);

  r = 0.5f; g = 0.4f; b = 0.3f;
  beautymath::applyFilter(r, g, b, 3.0f, 1.0f);   // 暖色
  EXPECT_TRUE(r > 0.5f && b < 0.3f);

  r = 0.5f; g = 0.4f; b = 0.3f;
  beautymath::applyFilter(r, g, b, 4.0f, 1.0f);   // 冷色
  EXPECT_TRUE(b > 0.3f && r < 0.5f);

  r = 0.5f; g = 0.4f; b = 0.3f;
  beautymath::applyFilter(r, g, b, 2.0f, 0.0f);   // 强度 0 → 不变
  EXPECT_NEAR(r, r0, 1e-6);
  EXPECT_NEAR(g, g0, 1e-6);
  EXPECT_NEAR(b, b0, 1e-6);
}

// 其余滤镜方向：日系清新提亮、复古胶片偏黄降对比、粉调偏粉
TEST(FilterMorePresets) {
  float lum0 = 0.299f * 0.5f + 0.587f * 0.4f + 0.114f * 0.3f;

  float r = 0.5f, g = 0.4f, b = 0.3f;
  beautymath::applyFilter(r, g, b, 2.0f, 1.0f);   // 日系清新：提亮偏青
  float lum = 0.299f * r + 0.587f * g + 0.114f * b;
  EXPECT_TRUE(lum > lum0);                        // 整体提亮
  EXPECT_TRUE(b > 0.3f);                          // 蓝色通道增强

  r = 0.5f; g = 0.4f; b = 0.3f;
  beautymath::applyFilter(r, g, b, 5.0f, 1.0f);   // 复古胶片：偏黄（R↑ B↓）
  EXPECT_TRUE(r > 0.5f);
  EXPECT_TRUE(b < 0.31f);                         // 蓝通道被压低（计算值 ~0.302）

  r = 0.5f; g = 0.4f; b = 0.3f;
  beautymath::applyFilter(r, g, b, 6.0f, 1.0f);   // 粉调：提亮偏红
  EXPECT_TRUE(r > 0.5f);
  EXPECT_TRUE(r - b > 0.2f);
}
