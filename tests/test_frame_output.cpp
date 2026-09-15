#include "test_common.h"
#include "frame_output.h"
#include <cstring>

// 生成一张 w×h 渐变肤色图（r>g>b，随 x 变化以便模糊产生差异）
static std::vector<uint8_t> makeSkin(int w, int h) {
  std::vector<uint8_t> px((size_t)w * h * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int i = (y * w + x) * 3;
      px[i + 0] = (uint8_t)(150 + x);
      px[i + 1] = (uint8_t)(110 + x * 0.6);
      px[i + 2] = (uint8_t)(90 + x * 0.4);
    }
  }
  return px;
}

// 无美颜快路径：presentFrame 后 frame() 应与输入逐字节一致
TEST(FrameOutputFastPath) {
  EngineFrameOutput fo;
  auto px = makeSkin(16, 9);
  fo.presentFrame(px.data(), 16, 9);
  std::lock_guard<std::mutex> lk(fo.lock());
  EXPECT_EQ(fo.width(), 16);
  EXPECT_EQ(fo.height(), 9);
  EXPECT_TRUE(fo.hasNewFrame());
  EXPECT_EQ(memcmp(fo.frame(), px.data(), 16 * 9 * 3), 0);
  EXPECT_EQ(fo.previewCount(), 0);   // 未生成预览
}

// 美颜生效：设置磨皮+美白后输出应与输入不同（肤色渐变区被模糊/提亮）
TEST(FrameOutputBeauty) {
  EngineFrameOutput fo;
  EngineBeautyParams bp;
  bp.smooth = 0.8f;
  bp.white = 0.3f;
  bp.filterIdx = 0;
  fo.setBeautyParams(bp);
  auto px = makeSkin(64, 36);
  fo.presentFrame(px.data(), 64, 36);
  std::lock_guard<std::mutex> lk(fo.lock());
  EXPECT_TRUE(fo.width() == 64 && fo.height() == 36);
  EXPECT_TRUE(memcmp(fo.frame(), px.data(), 64 * 36 * 3) != 0);
}

// 双缓冲交换：连续两帧不同内容，frame() 应反映最新帧
TEST(FrameOutputSwapLatest) {
  EngineFrameOutput fo;
  std::vector<uint8_t> a(48 * 27 * 3, 10), b(48 * 27 * 3, 200);
  fo.presentFrame(a.data(), 48, 27);
  fo.presentFrame(b.data(), 48, 27);
  std::lock_guard<std::mutex> lk(fo.lock());
  EXPECT_EQ((int)fo.frame()[0], 200);
  EXPECT_EQ(memcmp(fo.frame(), b.data(), 48 * 27 * 3), 0);
}

// 滤镜缩略图：数量正确、尺寸正确、黑白滤镜下各像素 r==g==b
TEST(FrameOutputPreviews) {
  EngineFrameOutput fo;
  auto px = makeSkin(320, 180);
  fo.presentFrame(px.data(), 320, 180);
  fo.makeFilterPreviews(7);
  EXPECT_EQ(fo.previewCount(), 7);
  EXPECT_TRUE(fo.previewWidth() > 0 && fo.previewHeight() > 0);
  EXPECT_TRUE(fo.previewWidth() <= px.size() / 3 / fo.previewHeight());  // 小图
  // 原图预览 = 基础图（非空、内容合理）；黑白滤镜预览应灰阶
  const uint8_t* bw = fo.previewBuffer(1);
  const uint8_t* orig = fo.previewBuffer(0);
  EXPECT_TRUE(orig != nullptr && bw != nullptr);
  int n = fo.previewWidth() * fo.previewHeight();
  bool gray = true, differs = false;
  for (int i = 0; i < n; i++) {
    if (bw[i * 3 + 0] != bw[i * 3 + 1] || bw[i * 3 + 1] != bw[i * 3 + 2]) { gray = false; break; }
    if (memcmp(&bw[i * 3], &orig[i * 3], 3) != 0) differs = true;
  }
  EXPECT_TRUE(gray);        // 黑白滤镜 → 灰阶
  EXPECT_TRUE(differs);     // 与原图有差异
}
