#pragma once
#include <string>

// 诊断信息是纯数据；JSON 序列化不依赖 GPU/Windows，便于单测。
struct DiagnosticsInfo {
  std::string version = "0.3.0-beta.1";
  std::string backend = "off";      // tensorrt / ncnn / off
  std::string mode = "auto";        // 用户期望 auto / compat / performance
  std::string modeActive = "auto";  // 当前 open 已应用
  bool modePending = false;
  std::string output = "cpu";       // d3d11 / cpu
  std::string adapter;
  std::string driverVersion;
  std::string adapterMode = "auto"; // nvidia / default / auto
  int vendorId = 0;
  int deviceId = 0;
  long long memoryMB = 0;
  std::string featureLevel = "unknown";
  bool h264HwDecode = false;        // D3D11VA 能力，不代表当前已硬解
  bool h264InUse = false;
  std::string h264Reason;
  bool srInited = false;
  bool srFailed = false;
  bool beauty = false;
  int targetW = 0;
  bool zeroCopyActive = false;
  bool gpuSink = false;
  bool gpuFatal = false;
  std::string fallbackReason;
  bool cudart = false;
  bool tensorrt = false;
  int vulkanGpuCount = 0;
  double fps = 0;
};

std::string diagnosticsToJson(const DiagnosticsInfo& info);
