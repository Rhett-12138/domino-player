#include "test_common.h"
#include "config.h"

// 配置：默认值应可用，模型路径拼接正确
TEST(ConfigDefaultsUsable) {
  Config c = Config::defaults();
  EXPECT_TRUE(c.modelBase.size() > 4);          // 仓库内模型目录
  EXPECT_TRUE(c.dataRoot.size() > 0);
  EXPECT_TRUE(c.srParam().find("RealESRGAN_General_x4_v3.param") != std::string::npos);
  EXPECT_TRUE(c.srBin().find("RealESRGAN_General_x4_v3.bin") != std::string::npos);
}

// 注入路径：srParam/srBin 基于 modelBase 拼接（末尾有无分隔符都应得到合法路径）
TEST(ConfigPathInjection) {
  Config c;
  c.modelBase = "C:/models/";
  EXPECT_STREQ(c.srParam().c_str(), "C:/models/RealESRGAN_General_x4_v3.param");
  EXPECT_STREQ(c.srBin().c_str(), "C:/models/RealESRGAN_General_x4_v3.bin");

  // 前端 Dart 传入的目录通常不带分隔符，必须自动补全
  c.modelBase = "C:/models";
  EXPECT_STREQ(c.srParam().c_str(), "C:/models/RealESRGAN_General_x4_v3.param");
  EXPECT_STREQ(c.srBin().c_str(), "C:/models/RealESRGAN_General_x4_v3.bin");

  c.modelBase = "C:\\models";
  EXPECT_STREQ(c.srParam().c_str(), "C:\\models/RealESRGAN_General_x4_v3.param");
  EXPECT_STREQ(joinModelPath("", "x.bin").c_str(), "x.bin");
}

TEST(ConfigTrtModels) {
  Config c;
  c.onnxBase = "C:/onnx";
  EXPECT_STREQ(c.srOnnx(SRModel::Realistic).c_str(),
               "C:/onnx/realesr-general-x4v3-fp16.onnx");
  EXPECT_STREQ(c.srOnnx(SRModel::RealisticHQ).c_str(),
               "C:/onnx/realesrgan-x4plus-fp16.onnx");
  EXPECT_TRUE(c.srOnnx(SRModel::Anime).empty());
  EXPECT_TRUE(srModelSupportsTrt(SRModel::Realistic));
  EXPECT_TRUE(srModelSupportsTrt(SRModel::RealisticHQ));
  EXPECT_FALSE(srModelSupportsTrt(SRModel::AnimeX2));
  EXPECT_STREQ(c.trtCacheDir().c_str(), "trt-rtx");
}
