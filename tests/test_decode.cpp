#include "test_common.h"
#include "decode.h"
#include <vector>

TEST(DecodeOpenBasic) {
  MediaStream ms;
  EXPECT_TRUE(ms.open(assetPath("test_av.mp4").c_str()));
  EXPECT_EQ(ms.vw, 1280);
  EXPECT_EQ(ms.vh, 720);
  EXPECT_NEAR(ms.duration, 2.0, 0.3);
  EXPECT_TRUE(ms.hasAudio);
  EXPECT_TRUE(ms.fps > 0 && ms.fps < 120);
  ms.close();
}

TEST(DecodeFrames) {
  MediaStream ms;
  EXPECT_TRUE(ms.open(assetPath("test_av.mp4").c_str()));
  std::vector<uint8_t> rgb((size_t)ms.vw * ms.vh * 3);
  double pts = -1;
  int frames = 0;
  int r = 0;
  while ((r = ms.nextVideoFrame(rgb.data(), &pts)) == 0) {
    frames++;
    // 帧内容非全零（解码出了真实像素）
    if (frames == 10) {
      bool nonzero = false;
      for (int i = 0; i < 64; i++) if (rgb[i] != 0) { nonzero = true; break; }
      EXPECT_TRUE(nonzero);
    }
    if (frames > 5000) break;  // 保护
  }
  EXPECT_EQ(r, -1);  // 正常读到 EOF
  EXPECT_TRUE(frames > 40 && frames < 100);  // 2s@30fps ≈ 60 帧
  ms.close();
}

TEST(DecodeSeek) {
  MediaStream ms;
  EXPECT_TRUE(ms.open(assetPath("test_av.mp4").c_str()));
  std::vector<uint8_t> rgb((size_t)ms.vw * ms.vh * 3);
  double pts = -1;
  EXPECT_TRUE(ms.seek(1.0));
  EXPECT_EQ(ms.nextVideoFrame(rgb.data(), &pts), 0);
  EXPECT_TRUE(pts >= 0.0 && pts <= 1.8);  // BACKWARD seek 可能落在目标前的关键帧
  ms.close();
}

TEST(DecodeOpenMissing) {
  MediaStream ms;
  EXPECT_FALSE(ms.open(assetPath("no_such_file.mp4").c_str()));
  ms.close();  // 未 open 时 close 应安全
}
