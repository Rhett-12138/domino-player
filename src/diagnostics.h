#pragma once
#include <string>

// 诊断信息是纯数据；JSON 序列化不依赖 GPU/Windows，便于单测。
struct DiagnosticsInfo {
  std::string version = "0.2.4";
  std::string backend = "off";      // tensorrt / ncnn / off
  std::string output = "cpu";       // d3d11 / cpu
  std::string adapter;
  int vendorId = 0;
  int deviceId = 0;
  long long memoryMB = 0;
  std::string featureLevel = "unknown";
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
