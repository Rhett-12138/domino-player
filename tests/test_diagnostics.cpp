#include "test_common.h"
#include "diagnostics.h"

TEST(DiagnosticsJsonBasic) {
  DiagnosticsInfo d;
  d.version = "0.2.4";
  d.backend = "tensorrt";
  d.mode = "performance";
  d.modeActive = "auto";
  d.modePending = true;
  d.output = "d3d11";
  d.adapter = "NVIDIA GeForce RTX 4070 SUPER";
  d.driverVersion = "31.0.15.5222";
  d.adapterMode = "nvidia";
  d.vendorId = 4318;
  d.memoryMB = 12282;
  d.featureLevel = "11_0";
  d.h264HwDecode = true;
  d.h264Reason.clear();
  d.srInited = true;
  d.targetW = 3840;
  d.zeroCopyActive = true;
  d.gpuSink = true;
  d.cudart = true;
  d.tensorrt = true;
  d.vulkanGpuCount = 1;
  d.fps = 29.8;
  const std::string s = diagnosticsToJson(d);
  EXPECT_TRUE(s.find("\"version\":\"0.2.4\"") != std::string::npos);
  EXPECT_TRUE(s.find("\"backend\":\"tensorrt\"") != std::string::npos);
  EXPECT_TRUE(s.find("\"mode\":\"performance\"") != std::string::npos);
  EXPECT_TRUE(s.find("\"modeActive\":\"auto\"") != std::string::npos);
  EXPECT_TRUE(s.find("\"modePending\":true") != std::string::npos);
  EXPECT_TRUE(s.find("\"zeroCopy\":{\"active\":true") != std::string::npos);
  EXPECT_TRUE(s.find("\"adapter\":\"NVIDIA GeForce RTX 4070 SUPER\"") != std::string::npos);
  EXPECT_TRUE(s.find("\"driver\":\"31.0.15.5222\"") != std::string::npos);
  EXPECT_TRUE(s.find("\"decode\":{\"h264Hw\":true,\"inUse\":false") != std::string::npos);
  EXPECT_TRUE(s.find("\"targetW\":3840") != std::string::npos);
}

TEST(DiagnosticsJsonEscapes) {
  DiagnosticsInfo d;
  d.adapter = "A\"B\\C";
  d.fallbackReason = "cudart/D3D\nfailed";
  const std::string s = diagnosticsToJson(d);
  EXPECT_TRUE(s.find("A\\\"B\\\\C") != std::string::npos);
  EXPECT_TRUE(s.find("cudart/D3D\\nfailed") != std::string::npos);
}
